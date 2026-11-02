# 059 · Windows game-thread profile: the 34.6 % in ntdll is the pacing sleep, not a spin

- **Date:** 2026-10-28
- **Task:** T-761
- **Status:** open (the ntdll lead is explained and is not the slowdown; the cause of the 1.6 fps is still to be pinned)

## Symptom
Speed check on the owner's PC (`tools/run/windows/fight_compare.ps1`, ougi fight, 3 runs, same state hash): old monolithic
63fc1b6 59.0 / 59.1 / 59.1 fps worst 10 s window, new zip 9dc800e (module at -O2) 57.0 / 57.5 / 57.9: FAIL by ~1.6 fps.
A game-thread profile on that PC (T-759 sampler, `DON_PROFILE`, exe built from main 38cc0fe without FFmpeg + the -O2 DLL,
130 s fight, 68158 samples, 0 lost, thread CPU 79.9 s of 130.4 s) gave, by module:

```
23587 34.6%  Battle Stadium DON.exe     23550 34.6%  ntdll.dll     11642 17.1%  don_game.dll
 3178  4.7%  (outside modules)           3020  4.4%  don_spu2.dll   2794  4.1%  VCRUNTIME140.dll
```

## Investigation
**Symbolizing.** The profiled exe was gone with T-759's worktree, so it was rebuilt from 38cc0fe
(`tools/pipeline/build_windows.sh module-exe`, 60 s) and relinked with the recorded link line plus `/MAP`. Layout check
against the profile: `SizeOfImage` 0x2655000 (40194048) equals the exe's mapped span in the profile
(0x7ff616ce0000..0x7ff619335000); `.text` VirtualSize 0x2A3B66, entry 0x2A39E4. The SHA-256 differs from the profiled
exe's (57bdd53e… vs 792a4958…; link timestamp, no `/Brepro`), so the exe symbol names come from a relink proven by size
and entry, not byte for byte;
the names below are coherent (every top address lands in a function that the code explains). ntdll and VCRUNTIME140 were
resolved with the nearest export below the address from the PC's own copies (their `SizeOfImage` equals the profile's
module span: ntdll 0x1f8000, VCRUNTIME140 0x2c000). `tools/verify/profile_summary.py` was not changed: the one-off script
lived in `work/t761/` (PE exports with `pefile`, lld map parser).

**Symbolized top (share of all 68158 samples):**

| samples | share | module | function |
|---:|---:|---|---|
| 22731 | 33.4 % | ntdll.dll | `ZwWaitForAlertByThreadId` (the park of every SRW / condition-variable / WaitOnAddress wait) |
| 11642 | 17.1 % | don_game.dll | (no symbols here; two 4 KB pages hold 2320 and 2137 of them) |
| 10742 | 15.8 % | exe | `EeScheduler::run` (inlined dispatcher and checkpoint, includes the `$ehgcr` regions) |
| 4316 | 6.3 % | exe | `EeScheduler::accountCycles` (out of line; its two callers are inside `run`/`waitForEvent`) |
| 3178 | 4.7 % | (outside modules) | one region 0x7ff9b0510000..0x7ff9b05c0000, not in the module list (unidentified) |
| 3020 | 4.4 % | don_spu2.dll | (no symbols here) |
| 2514 | 3.7 % | exe | `PS2Memory::processVIF1Data` |
| 1880 | 2.8 % | VCRUNTIME140 | `memchr` (the game's libc `memchr` stub, `Stubs/LibC.cpp`, or `std::find` on bytes) |
| 867 | 1.3 % | VCRUNTIME140 | `memmove` |
| 865 | 1.3 % | exe | `iop::detail::IopCpuCore::executeDecoded` |
| 823 | 1.2 % | exe | `iop::detail::IopKernel::beginNextReady` |
| 394 | 0.6 % | exe | `IopEmulator::Impl::runCpu` |
| 280 | 0.4 % | exe | `don_diag::dumpState` |
| 259 | 0.4 % | exe | `IopEmulator::Impl::tryNative` |
| 257 | 0.4 % | exe | `iop::detail::IopKernel::nextWakeCycle` |
| 231 | 0.3 % | don_vu.dll | (no symbols here) |
| 180 | 0.3 % | exe | `PS2Memory::writeIORegister` |
| 161 | 0.2 % | ntdll.dll | `RtlAllocateHeap` |
| 159 | 0.2 % | ntdll.dll | `ZwDelayExecution` (`Sleep`) |
| 126 | 0.2 % | exe | `IopMemory::decoded` |

Everything else in ntdll is below 0.1 %: no `NtYieldExecution`, no `RtlEnterCriticalSection`, no
`RtlQueryPerformanceCounter` among the top rows. **One function is 96.5 % of ntdll: `ZwWaitForAlertByThreadId`.**

**What it is.** On Windows 10/11 a blocked `std::condition_variable`, a contended `std::mutex` and `std::atomic::wait` all
park in that syscall. A spin or a yield would show `NtYieldExecution` / `SwitchToThread`; a `Sleep` shows
`ZwDelayExecution` (0.2 %). So the game thread is *parked*, not spinning. Where the game thread parks (source, current
main, unchanged runtime):

1. `EeScheduler::waitForEvent` (`third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` ~2240-2300):
   `m_eventCv.wait_until(lock, hostDeadline, …)` is the real-time pacing of the game ("the video is the clock",
   `host.cpp` ~1138): when every guest thread waits, the scheduler sleeps until the host time of the next event
   (the vertical blank). `processDueDeadlines` (~2038) waits the same way. The time is counted in `pacingWaitNs()`.
2. `GsThread::reserve` / `GsThread::wait_for_space` (`src/host/gs_thread.cpp` ~96-135): the GS ring is full, the producer
   (game thread) spins 2000 `_mm_pause` and then parks on `m_producer_cv` (1 ms poll) or `m_read.wait`.
3. `GlueGs::vsync` (`gs_glue.cpp` 338) "waits if the GS is behind", and the audio pacing hook (`host.cpp` 1142-1166,
   `sleep_for(1ms)`, only for `DON_AUDIO_PACING=wait`; the default is `rate`, which does not sleep).

Only (1) is idle time that costs the game nothing; (2) and (3) are back-pressure from another thread and mean the game
thread lost time. The profile cannot tell them apart (it keeps the instruction pointer only, no stack).

**Why 33 % is not 33 % of CPU.** The Windows sampler (`profile_win32.cpp`) stores a sample at each 1 ms tick in which the
thread's cycle counter advanced *at all* since the previous sample, then suspends the thread and reads its RIP. A thread
that wakes for a few microseconds (an IOP or timer interrupt, the blank) and parks again inside the same tick is found
parked in `ZwWaitForAlertByThreadId` and counted as a whole sample. 22731 wait samples = 17 % of the 130363 nominal ticks
= about 175 wake-ups per second (an estimate, not measured; the game has 60 blanks per second, so about three wake-ups per frame). The
thread's real CPU was 61 % (79.9 s / 130.4 s): the non-wait samples (45427) are the CPU work; the wait samples are a count
of *ticks with a wake-up*, not time spent burning a core. Nothing here shows a busy loop.

**Is it new since 63fc1b6?** The code that parks is not: `git log 63fc1b6..main` on the patches shows none that touches
`EeScheduler` waiting or `gs_thread.cpp` (the last change of `gs_thread.cpp` is 98f24a5, 2026-10-18, before 63fc1b6; the
patches since are 0086 IOP ready order, 0087 hot state, 0088-0092 module ABI / recompiler CLI). What changed is the module
boundary and the -O level, which a wait-sample count cannot see.

**Pacing counters on the PC.** `DON_PACING_LOG=6000` on the same PC during fight blanks (mean per blank):

| build | work per blank | pacing wait | renderer |
|---|---:|---:|---:|
| old monolithic 63fc1b6 | 14.29 ms | 2.37 ms | ~0.01 ms |
| module 9dc800e (`-O2`) | 14.96 ms | 1.70 ms | ~0.01 ms |

The new build does about 0.67 ms more work per blank and waits about 0.67 ms less. The renderer share is the same in both,
so the GS side is not the difference. This supports the reading above: ntdll is the parked pacing wait, and the 1.6 fps
is extra game-thread work. Which work (exe, `don_game.dll` or the VC runtime) is not split by these counters.

## Cause
Not determined for the 1.6 fps. Determined: the ntdll share is the game thread's parked wait (pacing and/or back-pressure),
sampled with a bias that makes short wake-ups look like time; it is not a spin, a yield, a `Sleep(0)`, a lock convoy or
heap traffic (`RtlAllocateHeap` 0.2 %). Real CPU consumers of the game thread: exe 34.6 % (`EeScheduler::run` 15.8 % and
`accountCycles` 6.3 % together 22 %: the checkpoint and the IOP batch it flushes), `don_game.dll` 17.1 %, SPU2 4.4 %,
VIF1 3.7 %, CRT `memchr`/`memmove` 4.1 %, 4.7 % in an unidentified region.

## Fix proposed (not done) and A/B for the PC
No code change is justified by this profile. To find where the 1.6 fps went, measure the same fight on both builds with
the counters that already exist, and compare:

1. Wait split, no new code: run each build (63fc1b6 monolith and 9dc800e) through `fight_compare.ps1` with
   `DON_PACING_LOG=1`. It prints `[don] pacing mean per blank: work X ms (GS transfers Y) + renderer Z + pacing wait W`
   (`src/host/pacing_stats.h`). `W` is the idle sleep of case (1), `Z` the GS/vsync wait of cases (2)-(3), `X` the game
   thread's work. A module build that is slower at equal speed shows a larger `X` and a smaller `W`; if `Z` differs, the
   cause is the GS side, not the DLL.
2. Same profile on the monolith: build the monolith exe with the T-759 sampler (the T-755 static build, journal 056, plus
   `profile_win32.cpp`) and run the same 130 s fight with `DON_PROFILE`; compare per-symbol counts. The difference between
   the two builds is the answer; this single profile has no baseline. Symbolize both with the recipe above (the relink
   with `/MAP` takes a minute).
3. If `X` is larger in the module build, look at `EeScheduler::run` + `accountCycles` + `don_game.dll` first: in the
   module the dispatcher cannot be inlined into the generated code across the DLL boundary.

## Result
Table and top 20 above; the culprit named; no fix applied. The run of the PC for the A/B is for the orchestrator.
