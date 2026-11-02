# 038 · "NOW LOADING" stutters: the game thread waits for the disk

- **Date:** 2026-10-14
- **Task:** T-214 (follow-up)
- **Status:** resolved (patch 0067, in the series)

## Symptom
Blanks of 80 to 130 ms with the game thread using 3 ms of CPU around the loading screens (work/t211, work/t212: blank 3691 128.72 ms, 2362 116.86 ms).

## Classification
Pacing, host I/O (not a runtime or recompiler fault).

## Investigation
- Route `main_mode_match2` cut at blank 4000 (en-jpvoice, `DON_PLAY_MOVIES=1`, `DON_PACING_LOG=1`), warm cache: max 41 to 43 ms, no blank near 100 ms (3 runs). So the stall depends on the host's state.
- The data lives on `/dev/md127` (rotational drives). After evicting the disc files from the page cache (`posix_fadvise DONTNEED`, `work/t213/evict.py`) the same route gives max 116.69 ms (blank 2461), 98.73, 79.57 ms.
- `strace -f -T -e trace=read` on the game thread: reads of 8 KiB to 1 MiB of the disc's packed files (`chr/nar/0000.seq` 411648 bytes: 53 ms; `font/1100_ps2.tx` 1 MiB: 26 ms; TOC reads 9 to 17 ms), each blocking the thread; the runtime has no asynchronous path (PS2Vfs `fread`, IopHost `fread`, both synchronous). Futex, poll and sleep calls were all short.
- Ruled out: GS transfers, renderer, pacing wait (all ~0 in those blanks).

## Cause
Synchronous host reads of files that are not in the page cache: the first read of each piece waits for the disk (seek plus transfer).

## Fix
Patch 0067: `ps2_file_prefetch::start(path)` (new header `runtime/ps2_file_prefetch.h`), called when PS2Vfs opens a file read-only and when the IOP host opens a file: a detached thread reads the file once front to back (not files under 256 KiB, once per path), so the game thread's reads hit the cache. `DON_NO_PREFETCH=1` disables it. It does not change what the guest sees, only when the disk is touched.

## Verification
Cold cache, same route: before max 116.69 ms, p99.9 61.63 ms, 5 blanks over 50 ms; after max 45.11 ms, p99.9 38.96 ms, 0 over 50 ms (one run each, so the size of the gain is indicative; the cause was measured with strace). `tests/runtime/file_prefetch_test`. Regression: see the task report.

## Temporary diagnostics
A timer around `readHostFile` (never printed: that path is not the slow one); removed.

## Follow-up 2026-10-15 (T-306): in a window, the owner's conditions: patch 0067 does not cover the packed files

**Set-up.** `build/player/don-linux/don` (patch 0067 included) in a window on the private desktop (`tools/run/headless_desktop.sh`, Vulkan, RX 6800 XT), `en-jpvoice` variant folder, sound on through SDL's `dummy` driver (`SDL_AUDIODRIVER=dummy`; the private session has no sound server), route title -> Start -> main menu -> character select -> NOW LOADING -> match (`work/t306/route.txt`, 5,399 blanks, no intermediate screenshots), `DON_PACING_LOG=1`. Cold = `posix_fadvise(DONTNEED)` on the variant folder and `disc/` (`work/t306/evict.py`, checked with `fincore`: 0 bytes resident), warm = same run right after. Two false starts were corrected: `--wav` writes the WAV synchronously on the game thread to a disk other agents were using (sound writes of 270 to 730 ms), and `--shots` every 60 blanks (and scripted shots) cost 40 to 77 ms each; both removed. The machine was loaded by other agents (load 4 to 7, md127 at 99 % utilization at times), so single numbers are indicative.

**Result (late blanks > 20 ms of 5,399; worst blank):** warm 44, 83, 49 (max 48, 53, 51 ms; the worst is the final screenshot at blank 5351); cold 235, 148, 412 (max 177, 144, 115 ms). Cold is worse in 3 runs of 3.

**Classification of the worst cold blanks** (all "work" with GS transfers 0 to 4 ms, renderer 0.2 ms, pacing wait 0, i.e. the game thread blocked, not busy; `strace -f -T` of the game thread, cold): 3360 (108, 162 ms, character select confirmed, NOW LOADING), 4769 (177, 116 ms), 4579 (110), 4108 (144), 4013 (65), 4670 (44 to 67), 2334 (92; also in warm runs: a GS transfer of 28 to 33 ms, not disk), 3074/3135/3175 (170 to 230 ms, title Start area). Reads of the game thread in the cold trace: BGM.AFS 16 KiB: **149.5 ms**; DATA.AFS 1 MiB 33 ms; SE.AFS 128 KiB 30 ms, 21 ms (x3); BGM.AFS 50 KiB 21 ms (x5); DATA.AFS 8 KiB 21 and 18 ms; about 25 reads of 10 to 150 ms in a run, against at most 1.1 ms warm. Process `read_bytes` for the cold run: 20 MB in the whole route, in small pieces, 11 MB at blank ~3837 (NOW LOADING). The rest of the slow blanks in cold: pacing wait of 100 to 500 ms and renderer of 40 to 430 ms in single blanks (8 of them), which are the private compositor under load, not the game; and 20 to 28 ms ones with GS transfers 4 to 6 ms (heavier fight scenes), also seen warm.

**Cause.** The game opens DATA.AFS, BGM.AFS and SE.AFS over and over and reads 8 KiB to 1 MiB pieces at scattered offsets. Patch 0067 starts a front-to-back reader per path on the first open, but on a rotational RAID that reader competes with the game's own scattered reads (seeks) and reaches an offset after the game does; only 20 MB of the 434 MB had been read in the route. Each first read of a piece waits 20 to 150 ms.

**Fix.** `src/host/folder_warmup.h`, called from `host.cpp` before `loadELF`: a detached thread reads every file under the ELF's folder (symlinks followed, MOVIE.AFS last, smaller files first) once, so the page cache is full within a few seconds of start (434 MB read in under 8 s cold here, `fincore` confirmed). `DON_NO_WARMUP=1` disables it; `DON_HOST_LOG=1` prints a summary line. Nothing the guest sees changes. Cost: up to about 430 MB of page cache (reclaimable) and one sequential pass of disk at start.

**Measured after (cold, same route).** The player was **not rebuilt** (no safe build in the time box with other agents' builds running); the same sequence was run by a separate process started with the game (`/tmp/wt`, the header compiled alone: same code, same order): late blanks 20 and 31 (max 64 and 51 ms; the worst is the end-of-route screenshot, 51 to 54 ms) in two runs, one run disturbed by the machine (814 late, a 1 s blank: another agent's disk and CPU load, not repeated). So cold with warm-up is the same as warm. To close: rebuild the player with `src/host/folder_warmup.h` and repeat once.

## Follow-up 2026-10-15 (T-306, second look): the game's own frame loop pauses 0.8 to 2 s, with the host on time

**Not a host stall.** `build/player/don-linux/don`, route `main_mode_match2` cut at blank 3700 (the regression script minus its shots and with `quit`), `--headless --renderer software --sound off`, `DON_PLAY_MOVIES=1 DON_TRACE_FRAMES=1 DON_TRACE_THREADS=1 DON_TRACE_IOP=1 DON_PACING_LOG=1`, output stamped with wall-clock time (`work`-free: scratch in /tmp). Pacing: 3,699 blanks, max 25.04 ms, none over 33 ms. The vsync handler (0x138EE0) keeps its 60 Hz, but the frame loop's counter (0x138B00 calls, printed every 60) does not:

| window (blanks) | what the screen does | 60 game frames took | pause |
|---|---|---|---|
| 2400-2460 | title Start (`GsSetCrt` frameMode 1 -> 0 at 40.43 s) | 1.78 s | about 0.8 s |
| 2880-2940 | main menu | 2.02 s | about 1.0 s |
| 3060-3180 | main menu -> character select | 3.00 s | about 2.0 s |
| 3540-3660 | NOW LOADING | 2.73 s | about 1.7 s |

(Normal: 60 frames per 1.0 s; the game does not catch up afterwards, it loses that time.)

**What the EE main thread is doing (DON_TRACE_THREADS, one dump per 60 vsyncs, identical at 2400, 2460, 2520, 2580):** thread 1 (main, priority 0) is `ready` with pc = ra = 0x0031587C and **no EE thread is running** (`running=0`); thread 5 (priority 25, entry 0x314850) is `waitsusp` at 0x10C1C8; threads 3 and 4 wait on a semaphore as usual. 0x31587C is the return point of `jal 0x314A60` in 0x3157E8, in the CRI sound library (0x314xxx-0x315xxx: `ReferThreadStatus` of a library thread and `WakeupThread` when it reads a wait, the code that patch 0068 is about). So the main thread is not waiting in `sceSifCallRpc`, a CD wait or a named semaphore in those windows; it is inside the sound library's thread-status poll. The IOP is not starved: +37.0 to 37.2 M cycles per second (real time) in the stall and out of it, 10 to 19 M instructions per second (14 M normally). The game thread of the host is not idle: /proc samples every 0.3 s give 20 to 60 % CPU in the stall (normal 37 %), with futex waits in between. The CD side cannot be the delay by itself: the runtime completes an IOP CD command in 128 cycles; only the CD *stream* (movies) is paced by sectors per second.

**Not done (time box of 75 minutes reached):** the sub-second view (what each blank of a window does, which RPC server and function the IOP answers, how long it takes) needs the guest call stack of the main thread; `gdb -p` is refused here (`ptrace_scope`), and a gdb-launched run with a SIGUSR2 sampler produced no samples (the Python stop handler never fired in batch mode; script left in /tmp/stall, not versioned). The reference emulator was **not** run, so whether the console's frame counter also pauses at Start / NOW LOADING is **unmeasured**; the six savestates only give the state at a moment, not a duration. No fix was made and the build/runtime were not touched.

**Next steps (in order):** (1) a temporary diagnostic in the scratch runtime clone (patches/ps2recomp untouched): at every vsync print thread 1's status and, in the sound library's poll, the host time per call of `ReferThreadStatus` / `WakeupThread` / `SleepThread` and the number of calls in the pause; (2) the console: from `01-title.p2s` (`drive.py loadstate`), press Start and grab screenshots at 10 per second, comparing when the menu transition appears with the port's 0.8 s; (3) if the port's poll is slower than the console's (patch 0068 reports READY for pacing waits; the caller then loops instead of sleeping), look at what makes the library's thread wake: patch 0071 (SPU2 DMA end time) and 0072 (wait before handlers) are the first suspects for a thread that never reaches its wake condition until a timeout.

## Follow-up 2026-10-15 (T-214, third look): the scheduler is not holding main back; the pause is a fixed ~5 ms-tick wait of the sound driver

**Set-up.** Scratch clone of the runtime (`work/t214-sched`, own build `build/t214-sched`; shared tree, series and `build/linux-dev` untouched) with a temporary log: one line per blank (thread table with status, priority, wait reason and pc; picks, idle entries and host ms spent in `waitForEvent`; EE dispatches) and an IOP log of SleepThread/WakeupThread/semaphore calls with the IOP cycle. Route `main_mode_match2` cut at blank 3700 (same as the previous look), `DON_PLAY_MOVIES=1`, software renderer, sound off. Both logs removed; the scratch clone and its build deleted.

**1. The scheduler dispatches main as soon as it can run.** In the pause (blanks 2430-2449) thread 1 is **Waiting, reason VSync, pc 0x37A310** (not READY at 0x31587C: that earlier dump was a snapshot taken at another moment, so the "ready thread with nothing running" reading is withdrawn). `run()` picks it at every blank (`picks` +1, completeVSync makes it ready, EE idle 14-19 ms per blank in `waitForEvent` until the next blank's deadline, as designed). 0x37A308/0x37A310 is the game's sound-request wait loop (journal 034): `sceGsSyncV(0)`, the SFSV frame call, `0x377A90(slot) == 2`. The loop runs once per blank until the slot reaches state 2, so the frame loop (0x138B00) cannot run: the game itself waits.

**2. How long, and what the IOP does meanwhile.** Idle (main not running) blank runs: 2430-2449 (20 blanks, title Start), 3539-3562, 3578-3601, 3632-3643 (24 + 24 + 12, NOW LOADING), also 2380-2401 before Start. IOP in the 2430-2449 window: 67 wakeups of IOP thread 13 from a timer interrupt (iWakeupThread from the handler at 0x8EFF0, SleepThread at ra 0x901C8), one every 184,3xx IOP cycles = **5.0 ms** (200 Hz), each run about 4,800 cycles, then it sleeps again; in between thread 10 (CRI_ADXI's SPU2 transfer thread, semaphore 6) does 499 rounds of 2 KB (7.5 per tick). 67 ticks x 5 ms = 0.335 s = 20 blanks.

**3. It is not the SPU2 DMA time (patch 0071).** With the DMA 2x faster (cycles per halfword 12 instead of 24, env knob in the scratch clone) the same window has 996 rounds in the same 67 ticks and the idle runs are identical (20, 24, 24, 12 blanks; frame-counter intervals 1.55/2.06/3.05/2.47 s against 1.78/2.02/3.01/2.77 s: the same within noise). With 6 the pauses disappear (idle runs only 2380-2400) but frames 1920-2220 get 10-30 % slower (more IOP rounds per second) and the outcome of the wait is not known to be the same (0071 is about a stream object destroyed while its transfer thread is mid-walk), so it is not a fix: it would be hiding the wait.

**Reading.** The duration is set by thread 13's 200 Hz service counting a fixed number of ticks (about 0.33 s at Start; the NOW LOADING ones add up to 1.0 s of idle blanks in the window), which is what a fade-out/ramp of a fixed length looks like; it does not scale with data or with the host. Not verified: what thread 13 counts (the module behind 0x900xx/0x8EFxx was not identified) and whether the console's frame loop pauses the same time. If the console does not pause, the next place to look is the 200 Hz source (which call arms the timer that fires the 0x8EFF0 handler, and in which clock the period is counted) and that thread's per-tick work.

**No code fix made:** shortening the wait would mean dropping the game's own wait or changing the IOP's time base for everything (the EE idle accounting is already exact: waitForEvent adds the skipped cycles to the EE and the IOP). Pacing-only options (do not sleep the host while the EE is idle and only the vertical-blank deadline is pending) would also speed up every normal frame, so they cannot be limited to these waits at scheduler level.
