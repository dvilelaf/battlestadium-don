# 014 · Two runs of the same script are not the same run: host time and other threads in the guest

- **Date:** 2026-10-13
- **Task:** T-306 (why two runs of the same script differ; follows 009, 010, 011, 012)
- **Status:** open

## Symptom

With the same binary and the same options, two runs of the same input script can differ, and the four
places where it was seen are all of this kind:

- 009: the buffer address passed to the replaced `sceMpegCreate` was a heap address in one run and an
  address inside the game's code in another;
- the uniform frame during the (undecoded) opening video is white in some runs and black in others
  (also seen by hand: `305  shot t114route3` white at 23:34, the same shot black with
  `DON_VU1=microvu`);
- 010: the practice match stays on its first "GET READY" frame in three runs and advances to "FIGHT"
  in a fourth, the slowest one (two diagnostics on); no thread is blocked;
- 011: one crash near vertical blank 1740 with `--sound on`, not seen again in six runs.

The port runs all guest code, the scheduler, the IOP emulator and the VU on one thread; an input script
is stepped on that thread at each vertical blank; the blanks fall at exact emulated cycles. So a run
should be a pure function of its inputs.

## Investigation

### The instrument (new diagnostic, `src/game/don_overrides.cpp`)

`DON_TRACE_STATE=<n>` prints one line every n-th call of the game's vertical-blank handler:

```
[don] state blank=<n> eeCycle=<ee> iopCycles=<iop> iopInstructions=<iopi> ramHash=<64-bit>
```

- `eeCycle` is `EeScheduler::eeCycle()` (patch 0034); `iopCycles`/`iopInstructions` come from
  `PS2Runtime::iopDebugSnapshot()`; the hash is 64-bit FNV-1a over the whole 32 MiB of guest RAM.
- Cost: the hash costs **3.2 ms** per line on this machine (printed once, on the first line). At n=60
  that is ~0.3% of the game thread; at n=600, 0.03%. The diagnostic reads memory and writes none, and
  it touches no emulated state (checked: `iopDebugSnapshot` is const end to end, `appendDebugMetrics`
  is const, `eeCycle()` is a plain read; `snapshot()` is deliberately not used because asking for a
  snapshot changes when the scheduler runs).
- `DON_STATE_DUMP=<file>` with `DON_STATE_DUMP_AT=<blank>` writes the 32 MiB once, at the very instant
  that blank's state line is printed, so two runs can be compared *at the divergence* (comparing the
  dump at the end of a run cannot: a value that differs for one frame is overwritten by then).
- Off by default, one integer test per blank when off.

### Where host time or another thread can reach the guest (ranked)

Full evidence with file and line: `work/orchestrator/tdet/part1-host-time-and-threads.md`. Summary:

| Rank | Where | What | Guest sees it |
|---|---|---|---|
| 1 | `Kernel/Stubs/CD.cpp:520-536` `sceCdReadClock` | writes the **host wall clock** (local time, BCD) into guest memory | yes, directly: it changes every run |
| 1 | `Kernel/Stubs/LibC.cpp:1083-1090` `rand`/`srand` | the stubs call the **host libc** `std::rand`/`std::srand` | yes: the seed the guest passes drives the host RNG |
| 1 | `Kernel/Stubs/MemoryCard.cpp:767-770` (`std::time(nullptr)`), `:264-268` `fileTimeToTimeTMc`, `Kernel/Syscalls/Helpers/Runtime.h:176-180`, `ps2_vfs.cpp:148-151`, `Runtime.h:194-202` | host file times and the time zone are converted with `system_clock::now()`/`file_clock::now()`/`std::time(nullptr)` | yes, as values in directory tables and timezone answers |
| 2 | `src/host/vu_glue.h` (defect of journal 012, fixed) | `HostVu` held a `DonVuApi` table that lived in a block that had ended: the VU callbacks read a **dead stack slot** whose contents depend on what the stack held before, i.e. on the run | yes, and it is the one already-proven cause: every run of 009-011 and the white/black observation used a binary built before the fix |
| 3 | `EeScheduler.cpp:1917-1964` `processDueDeadlines`, `:2142-2206` `waitForEvent`, `:2179-2183`, `:483-516` `accountCycles`/`flushIopDebt` | the pacing: an event due in emulated cycles is held until `steady_clock::now() >= hostDeadline`; the idle wait advances the emulated clock to the cycle deadline and flushes the IOP debt; the IOP runs in batches whose flush points are the batch threshold, VBlankStart, the idle advance and EE↔IOP access | not directly: the emulated cycle at which an event is processed does not change; this is the one candidate left for a divergence between runs whose RAM is otherwise identical |
| 4 | `src/host/host.cpp:72-84`, `:216`, `:373` (main thread) → read and merged at `:229-245` (game thread) | device pad states published at host moments, merged with the script at the emulated blank | with real devices yes; in `--headless` `sdl == nullptr` publishes a constant `neutral_pad_state`, so no |
| — | `ps2_runtime.cpp:2341-2344` `postEeEvent` → `EeScheduler::postEvent` | the only cross-thread way to inject an EE event | **no caller anywhere in the tree**; the only push into `m_events` is inside `postEvent` (`EeScheduler.cpp:372`). No thread injects events in this build |
| — | `EeScheduler.cpp:355-360` `requestStop` | the only real cross-thread wakeup | it stops the run |
| — | `Kernel/Stubs/MPEG.cpp:1847,1871,1906,2075,2121,2127,2214,2220` `completeExternalWait` | the guest's own `sceMpeg*` calls | delivered at the calling cycle; the file has no `chrono` and no thread |
| — | `src/gsbridge/don_gs.cpp:52,113,240` (owner thread), `:292` `GSgifTransfer`, `:329-349` `don_gs_read_fifo` | the PCSX2 GS is called synchronously from the game thread and refuses other threads | ruled out |
| — | `src/vubridge`, `src/spu2bridge` | no threads of their own | with `--sound on` the audio device and the CRI thread's sleep/wake enter; not these runs |
| — | `ps2xIOP/src` | **no** `chrono`, `std::time` or `std::thread` at all | the IOP is purely cycle driven; the CD stream's production is modelled in **vblank ticks** (`CD.cpp:16-119`: 4× DVD = 2700 sectors/s, 59.94 fields/s, 64-sector banks) |
| — | `ps2_memory.cpp:344-345` | guest RAM is `memset(...,0,ramSize)` at allocation | no uninitialised guest RAM |
| — | `ps2_runtime.cpp:1867-1887` (`configureGuestHeap`, `guestMalloc`) | guest-address allocator in a private range, calloc zeroes, malloc does not | no host address enters guest memory (grep: no pointer→u32 store in the stubs) |
| — | `src/host/screenshots.cpp` (`ScreenshotWriter::write` → `SDL_CreateSurfaceFrom`/`SDL_SavePNG`) | reached from the vblank path (`HostGs::on_vsync` ← `PS2Runtime::notifyHostGsVsync` ← `EeScheduler::processEvent`), so it runs on the game thread at a vertical blank | not guest-visible, but it crashes the run: 0 crashes in 199 runs without `--shots`, 1 in 2 match runs with it (the same `rip=0x66f80` as journal 011's core; see the match section) |
| — | `Support.h:553`, `iop_subsystem.cpp:89`, `EeScheduler` maps | containers keyed by u32 or int ids, not by host pointers | no iteration order the guest can see |

### What the measurement says (all headless, `--renderer software`, default options)

| Route | Runs | Result |
|---|---|---|
| `to_main_menu.txt`, `DON_TRACE_STATE=60` | 2 (`run-a1`, `run-a2`) | **differ at the very first line (blank 60)**: a1 `eeCycle=334387810 iopCycles=40630235 iopInstructions=35758556 ramHash=dcc6d9bf…` vs a2 `eeCycle=334387800 iopCycles=40645063 iopInstructions=35924193 ramHash=2820717a…` |
| same, more runs | 18 more (2 with dumps, 8 with dumps, 8 against a shorter script) | 19 of 20 runs at blank 60 are byte-for-byte the **a2** line; 1 of 20 is the **a1** line |
| short script, 24 more runs with `DON_STATE_DUMP_AT=60` | 24 | 24 of 24 canonical (and the whole 32 MiB identical to the first of them) |
| disc files and ISO evicted from the page cache (`POSIX_FADV_DONTNEED`) before each run | 3 cold + 2 warm | all 5 identical at blank 60; wall time 4.55-4.77 s cold, 4.52-4.60 s warm (the boot's reads are too small for the difference to matter) |
| `work/orchestrator/tdet/hunt_divergence.sh 150` (the repeatable hunt) | 150 | **0 divergent**: 150 of 150 canonical |
| `DON_TRACE_STATE=1`, 191 blanks | 2 | the two runs are **identical to each other at 191 of 191 blanks** (counters and hash) — but **both differ from every run at n>=5** (see "The host's speed …" below), so this pair is one reproducible regime, not proof of insensitivity |
| `to_main_menu.txt`, no trace, full route, `DON_DUMP_RAM` | 2 | final guest RAM **identical**: 0 differing words of 8,126,464 over 0x100000-0x2000000 |
| `answer_memcard.txt`, no trace, `--shots` | 2 | 3450 vblanks each; **every shot byte-identical** (`memcard_dialog`, `memcard_left`, `memcard_after_circle`, `next_1`..`next_5`, `frame_003360`, `frame_003420`) |
| `to_match_1v1.txt`, `DON_VU1=microvu`, trace every 600 blanks, `--shots` | 2 | **identical up to frame 4800**: all 84 shots byte-identical and the lines of blanks 600…4800 equal (`eeCycle=23632905059 iopCycles=3155835121 iopInstructions=3151114251 ramHash=dcc36be0e5eb6189`), then run 2 **died with SIGSEGV** (rc 139, 287 s) with no runtime message; run 1 went on to blank 7800 and **froze on "GET READY"**: frames 6540…7800 all `d1e6c5c1…`, the md5 of journal 010's frozen runs |
| `DON_STATE_DUMP_AT=60` over the whole 32 MiB | 9 + 8 runs | **identical** (`0 differing word(s) of 8388608`) — the canonical runs agree down to the byte at blank 60 |
| **total, in the build used before 14:01** | **221 runs at blank 60** | **218 canonical, 2 in the n<=2 regime (the `DON_TRACE_STATE=1` pair), 1 divergent (`run-a1`)** |

So in this configuration the port *is* a pure function of its inputs most of the time — **within one build
and one host-speed regime**: 218 runs of the short routes gave 1 divergence, both full routes are
byte-identical where they complete, and the dialog route is identical even in its pictures. Outside those
two conditions it is not (the next section), and the white/black video frame of my own earlier
observation and journals 009-012's runs were all made with binaries built **before** the `vu_glue.h` fix
of journal 012, which is a proven input of host memory into the guest.

The match route is the exception, and it separates the two kinds of difference:

- **The guest is identical**: two runs of `to_match_1v1.txt` with `DON_VU1=microvu` agree on every one
  of the 84 shots up to frame 4800 and on every state line up to blank 4800, down to the RAM hash.
- **Then the host faults**: run 2 died with SIGSEGV inside the next second, with no runtime message at
  all, while run 1 went on and froze on "GET READY" with exactly journal 010's frozen picture
  (`d1e6c5c1…` for 21 s). So journal 010's freeze is *not* fixed by the `vu_glue.h` fix: it still
  happens, byte-identically, with microVU and no sound; and the crash of journal 011's class is not
  fixed either — it reproduced here in 1 run of 2, without sound, at blank ~4810, at a point where the
  emulated state was byte-identical to the run that did not crash.
- Because the guest state is identical immediately before, the crash cannot be caused by anything the
  guest did differently: it is a host-side fault (uninitialised host memory, an out-of-bounds write or a
  race in the host code — the class of journal 012, whose fix was one instance of it).

### The match route's crash is in the host's screenshot writer, not in the guest

The run that died left no runtime message. Under gdb (`work/orchestrator/tdet/match-crash.gdb`,
`DON_VU1=microvu`, `--shots`) the fault is an indirect call to an unmapped address, on the game thread:

```
Thread 2 "GameThread" received signal SIGSEGV, Segmentation fault.
0x0000000000066f80 in ?? ()
#0  0x0000000000066f80 in ?? ()
#1  ScreenshotWriter::write(std::string const&)
#2  ScreenshotWriter::on_vsync(unsigned long)
#3  HostGs::on_vsync(unsigned long const*, int)
#4  PS2Runtime::notifyHostGsVsync()
#5  EeScheduler::processEvent(EeEvent const&)
#6  EeScheduler::processDueDeadlines()
#7  EeScheduler::processPendingEvents()
#8  EeScheduler::run()
#9  std::thread::_State_impl<...startGameThread...>::_M_run()
```

0x66f80 is below the executable's mapping (its `.text` is at 0x5555…6b9…), so this is a call through a
bogus pointer, and the only calls in `ScreenshotWriter::write` besides the `std::filesystem` helpers,
`fprintf` and the operators are `SDL_CreateSurfaceFrom@plt` (0x6b95f38) and `SDL_SavePNG@plt`
(0x6b95f4c), against the locally built `build/linux-dev/libSDL3.so.0`. Every shot that was written is
640x448, so no resolution change is involved.

How often: **0 crashes in 203 runs** *without* `--shots` (their logs all end normally), **0 of 20** short
runs *with* `--shots`, **1 of 2** match runs with `--shots` and **1 of 1** gdb runs with `--shots`. So the
screenshot path is a host-side crash source of its own, unrelated to what the guest computes (the guest
state was byte-identical to the run that survived, one second before).

**This is journal 011's undiagnosed crash.** That entry's core read (`work/cores/core.63685`, whose frame
chain could not be symbolised because `don` had been relinked) found `rip = 0x66f80` and the strings
`frame_00` / `3300.png` on the crashing thread's stack, and `frame_003300.png` is exactly the shot missing
from that run's folder. My backtrace has the **same faulting address, 0x66f80**, and names the frames
`ScreenshotWriter::write` ← `ScreenshotWriter::on_vsync` ← `HostGs::on_vsync` ←
`PS2Runtime::notifyHostGsVsync` ← `EeScheduler::processEvent` ← `processDueDeadlines` ← `run` on the game
thread — i.e. a screenshot write. Both of journal 011's crashes ran with `--shots` (`--shot-every 600` and
`--shot-every 300`), and `--sound` is incidental: my crashes have no sound at all. So journal 011's "the
first thing to fix is the crash" has its frame, and the crash is not in the sound path and not in the
guest.

`src/host/screenshots.cpp` is another agent's file and is not touched here. Whoever takes it: the value
`0x66f80` is a *host* address and is the same every time, and the calls in `ScreenshotWriter::write`
besides `std::filesystem`/`fprintf` are `SDL_CreateSurfaceFrom@plt` (0x6b95f38) and `SDL_SavePNG@plt`
(0x6b95f4c) against the locally built `build/linux-dev/libSDL3.so.0` (which does define `SDL_SavePNG`, at
ELF offset 0x60010 — next to the faulting 0x66f80, so check whether the two are being confused).

### The host's speed and the host's build are both inputs to the guest

Two measurements correct the "most of the time" above.

**1. The host's speed picks one of two reproducible states.** `DON_TRACE_STATE=n` costs ~3.2 ms per
printed line and the line is printed *inside* the game's vertical-blank handler, so n sets how much
systematic extra host time a vertical blank carries. At blank 60, two runs per n:

| n (one hash per n blanks) | wall of the 200-blank run | eeCycle | iopCycles | iopInstructions | RAM hash |
|---|---|---|---|---|---|
| 1 | 6.10 / 6.68 s | 334387800 | **40651185** | **36027780** | b8accb615b71cdaf |
| 2 | 4.72 / 4.67 s | 334387800 | **40651185** | **36027780** | b8accb615b71cdaf |
| 3 | 4.57 / 4.55 s | 334387800 | 40645063 | 35924193 | 5846af9aaaf75faf |
| 4 | 4.49 / 4.45 s | 334387800 | 40645063 | 35924193 | 5846af9aaaf75faf |
| 5, 6, 10, 20, 60 | 4.0-4.7 s | 334387800 | 40645063 | 35924193 | 5846af9aaaf75faf |

Both runs of an n agree exactly, and n<=2 is a *different* state from n>=3: at the same EE cycle the IOP
is 6,111 cycles (3,587 instructions) further on. So the host's speed does not jitter the guest, it
*selects* one of two trajectories, reproducibly, and the threshold is between n=2 and n=3, that is
between ~1.6 ms and ~1.0 ms of systematic extra host time per ~23 ms frame (~8 % and ~5 %). Instance noise
does not do it: the n=6 pair's second run took as long as the n=2 pair (4.73 s) and stayed in the n>=3
state, because what matters is the time added *inside the blank*, not the run's total.

What that points at: the line is printed inside the guest's vertical-blank handler, so the extra time lands
between two blank processings; past ~1 ms per blank the *next* blank's host deadline is already in the
past when `processDueDeadlines` looks (`EeScheduler.cpp:1942`), so `now < pacingDeadline` is false and the
wait is skipped: the run stops being paced and runs "always late". That branch is therefore *guest-visible*
at the same emulated cycle — the pacing gate of `processDueDeadlines`/`waitForEvent` is the path from host
speed to guest state, and the IOP's batch flush points are where it shows first. Which of the two
`accountCycles`/`flushIopDebt` call sites inside that path moves the IOP 6,111 cycles was not isolated;
the n ladder above is the tool (any n between 1 and 60 bisects it further).

**2. The host's build is an input to guest RAM.** The 218 runs with n=60 before 14:01 (12:5x-14:0x) all
have `ramHash=2820717a217e95cf`; every run of the same script after 14:01 has `ramHash=5846af9aaaf75faf`
with **identical** counters. What changed in between: another agent's build relinked `build/linux-dev/don`
at 14:01:47 and rebuilt `libSDL3.so.0` (14:01:46), and an agent edited the root `CMakeLists.txt` and
`CMakePresets.json` at 13:36 (adding an *optional* AddressSanitizer preset — the flags of the normal
build are untouched: `build.ninja` still has `-ffp-contract=off` and `-march=x86-64-v3` on all 1,985
compile units and no `-ffast-math`). The memory card is excluded: the first `[MC]` line is *after* the
blank-60 checkpoint (`q11.log` line 86 against the state line at 82), and mc0/mc1 are empty. So some
guest word at blank 60 is written from host state that changes with the binary, while the emulated
behaviour does not change at all. Not chased to the word.

Consequence for the method: a state line is comparable across runs **of the same build**, not across
rebuilds, and the census below counts runs of one build at a time.

### What the one divergent short-route run looks like

The two traces of the first pair, subtracted blank by blank (`run-a1` minus `run-a2`):

| blank | ΔEE cycle | ΔIOP cycles | ΔIOP instructions | RAM hash equal |
|---|---|---|---|---|
| 60 | +10 | −14,828 | −165,637 | no |
| 120 | +18 | −13,393 | −164,202 | no |
| 180 | −14 | −15,006 | −165,815 | no |
| 360 … 1980 | +2 | **−15,596** | **−166,405** | no |
| 2460 | +8 | −15,081 | −165,890 | no |
| 2520 … 3000 | 0 | **−15,596** | **−166,405** | no |
| 3180 | +16 | −28,306 | −179,115 | no |
| 3240 | 0 | −44,838 | −195,647 | no |

The IOP difference is **exactly constant** (to the cycle and to the instruction) for thousands of
blanks after the first second: the divergent run acquired one shortfall of 15,596 IOP cycles
(166,405 instructions) before blank 60 and then executed *exactly the same* number of instructions per
blank as the other run for the whole route. The EE cycle at a blank differs by one or two dispatches
(±8 cycles each) because a blank is processed at the first dispatch after its cycle deadline. So this is
not a race that grows: it is a single, quantized shortfall very early in the boot.

15,596 IOP cycles is 124,768 EE cycles, ~0.42 ms of emulated time, ~61 slices of the IOP's 256-cycle
batch (2024-EE-cycle) size. `IopEmulator::runEeCycles` carries its remainder (`iop_emulator.cpp:805-812`),
so a lost batch boundary does not explain it: the IOP really executed less, which is the signature of an
early stop inside the IOP's run at boot (a SIF/RPC block, or a batch the IOP could not consume) and of
that leftover being accounted somewhere else. Whether host timing enters there was not determined.

## Classification

Not a classification of `tools/run/classify_log.py`: the runs all end `OK_VSYNC_LOOP`. The mechanism
belongs to the family "a result decided by the host instead of by an emulated cycle". Precedent:
journal 012 (a value read from host stack memory), the profile-reader correction of 2026-10-13, and the
catalog row for host-time dependencies in `docs/knowledge/08-external-references.md`.

## Cause

Not determined, and now known to be *two* things, both of which contradict "a run is a pure function of
its inputs":

1. **The host's speed selects the state** (reproducible, section "The host's speed and the host's build"):
   the same script at `DON_TRACE_STATE=1` lands on a different but self-consistent state than at n>=5,
   at the same EE cycle and with the IOP 6,111 cycles further. So the pacing *does* reach the guest; the
   path from "host deadline" to "guest word" was not found. `ProcessDueDeadlines`/`waitForEvent`'s
   host-deadline gate (`EeScheduler.cpp:1917-1964`, `:2142-2206`) and the IOP's batch flush points
   (`:483-516`, `:2013`, `:2203`, `ps2_runtime.cpp:636-645`) are where a host-speed-dependent emulated
   cycle or IOP work could come from, and they are what to bisect next.
2. **The host's build changes guest RAM** with identical counters (same section): not chased to the word.

The one divergent short-route run is of the same kind as (1) but not reproducible in 199 runs: a single,
constant 15,596-IOP-cycle shortfall acquired before blank 60 (its own section).

Ruled out by reading and by measurement: cross-thread event injection (no caller anywhere), the
GS/VU/SPU2 bridges (game thread only), uninitialised guest RAM (zeroed), host addresses in guest memory
(none found), pointer-keyed iteration order (none), the pad path in headless (a constant), the memory
card and the RTC/file-time values for the *blank-60* state (the first `[MC]` line is after that
checkpoint, and 218 runs over 1.5 h share one hash while the host clock moved).

Certain causes of non-reproducibility in general, whether or not they fired here: the host wall clock in
`sceCdReadClock` and in the memory-card/VFS time conversions, the host libc RNG behind `rand`/`srand`,
and the VU-glue defect of journal 012 (fixed).

## Design: a run is a pure function of its inputs

The principle to design for: **every result the guest can observe is delivered at an emulated cycle
decided by emulated state, never by when the host got around to it.** For work a host thread or the
host's filesystem does, that means the delivery cycle is fixed when the request is made, from a model of
the device's duration, and if the host has not finished by then the game thread waits for it.

The console's drive, for the figures: DVD-ROM 4× CAV, 675 sectors/s at 1× (`CD.cpp:11`), 2700 sectors/s
at the speed this game uses (`CD.cpp:12,24`), i.e. 5.28 MB/s of 2048-byte sectors, with sector access
times of a few tens of ms for a few-sector read and ~100-200 ms for a full-stroke seek. The runtime
already knows, when a request is made, the LBN, the sector count, and the last LBN it served
(`g_cdStreamingLbn`, `readCdSectors`), and it already models the *streaming* path this way, in vblank
ticks (`CD.cpp:16-119`). What is not modelled is the *synchronous* path: `sceCdRead`/`readCdSectors`
and the VFS file reads currently do their host work inside the guest's call and report the result at
that same emulated cycle.

Per source, the delivery cycle and where it is decided:

| Source | Delivery cycle | Where |
|---|---|---|
| `sceCdRead`/`sceCdStRead`/`sceCdStream` sectors | request cycle + seek model (f(LBN − last LBN)) + sectors × 1/rate, in emulated cycles; the completion and the interrupt happen then, and the guest's `sceCdSync`/semaphore sees it then | `Kernel/Stubs/CD.cpp` (`readCdSectors`, `updateCdStreamProduction`, `sceCdSync`); the model replaces the immediate `ok` return |
| VFS file open/read/write (`ps2_vfs.cpp`, Support.h) | request cycle + a fixed small latency (or a modelled one), completion delivered at that cycle | the VFS layer, once per request; `completeExternalWait`-style completion for the file services that have a wait |
| `sceCdReadClock`, the RTC | a fixed epoch per run + the emulated clock (never the host's) | `CD.cpp:520` and `Kernel/Syscalls/Helpers/Runtime.h` |
| `rand`/`srand` | a runtime-owned PRNG, deliberately seeded per run (the run's seed is an *input*, like a real memory card) | `LibC.cpp:1083-1090` |
| memory-card/VFS timestamps, time zone | the same frozen clock as the RTC | `MemoryCard.cpp:264-268,767`, `Runtime.h:176-202`, `ps2_vfs.cpp:148-151` |
| host-thread work that has no guest request behind it (sound, GS) | already game-thread-only; a device that is fed by an interrupt must schedule that interrupt at an emulated cycle | `src/spu2bridge`, `src/gsbridge` |
| pad | latch the device state once per emulated blank, with a sequence number, so the game thread reads a consistent frame | `src/host/host.cpp` |
| the pacing (`processDueDeadlines`, `waitForEvent`) | unchanged in what the guest sees: the event is processed at its *cycle* deadline; the host deadline only throttles the host thread | `EeScheduler.cpp:1917-2206` |

Two options for the drive, for the orchestrator to choose:

- **(a) Faithful.** Delivery cycle as above with the console's rates. Consequence: loading screens last
  as long as on the console (long), the runs stay deterministic, and the game's streaming logic, which
  expects a drive slower than the CPU, sees what it was written for. This is the option that also makes
  the CD-streaming model of `CD.cpp` and the synchronous path agree.
- **(b) Fixed short latency.** Every request is delivered at request cycle + a fixed constant (say two
  vertical blanks or a fixed number of EE cycles), regardless of size. Consequence: deterministic and
  faster than the console, the guest cannot tell the difference from a fast drive, but a game that
  over-requests because it knows the drive is slow can still starve, and nobody can compare timings with
  a console run.

A reasonable middle: (b) for the file/VFS path (its results are already gated by the game's own load
screens) and (a) for the CD stream, where the game sees the rate.

**How a test would prove it.** In `tests/runtime`, a guest stub that issues a request and records the
emulated cycle at which the completion arrives, with a host-delay injection (an environment variable
that makes the host sleep a random 0-50 ms before serving the read): the recorded cycle must be
identical on every run and equal to the modelled one, and must not change when the injection changes.
The integration test is what was used here: run the same input script N times and compare
`DON_TRACE_STATE` lines and `DON_STATE_DUMP_AT` dumps — any difference is a failure.

## Verification

- `DON_TRACE_STATE=1` twice on `to_main_menu.txt` up to blank 200: 191 of 191 blanks identical to each
  other, and both in the *other* regime from the n>=5 runs (see "The host's speed and the host's build").
- `DON_TRACE_STATE=60` 218 times at blank 60 before 14:01: byte-identical to each other (whole 32 MiB
  dumps equal), with one divergent run (`run-a1`) — occasional, not systematic. The same script after the
  14:01 relink gives a different RAM hash with the same counters.
- Cold page cache (disc files + ISO evicted): 3 runs identical to 2 warm runs.
- No trace, full route: identical RAM at the end; `answer_memcard.txt`: identical shots (3450 vblanks,
  8 named shots each).
- `to_match_1v1.txt` with `DON_VU1=microvu`: identical up to frame 4800, then 1 crash in 2 runs, backtrace
  in the section above; the surviving run froze on "GET READY" (`d1e6c5c1…`).

Logs, dumps and diffs: `work/orchestrator/tdet/`. Dumps are guest memory and stay under `work/`.

## Temporary diagnostics

`DON_TRACE_STATE`, `DON_STATE_DUMP`, `DON_STATE_DUMP_AT` are new and remain (off by default, no cost
when off); they are the harness for this class of bug and are documented in the header comment of
`dumpState`. Nothing else was added. `src/game/don_overrides.cpp` is the only file of the port that was
changed for this entry.

## What is not verified

- The value that differs in the divergent run and its writer are unknown: that run predates
  `DON_STATE_DUMP_AT`. The instrument is in place and the hunt is repeatable — `work/orchestrator/tdet/hunt_divergence.sh 150`
  runs the same script 150 times in about 15 minutes and keeps the 32 MiB dump taken at blank 60 of any
  run whose state line is not the canonical one (that hunt found none; the one divergent run seen so far
  was the first run after the binary was linked, which is the lead to follow: run the hunt right after a
  rebuild, or under a deliberate CPU load). `tools/verify/ram_diff.py` on that dump against a canonical
  one lists the differing words; their writers then come from `tools/re/callers.py` and
  `bash tools/re/decompile.sh`.
- Whether journal 009's MPEG-buffer write and journal 011's `--sound on` crash still happen: their
  exact routes and options were not run. (010's GET READY freeze *does* still happen: see the match
  route above.)
- Whether the RTC, the file times or `rand`/`srand` are reachable in this game's boot: not instrumented.
  They are certain sources in general; in these routes the measured state does not depend on them.

## Dated note (2026-10-13, later the same day; T-304, journal 018) — one source of "two runs differ" is a build during a run

Agent C's observation that runs of the same script differ, and the crash that some of them ended in, have
one cause in common that has nothing to do with the game: `tools/pipeline/build.sh` ended every build with
`cp -fL .../libSDL3.so.0.4.12 build/<preset>/libSDL3.so.0`, and `cp -f` rewrites the existing file **in
place**. A run that had that library mapped lost its pages when a build ran during it (the kernel discards
the pages of a truncated mapped file, the relocated data included), and the next SDL call jumped to a
link-time address - `0x66F80`, seen as the crash in journals 011 and 015. The rule for it and the proof are
in journal 018; the build scripts now copy beside the target and rename over it. Anything measured in this
entry while a build was running in the background should be treated with that in mind.

## Dated note 2 (2026-10-13, T-304): the stop's *point* is selected by the IOP/EE batch size

Measured with `DON_IOP_BATCH_EE_CYCLES` on the sound match route (same binary, same script): with 8 or 64
the game stops on the main menu (frame 3060), with the default (2048) after the character select (4140),
and with 512 it renders the whole menu/character-select/stage-list sequence and stops in the match stretch.
Table in journal 011. That is this entry's subject in another form: at these screens the host is late, the
scheduler decides differently, and the difference is not in the guest's state but in *when* each
processor's work happens.
