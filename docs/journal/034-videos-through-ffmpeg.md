# 034 · The videos through the runtime's FFmpeg-backed `sceMpeg` stub: it builds, it is never asked for a picture

- **Date:** 2026-10-14
- **Task:** T-305 (first step: option (a1) of docs/knowledge/19-videos.md section 7)
- **Status:** parked (the build works; no picture appears; the chain stops before `sceMpegGetPicture`)

## FFmpeg for the build

`tools/setup/build_ffmpeg.sh` downloads `ffmpeg-7.1.1.tar.xz` (sha256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`,
pinned from the first download; ffmpeg.org publishes no hash file) into `third_party/ffmpeg/` and builds it **inside
the build container** (same clang 17 and glibc as the game; through `tools/container/run.sh`, so the lock and the memory
limit apply) into `third_party/ffmpeg/linux`. The container has no FFmpeg development package (its Dockerfile installs
none), so a pinned source build was the way. Configure line (all `--disable-everything`):

```
configure --prefix=third_party/ffmpeg/linux --cc=clang --cxx=clang++ --disable-everything --disable-programs
  --disable-doc --disable-network --disable-autodetect --disable-debug --enable-static --disable-shared --enable-pic
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo --enable-demuxer=mpegps
```

Result: 7 static libraries, 6.2 MB installed (x86 assembly on, nasm is in the image), about 4 minutes with 6 jobs.
Licence: no `--enable-gpl`, no `--enable-nonfree`, no external library: **LGPL v2.1 or later**, linked statically into a
GPL-3.0 program. What the stub calls (read in `Kernel/Stubs/MPEG.cpp`): `avcodec_find_decoder`, `av_parser_*`,
`avcodec_send_packet`/`receive_frame`, `av_packet_*`, `av_frame_*`, `sws_*`; the runtime's CMake also requires
libavformat and libswresample through pkg-config, so they are built (no demuxer is used by the stub).

The top-level `CMakeLists.txt` had `PS2X_ENABLE_FFMPEG` forced off in a loop. Now there is an option
`DON_ENABLE_FFMPEG` (default **OFF**, which sets `PS2X_ENABLE_FFMPEG` OFF exactly as before) and `DON_FFMPEG_PREFIX`
(default `third_party/ffmpeg/linux`; put on `PKG_CONFIG_PATH`). My build: `cmake --preset linux-dev -B build/linux-ffmpeg
-DDON_ENABLE_FFMPEG=ON`, then `cmake --build build/linux-ffmpeg --target don` (first attempt 5 min, failed only at the
link with `undefined symbol PadTable::PadTable(unsigned int, unsigned int)`: another agent's `src/host/pad_table.*` was
half edited while I built; the second, incremental build, 2 min, linked: `[6/6] Linking CXX executable don`, 119 MB,
and `nm` shows `avcodec_send_packet` and `MpegFfmpegDecoder` in it). **The binary therefore contains the other agents'
uncommitted changes of that moment in `src/host/`, not only mine.**

## The runs (headless, `--renderer software`, sound on with `--wav`, `DON_PLAY_MOVIES=1`, Japanese disc folder)

Route: LEFT at 270, CIRCLE at 330, nothing until a Start at 8400 and another at 8760; shots every 30 blanks. 9060 blanks in
2.5 minutes of wall time, exit 0.

- Frames 390-1830: the white health screen and the logos, as before. Frame 2010: grey. **Frames 2190 to 8310 (6120
  blanks, the console's whole video and more): a plain white screen, mean 255, standard deviation 0.0 in every shot.** No
  picture, no motion; not black as in journal 007 (white is what the player's own set-up leaves, as docs/RUNNING.md says).
- The video never ends by itself: no title before the Start at 8400. After it (shot 8700) the title is arriving
  (a grey image of the logo sliding in), at 9000 the title screen with "PRESS START button"; the second Start at 8760
  came too early for the title (it arrives about 300 blanks after the skip), so "Start opens the menu" was **not
  re-checked here** (it was in journals 007 and 024).
- Sound: the wav is 151 s; RMS 0 and peak 0 in every 10 s window from 10 s to 140 s, i.e. through the whole video;
  the music only starts with the title (RMS about 3000 from 140 s). Silence in the video as expected (the video's own
  audio is not connected).
- The run's log has **no `[MPEG...]` line at all** (the stub prints when it decodes, feeds, reaches EOF or fails; none did).

## Where the chain stops (gdb breakpoints on the runtime's handlers and on game functions, 100 to 110 s, with the same route)

`tools/run/gdb_trace.sh` copied to `work/t305/` with `--input-script` added, so that the dialog is answered.

1. The stub's calls, in order: `sceMpegInit` (from `0x35CF60`), `sceMpegCreate` (from `0x35D010`), 4 x `sceMpegAddCallback`
   (from `0x35D010`). **Never** `sceMpegGetPicture`, `sceMpegReset`, `sceMpegAddBs`, any `sceCdSt*` or `notifyMpeg*`
   (so the stub's stream hooks, `sceCdStStart`/`sceCdStRead`, are not what the game uses: the game reads the movie with
   plain `sceCdRead` through its file layer) and no `sceIpu*`.
2. Game functions in the 100 s: `0x346908` (the decode step of the player loop) **4508 times**; `0x35CA80` once, from
   `0x35D010` (the DMA4 start of the open); `0x35D768` (the frame step that calls `sceMpegGetPicture`), `0x35A058`,
   `0x357BE0`, `0x3573B0`, `0x356E38`, `0x35CB80`, `0x35D438`, `0x347228`: **never**. Calls of `sceCdRead` from `0x3244E8`:
   22, of `sceCdSync` 38 (the reads of the file layer; MOVIE.AFS is 69 MB, so only the beginning is read); the
   16 KiB blocks on `0x1000D400` keep alternating.
3. So the loop runs (4508 entries in 100 s) but CRI's state machine never reaches the frame step, so the stub is not
   asked for a picture. The decoder is not the blocker; something upstream of `0x35D768` is: either the stream data that
   CRI waits for never arrives (the stub's feed is by `sceCdSt*`/`sceMpegAddBs`, which the game does not call) or the CRI
   state machine waits on an event that the runtime does not produce. Which one is **not determined**; `0x346908` is
   the place to look (what it checks before calling into `0x357BE0`/`0x35A058`).

## Conclusion for (a1)

Turning FFmpeg on is not enough: the stub's own stream hooks (`CD.cpp:948`, `:826`) do not see this game's reads, and
CRI never gets to `sceMpegGetPicture` in the first place. Knowledge 19 section 7 (a1) said the stub is fed by those
hooks: measured now, that is not how this game reads the movie. Next step (own task): find with breakpoints/conditions
what `0x346908` and its callees wait for before `0x35D768` (in the same gdb setup), and whether feeding the stub from the
file layer's reads (the `sceCdRead` calls from `0x3244E8` onward) or running the game's callbacks for the data
(`0x35D238`...) is what the chain needs. If it turns out that CRI wants the IPU, (a2) stays.

## Windows

The runtime's CMake has a Windows branch that downloads `ffmpeg-n7.1-241205-lgpl-amd64-shared.zip` from
github.com/System233/ffmpeg-msvc-prebuilt at configure time (shared DLLs, copied beside the executable). That is a
third-party binary fetched during the build, with no pinned hash, which does not fit this project's way (pinned sources,
hashes, scripts in `tools/setup/`). Proposal: the Windows build uses the same script's configure line cross-built with
the Windows container's toolchain (`--target-os=mingw32 --arch=x86_64`, clang-cl/lld-link, static) into
`third_party/win-deps/...`, with `DON_ENABLE_FFMPEG` and a prefix variable instead of the runtime's download. **Not
done or tried here**; it is only worth doing once the Linux build shows a picture.

## Files

`tools/setup/build_ffmpeg.sh` (new), `CMakeLists.txt` (option `DON_ENABLE_FFMPEG`, default OFF). Work files under
`work/t305/` (not versioned). No change to the runtime, no patch.

## Follow-up 2026-10-14: what the player waits for before its frame step

Measured with gdb on `build/linux-ffmpeg/don`, headless, `DON_PLAY_MOVIES=1`. Runs `t305-gdb3` to `gdb16` were made with
the build that did NOT have patch 0062. The build was then redone (incremental) with the shared tree's patches 0061 and 0062
(and the other agents' uncommitted `src/host/` files) and `gdb17`/`gdb18` were run with it: same result. The movie's audio
level was not measured again after the rebuild (the gdb runs use `--sound off`).

The chain, from the player loop down (addresses are the game's):

1. `0x346908` (player loop) takes finished pictures from the handle's output ring (`0x360000`). It ran 3,212 times in 70 s
   and never got one, so `0x35D768` is never reached. The loop is not paced by an audio clock: nothing in it compares time.
2. The ring is filled by the Sofdec exec step `0x35E0E8` (state machine on handle `+0x48`, one-shot flag `+0x44`), called
   from `0x35E038` <- `0x34E3C8` (player server, per player) <- `0x34E310` <- `0x34E248` (server for all eight players).
   `0x35E0E8` was never entered. `0x34E248` ran exactly once (during init) and never again.
3. `0x34E248` has three callers: the SVM class-5 wrapper `0x34DF98`, the SVM class-6 wrapper `0x34DFE0` and `0x34E4A8` (no
   caller in the executable). Class 5 only runs it if the player global's `+0x10` is 1. The game's `mwPly` init (`0x347D58`)
   was called with `{0x426fc28f, 1, 0, 0}`, so `+0x10` = 0: only class 6 does it.
4. Class 6 (`0x331800`) is called only from the loop of the CRI server thread body `0x314850` (loop at `0x3148C4`). It ran
   once, returned 0, and the thread went into `SleepThread` (`0x314B00` from `0x3148E0`). It is never woken.
5. The wake-up comes from the vertical-blank handler `0x3157E8` (it runs ~3,000 times in 70 s and wakes the other two server
   threads, ids 3 and 4, each time). It wakes the third thread (`0x3D237C`) only if `0x3D22E8` == 1. `0x3D22E8` is written
   only in `0x315C18` (sets 1) and `0x315E08` (sets 0). `0x315C18` has no direct caller and was never entered; the game's
   init is the shorter `0x314FF0` (called from `0x331D20`), which does not set it. Read during the movie: `0x3D22E4`=1,
   `0x3D22E8`=0, `0x3D22D0`=-1 (so the handler runs in "mode 2": class 0 and class 5 only), `0x3D2400` (user callback) = 0.

So: the pictures are not produced because the player's server (`0x34E248`) is only runnable from the class-6 server thread, and
that thread, after its single pass, is never woken because `0x3D22E8` stays 0. The lead is therefore the thread wake-up
condition, not the audio clock and not the stub.

Not verified: that the thread asleep at `0x3148E0` is the one stored at `0x3D237C` (inferred from the class-6 call count of one
and the wake-up code, not read as a thread id); what the real console does to get `0x3D22E8` to 1 or to wake that thread
another way (the `0x3D2400` callback and `0x315C18` called through a pointer are the two untested ideas); whether the movie's
audio has the same stall (it is silent, which fits: its player is the same one).

Link with the music (journal 011): the same CRI library, but a different mechanism. The music's cause was the IOP thread
scheduler (patch 0062); this one is in the EE-side server threads and 0062 did not change it. If the music agent sees
`0x3D22E8` or the class-6 thread in its own traces, it is the same place.

No runtime patch proposed: no small certain fix, and forcing `0x3D22E8` = 1 would be hiding the symptom (rule 4).

## 2026-10-14, follow-up 2: the console during the movie, and the real difference

Seventh dump: `work/ref/x/07` (ignored by git), Japanese image, reference PCSX2 on its private Xvfb display (`:1`), memory-card
dialog answered (left, circle), nothing else pressed, savestate (F1) about 80 s after the dialog, while the capture showed moving
pre-rendered pictures (not black, not the title). Emulator stopped afterwards.

Console, in that image: `0x3D22E8` = 0 (as in the port), `0x3D22E4` = 1, `0x3D22D0` = -1, `0x3D2400` = 0, `0x3D237C` = 0, the
player's `+0x10` flag = 0. So the console does NOT use `0x315C18`; it runs the server another way. The difference is in the
loop counters: `0x3D2360` (class-6 thread loop) = 0x184F on the console (about one per vsync, `0x3D2350` = 0x17EB), but 0 in the port.
The class-6 thread (`0x3D238C`, body `0x314850`) is suspended by the vsync thread at every loop (`0x314B80`) and resumed + woken
by `0x315940` (called from `0x315A70`, which the movie loop calls every iteration): `if (ResumeThread(t) == t) WakeupThread(t)`.

Port, same code: `0x315A70` ran 2113 times and `ResumeThread(5)` 1057 times in 110 s, but `WakeupThread(5)` (`0x314AB0`) ran
once. (The gdb runs of the previous follow-up with a small event cap never reached this part of the run; the 110 s run does.)

Cause, by reading the runtime: `EeScheduler::resumeThread` and `wakeupThread` return `KE_OK` (0)
(`Kernel/EeScheduler.cpp`, `resumeThread` about line 811 and `wakeupThread` about line 860; wrappers in `Kernel/Syscalls/Thread.cpp`
`ResumeThread`, `wakeupThreadImpl`). The game compares the result with the thread id (`0x315940`, `0x3159C8`) as the
EE kernel does (those calls return the thread id on success). With 0, `0x315940` never wakes the class-6 thread, so the player's
server (`0x34E248` through `0x34DFE0`) never runs and no picture is decoded.

Proposed fix (not applied; shared runtime tree): make `ResumeThread`, `iResumeThread`?, `WakeupThread`, `iWakeupThread` (and check
`SuspendThread`) return the target thread id (the argument, after id 0 is resolved to the current one) on success, and the
negative error code otherwise. Test that fails without it: a runtime test with a thread that suspends a second thread, then
checks `ResumeThread(id) == id` and `WakeupThread(id) == id`. Risk: other game code that tests `== 0`; to check with the full
suite and the music/title runs.

Not verified: the fix itself (not built); the exact values of the return of `SuspendThread` on the console.

## 2026-10-14, follow-up 3: patch 0063 (thread calls return the thread id) — correct, but not enough; not in the shared tree

**Return values, by evidence.** The game's own code compares only two: `ResumeThread(t) == t` (0x315940) and
`WakeupThread(t) == t` (0x3159C8). Nobody compares the answers of SuspendThread (0x314B80 returns it, no caller reads it),
TerminateThread, DeleteThread, StartThread, SleepThread (`0x10C1C0`, results unused), ReleaseWaitThread, CancelWakeupThread. The
runtime answered 0 for Resume and Wakeup; the kernel answers the id (the game relies on it). No PS2SDK copy is on this
machine, so the documented returns of the others are **not** settled and were not changed. Errors kept: id 0 -406, unknown -407,
not suspended -415, dormant -413.

**Patch** `patches/ps2recomp/0063-thread-resume-wakeup-return-id.patch` (`EeScheduler::resumeThread` and `wakeupThread` return the
thread id on success; the i-variants share the handlers). Test `tests/runtime/thread_wake_return_test.cpp`: fails on the unpatched
tree (4 checks: Wakeup, iWakeup, two Resumes) and passes with it; the whole runtime suite passes (27). The test file is in the
tree but its line in `tests/runtime/CMakeLists.txt` is **not** (it would fail on the shared tree without the patch): add
`thread_wake_return_test` to the `foreach(host_test ...)` list together with the patch in `series`.

**Effect, measured** (shared build, then `build/linux-ffmpeg`, `DON_PLAY_MOVIES=1`, headless, software renderer):
- Server: the class-6 thread now runs, **four times**, then stops: `0x3D2360` = 4 at vsync 3000 and still 4 at 5000 (the console:
  one per vsync). `0x34E248` ran 4 times, `0x35E0E8` 3, and the frame step `0x35D768` was entered for the **first time** (2 times).
- Then the thread 5 is not woken any more: `0x314AB0(5)` runs 2423 times in 150 s, but the runtime's WakeupThread is never called,
  because the status it reads is not 4/0xC. Read at every ReferThreadStatus(5) with a gdb finish breakpoint: the first read is
  READY (2), every later read is 8 (SUSPEND), so the thread is left suspended and never runs again. It is suspended twice per
  frame (4442 SuspendThread(5) calls against 2423 resumes) by the two vertical-blank threads, `0x314658` and `0x314760`
  (both call `0x314B80(0x3D238C)` each loop); the console has the thread running between them. What lets it run there (priority
  and preemption: the resume of a thread that is higher than the caller preempts at once on the console) is the next thing to check:
  the thread's priority (`DAT_003d22f0`), the scheduler's preemption on Resume (`requestPreemptionIfHigher`) and the order of
  the three threads in one vertical blank.
- Pictures: still a plain white screen from blank 2160 to the end (180 s, 356 shots, mean 255, deviation 0.0); no hand-over to
  the title in 180 s; the WAV of that run was not analysed.

**Default build (FFmpeg off) gets worse with the patch**, so it was taken off the shared tree and both builds were redone without
it: the regression suite (`-m slow`, 13 min) had 4 failures of 4, all the same: `the frame loop did not run from vsync ~2200-2600 to
~5300-5640` (a gap of 3060 vsyncs, limit 300) in `default_title_idle`, `fight_practice`, `multiplayer_match`, `survival_result`.
Without the patch `default_title_idle` passes (re-run: 1 passed). So with the server half-alive the port's Start press (240 blanks
into the player) no longer ends the player quickly: the stop path of the player waits for the server, which does not run (51 s
before it gives up). No landmark picture was compared (the failure is the liveness check, not an image).

**Next:** make the thread-5 schedule right (the status 8 above), then put 0063 in the series; the patch alone makes the default
build wait 51 s at the video.

## 2026-10-14, follow-up 4: 0063 revised (ids as answers + suspends do not nest): the thread still stops after four runs; removed again

**The premise "the game suspends thread 5 twice per loop" was not right.** The 4,442 against 2,423 of follow-up 3 were calls of the
game's wrappers (`0x314B80`, `0x314B18`), not of the syscalls. The suspend wrapper itself skips a thread whose status is 8 or 0xC, so
the game never nests a suspend. Counted at the syscalls (150 s, movie run): `SuspendThread` 43,776, `ResumeThread` 43,774: balanced.
So the non-nesting change has no effect on this game; it is kept in the patch only because it was asked for and the console does not
nest (not measured: the dump 07 thread table, words at 0x1A740 + n*0x4C, shows the entry points and priorities (the server thread
0x68, the vertical-blank threads 0x12 and 0x19) but no field that can be read as a suspend count).

**Patch** (`0063-thread-resume-wakeup-return-id.patch`, header rewritten): returns of Resume/Wakeup as before, plus a SuspendThread of a
Suspended or WaitingSuspended thread changes nothing and answers 0 (chosen; no caller reads it). Test extended (two suspends, one
resume: the id, then -415, status READY by ReferThreadStatus); fails on the unpatched tree (4 checks), passes patched, runtime suite 27
passed. Its line was added to `tests/runtime/CMakeLists.txt` and **removed again** with the patch (otherwise it fails on the shared
tree): add `thread_wake_return_test` at the end of the `foreach(host_test ...)` list together with the patch in `series`.

**Measured with the patch on the shared tree and `build/linux-ffmpeg`** (`DON_PLAY_MOVIES=1`, headless, software, 200 s, 399 shots):
- Server loop counter `0x3D2360`: 0 until blank 2000, **4** from blank 2500 to 6900 (the console: one per blank). `0x34E248` 4 times, `0x35D768`
  (frame step) 2 times. Same as follow-up 3.
- Thread 5 now cycles correctly between SLEEP (status 4, wait type 1, wakeup count 0) and WAITING-SUSPENDED (0xC) when read at
  ReferThreadStatus, so the wakeups arrive, but it makes no iteration: the loop body calls `0x331800` -> `0x331610(6)`, which runs six
  registered callbacks; the fifth iteration never comes back from one of them (the counter is bumped before it). At 230 s the scheduler
  is idle (`EeScheduler::waitForEvent`: every guest thread is waiting), i.e. a callback of class 6 waits for an event that does not come.
  Which callback and for what is the next question (the list is at `0x548BA0 + 6*0x48`, six (function, argument) pairs).
- Pictures: plain white screen, mean 255, deviation 0.0, from blank 2160 to the end (shots 70 to 398). No hand-over to the title. The
  sound of the movie was not measured (nothing was decoded).
- Regression (`-m slow`, 15 min) with the patch: **4 failed, 4 skipped**, the same four liveness failures as in follow-up 3 (frame loop
  stopped 3,060 blanks; the Start press after 240 blanks does not end the player). Patch taken off, both builds redone.
Other agents' uncommitted `src/host` files were in the builds.

## 2026-10-14, follow-up 5: the callback that blocks the server is the runtime's `sceMpegGetPicture`, which waits for a stream that nothing feeds

**Method.** From now on 0063 is only in an isolated tree: `work/t305/iso/PS2Recomp` (the whole series applied and committed as base, then
0063 on top) and `build/t305-iso` (`cmake --preset linux-dev -B build/t305-iso -DDON_ENABLE_FFMPEG=ON -DDON_RUNTIME_DIR=<that tree>`; the cache
variable `DON_RUNTIME_DIR` already existed, nothing was added to CMake; full build 6 min 55 s). The shared `third_party/PS2Recomp` was not touched.
The regression harness can be pointed at it with `DON_BINARY=build/t305-iso/don` (`tests/regression/harness.py:30`).

**The list.** `0x548BA0 + 6*0x48` holds six (function, argument) pairs; on the console (dump 07) only the first is used:
`0x34DFE0(0x4C4D90)`, the other five are 0. It is the video player's server: it calls `0x34E028` (a flag), and unless that answers 1,
`0x34E248` (reads the player at `0x347B18`, loops over 8 sub-players with `0x34E310` -> `0x34E3C8` -> `0x35E038`, then `0x35DFC0`,
`0x34E5D8`, `0x34E218`).

**Where it blocks (port, patched 0063, 200 s, gdb, entry breakpoints and the guest `ra`/`sp` of WaitSema/SleepThread read from the context).**
The server thread's last events are many `0x35C4E0` (building DMA tags for the IPU's DMA channel 4), one `0x35C478` (starting it), and then the
**runtime's `sceMpegGetPicture`**, called from the frame step `0x35D768` (via `0x35D010`'s registered path); it never returns. The stub waits
(`EeScheduler::waitExternal(EeWaitReason::Mpeg ...)`, `MPEG.cpp:2299`) while `decodedFrames` is empty and no CD stream end was seen.
Every semaphore wait/signal of the thread balances (379 waits, 378 signals + 1 `iSignalSema`); the earlier SleepThread/WakeupThread
cycling is the thread being resumed and then sleeping again between work.

**What it waits for.** Frames from the stub's decoder. Its input is only fed by `sceCdStStart`/`sceCdStRead` (`CD.cpp:948`, `:826`) and
`sceMpegAddBs`. Counted with gdb in the same run: `notifyMpegCdStream*` (3 breakpoints) 0 hits, `feedElementaryStream` 0 hits, `sceCdSt*` never
called; the game reads the movie with `sceCdRead` (25 calls from `0x3244E8`) into CRI's own ring and sends the elementary stream to the IPU
through DMA channel 4 (the `0x35C4E0` tags, `0x35C478`). **The runtime has no DMA channel 4 / IPU model** (`ps2_memory.cpp`: IPU registers are
stored words; no handler for `0x1000B400`), so the bitstream never reaches the stub: knowledge 19's (a1) paragraph "the stub is fed by the runtime's
CD streaming hooks" is false for this game (it was already marked so in the 2026-10-14 note) and the console's decode (DMA4 -> IPU -> DMA3) has no
counterpart. On the console the same call returns, because the IPU decodes what DMA4 delivers.

**Defect: a data condition (a missing input path), not a wrong return value.** With 0063 the server runs until its first picture request and then waits
for ever; without it the server never runs at all. 0063 is necessary and, with this, the movie needs a second piece: **feed the stub from the game's
DMA channel 4** (write to `0x1000B430` CHCR with STR, source chain mode: walk the tags written by `0x35C4E0`, pass the ES bytes to
`feedElementaryStream`, mark DMA done and raise the channel's interrupt), and check that the picture the stub writes at the `sceMpegGetPicture`
buffer is what CRI's DMA3 callback (`0x35CB80`) then expects. That is a feature of about the size of the GIF path's chain walker, not a small fix:
not written here. The regression suite was not run (no new patch).

**Not verified:** that the DMA4 data is a plain video elementary stream (the tags point into CRI's ring, the demuxer is CRI's); the layout CRI wants back.

## 2026-10-14, follow-up 6: DMA channel 4 now feeds the decoder stub (patch 0064, draft); the pictures decode and arrive scrambled

**What the game sends on channel 4.** The only IPU command it ever writes is BCLR (`IPU_CMD = 0`). The first start is `MADR=0x01121040`
(CRI's ring), `TADR=0x011a1040` (the tags), `QWC=0`, `CHCR=0x30000105`: source chain, one `ref` tag (id 3) of `0x80` quadwords per 2048-byte
sector. The ring holds a raw MPEG-2 video elementary stream (it starts `000001B3`, sequence header 640x448). CRI's `0x35C880` rewrites the
tags after each stop, so the chain ends exactly at the last valid sector (last tag `refe`, id 0). `IPU_BP` and `IPU_CTRL` read as 0, so CRI
takes the DMA cursor as the consumed position; the stub keeps the bytes it has already pulled.

**The patch** (`patches/ps2recomp/0064-ipu-to-dma-feeds-mpeg-stub.patch`, applies on top of 0063, not in `series`): `PS2Memory::ipuToPull`
walks the started chain lazily and `sceMpegGetPicture` pulls 2048 bytes at a time until a picture is ready. A separate "started" flag is used
because reading CHCR clears its busy bit.

**Measured.** With `DON_PLAY_MOVIES=1`, the route that presses nothing, 399 channel-4 starts in 200 s; the picture changes from blank 2010 to
about 6840; the shots show real decoded content in the right colours (a character in orange, another crying).

**Not solved: the layout on screen.** The upload is the game's `0x36A430`: for each 16-pixel column, a GIF image transfer of 16 x `height` pixels
(`height` is what the stub reports, 448) to x = 16 * column, with a ref tag of `height*4` quadwords; so the strip layout the stub already wrote is
what the consumer reads (stride 28 macroblocks measured with a colour-per-macroblock pattern). On screen only the first 16 macroblocks of every
strip land where they should; the next ones show another strip's rows (screen rows 256+ of columns 0-19 show strips 20-39), as if the strips
were written with a different stride or wrapped. `GetPicture` is called with 1200 macroblocks (40 x 30), not 1120; padding the strips to 30 rows,
and a macroblock-raster layout, were both tried and are worse. Not yet checked: whether the fault is in the runtime GS's image upload (16 x 448
rects, `DBW=12`) rather than in the stub's layout.

**Also open.** `sceMpegIsEnd` depends on the CD-stream end flag, which this path never sets, so the movie does not end by itself. Movie sound
and timing not checked (a `--wav` of 100 s has a peak of 18764). No test written; the regression suite was not run. The time box was reached.

**Temporary diagnostics (all removed):** `DON_IPU_TRACE`, `DON_MPEG_DUMP`, `DON_MPEG_PATTERN`, a GetPicture trace and a `sceDmaSend` trace.

## 2026-10-14, follow-up 7: the opening movie plays right and ends by itself (0063 + 0064 + 0065); 0063 stalls the port's skip

Written by the orchestrator from the agent's report (T-305); the agent stopped at its time box.

**The scramble was the display, not the decoder (patch 0065, draft).** The runtime's `sceGsSetDefDBuff` (stub of patch 0021) took
`ffmode != 0` as field mode; the game's own code has field mode as `interlace == 1` and `ffmode == 0`. The second display buffer
went to FBP 35 instead of 70, so the two 640x224 buffers overlapped in GS memory. The console's dump writes `FRAME_1 = 0xa0046`
(FBP 70). With 70: 0 bad blocks of 2240. Test `tests/runtime/gs_dbuff_frame_mode_test.cpp` (fails without: page 35).

**The end (patch 0064, regenerated).** `sceMpegGetPicture` now calls once the Nodata callback the game registered (type 1,
`0x35D340`) when no picture is ready, channel 4 has delivered bytes and is idle; that callback marks the end and the player's loop
exits. Test `tests/runtime/mpeg_ipu_dma_test.cpp` (first picture only; needs `PS2X_ENABLE_FFMPEG`; without 0064 it crashes).

**The movie** (isolated FFmpeg build, `DON_PLAY_MOVIES=1`, the route that presses nothing, 200 s, sound on): right colours from
frame ~1830 to ~6930 (a ramen scene with three characters, a group pose with Luffy and friends, Sakura running, a stadium with
fists); then the title "BATTLE STADIUM D.O.N / PRESS START button" by itself; Start opens the main menu. Sound from 34.1 s, about
1 s after the first picture, RMS 1.7k-4.8k (peaks ~19000), then the title music. Not repeated on the final binary.

**Blocker: 0063 stalls the port's skip.** With 0063 + 0064 + 0065 all four regression routes fail ("the frame loop stopped"); with
0063 alone `default_title_idle` stops at frame 3720; 0063 without its non-nesting part fails the same way, so the cause is the ids
that ResumeThread/WakeupThread now return. With `DON_PLAY_MOVIES=1` (nothing pressed for the player) the game runs. 0064 + 0065
without 0063: `default_title_idle` and `fight_practice` pass; `multiplayer_match` ran to its end (verdict lost); `survival_result`
not run. Isolated tree `work/t305/iso/PS2Recomp` has 0064 + 0065 without 0063; 0063's variant in `work/t305/hold/`.

## 2026-10-14, follow-up 8: why 0x34E248 answers 1 after the stop (T-305, sub-agent; time box reached, not fixed)

**What makes `0x34E248` return 1.** It returns `FUN_0035dfc0() != 1`. `0x35dfc0` walks the 8 decoder handles in the table at `0x3F1ABC` and
answers 0 as soon as one has `+0x48` (state) in 1..4 **and** `+0x44` (request pending) != 0. So the server says "busy" for as long as one
decoder handle has a request that nobody has processed. The request is processed only by the player's step: sub-player `0x34E310`
(needs `*sub == 1`, `sub+0x60 != 1`, and `*(0x3F0298+0x24) != 1`) -> `0x34E3C8` -> `0x35E038` -> `0x35E0E8`, which clears `+0x44`.
`*(0x3F0298+0x24)` is the flag that `0x34E590(sub, 1)` sets around the flush `0x34E620` (`0x34DA00` = `0x331560(6)`, the server run
inline on the calling thread). `0x314418` clears nothing itself: thread 5 clears `0x3D2324` and sleeps once (`0x314B00`) when its
`0x331800` returns, so main is released if thread 5 then sleeps again, i.e. if class 6 answers 0.

**Measured at the stall** (temporary diagnostic in `ChangeThreadPriority`, removed): slot 0 = `0x140BA60`, `+0x44=1`, `+0x48=4`, `+0x4C=4`;
sub-player 0 (`0x3F0304`) `+0=1`, `+4=2`; `0x3D2324=0`, `0x3D2334=1`, `0x3D23D8=0`; thread 5 iterates `0x331800` (~340 priority raises
per iteration) and never sleeps. The first flush of the stop (`0x34E620` from `0x348A28`... first call) works with the same
`+0x44=1`/flag 1; the stalled one is the second call of the same function, issued while thread 5 is blocked **inside** a sub-player
step (gdb: `0x34E508(sub,1)` and `0x35E038(handle)` seen, then main's `0x34E620`, then the step finishes with `0x34E508(sub,0)`).
After that the handle is left in state 4 with a request pending and the flag is 1, so no step can drain it.

**What was not found:** which stub answer differs from the console. Not yet looked at: what thread 5 waits on inside that step (a
`WaitSema` in the decode state, `0x35E6A8`, is the candidate: the IPU/DMA3 completion that the stub does not signal the way the
hardware does), and whether on the console the step is not blocked at that moment. Next measurement: breakpoints on `WaitSema`/`SignalSema`
with the guest `ra` between `0x34E508(sub,1)` and `0x34E508(sub,0)` of the last step, and the semaphore id (the DMA3 semaphore at
`handle+0x...`/`0x35CB80`'s `puVar9[0x10]`). No patch, no test written; the regression suite and the Start-tap run were not repeated
(nothing changed). Isolated tree restored (diagnostic removed, rebuilt).

## 2026-10-14, follow-up 9: thread 5 does not block inside the sub-player step (T-305, sub-agent; time box reached, not fixed)

**Measured** (gdb breakpoints on `WaitSema`/`SignalSema`/`iSignalSema`/`WaitEventFlag`/`SleepThread` plus the game's `0x34E508`, `0x34E620`,
`0x35E038`, `0x35CB80`, with guest `a0`/`ra`; and a temporary per-syscall log `tid/a0/ra`, removed): between `0x34E508(sub,1)` and the next
`0x34E508(sub,0)` of the stalled step the only semaphore touched is **id 4**, always as a lock/unlock pair (`WaitSema` at ra `0x35C6F8`/`0x35C7D0`/`0x35C820`/
`0x35CBB0`/`0x35C8B8`, `SignalSema` at `0x35D2D4`/`0x35D324`/`0x35C868`/`0x35CE68`/`0x35CA4C`): it is the ring-buffer mutex `param[0x10]` of
`0x35C6C0`/`0x35C880` (decompiled), never contended. `0x35CB80` (the DMA3 callback) is called once inside the step. There is **no `WaitEventFlag`** and no
`SleepThread` by thread 5 in the window. So the candidate of follow-up 8 (a decoder completion waited on with a semaphore) is **false**: nothing waits.
The step is not blocked, it is **preempted**: after `SignalSema(4)` at `0x35CA4C` the timer handler (`0x314A60`, `iWakeupThread` of 3, 4) runs, threads 3, 4, 1 run and sleep
(`SleepThread` at `0x31471C`, `0x314810`, `0x315A98`), and the next thing the CPU executes is the second `0x34E620` (reached from `EeScheduler::run`, i.e. a thread resumed
at that entry, no game frames below it), whose first calls are the same `WaitSema(4)` at `0x35CA98` plus `0x35E038(0x140F1C0)` as a step starts with, then `0x34E508(sub,0)`.
After it thread 5 sleeps at `0x3148E8` and then loops `SuspendThread(2)`/`ResumeThread(2)` (4100 times per 100 s).
The gdb run is slower than the game, so its timing diverges (one run never reached the stall); the per-syscall log run (no gdb) did.

**Conclusion:** there is no stub answer to fix on this path: the stalled state is an interleaving, a flush that starts while a step is half done. Open question for the next
attempt: whether on the console the step can be preempted between `0x35CA4C` and its end at all (priorities of threads 1, 3, 4 against 5 and the time-slice; the
runtime's `m_timeSliceExpired` path in `EeScheduler::run` re-queues a preempted thread, `enqueueReady(.., !m_timeSliceExpired)`), and why the resumed flush does not
see `*(0x3F0298+0x24)` cleared. No patch, no test; regression suite and Start-tap run not repeated (nothing changed). Isolated tree restored, rebuilt.

## 2026-10-14, follow-up 10: the scheduler follows the priority rule; the stall is thread 5 spinning on `ChangeThreadPriority(self, 1)` (T-305, sub-agent; time box reached, not fixed)

**The lead of follow-up 9 does not hold.** Checked `EeScheduler` against "never run a lower-priority thread while a higher one is ready":
`selectReady` takes the lowest-numbered non-empty queue; `requestPreemptionIfHigher` only reacts to a strictly higher thread; the time slice
(`hasReadyAtOrAbovePriority`) only rotates equals; `iWakeupThread`/`iSuspendThread` of a lower thread never move the running one (a stale
`m_rescheduleRequested` re-queues the running thread at the front and `selectReady` picks it again). No deviation, so **no patch and no test**.
**Measured** (temporary logs in `makeRunning` and `changeThreadPriority`, removed; Start-tap route `work/t305/stall/s.txt`, `DON_PLAY_MOVIES=1`,
`--headless --renderer software`, `en_patch/jpvoice`): the stall reproduces (liveness: frame loop stops after frame 2280). Thread 5
(CRI server, entry 0x314850) is **not** at priority 1 during its step: it lowers itself, `ChangeThreadPriority(5, 25)` then `(5, 1)`
(about 600 pairs, one per step), so threads 3 (16) and 4 (18) legitimately preempt it (`5->3 status5=Ready prio5=25`, 1000+ times). The
`DON_TRACE_THREADS` table shows `prio initial/current`, which is why 1 looked like the current one.
**The stall starts at the last pair:** thread 5 is at 25 and Ready, preempted by thread 3; then thread 5 is **Suspended** (status 4, by a
vblank thread) and thread 1 (main, prio 24, running) calls `ChangeThreadPriority(5, 1)` on it (the log line: `t5 25->1 status=4 cur=1`);
from then thread 5 runs at priority 1 and calls `ChangeThreadPriority(5, 1)` itself 10 million times without ever blocking (`1->1`,
`cur=5`), so threads 1, 2, 3, 4 never run again: the frame loop (thread 1) starves. In the normal steps thread 5 lowers itself to 25
before the next step and that is where the others get their time; after the main thread's forced `(5, 1)` the loop that does the lowering
does not (it keeps asking for 1). Next step: the code at 0x314850 (the loop around the `ChangeThreadPriority` call: what it compares
before lowering to 25), and the main thread's caller of `ChangeThreadPriority(5, 1)` (ra not logged). Open: whether on the console the
SuspendThread by a vblank thread can hit thread 5 while it is at 25 (the console may keep suspend and priority together, or the
main thread's call may be on a path the port takes wrongly). Regression suite and Start-tap not re-run for a change: there is none; the
tree is back to its state before the logs and rebuilt.

## 2026-10-14, follow-up 11: ending the stream from the host (T-305, sub-agent; time box reached, NOT solved: the player does not leave)

Binary `build/t305-iso/don` (0063 + 0064 + 0065 + 0066 + the draft below), English folder, `--sound off`, route `work/t305/skip/tap.txt` (the
memory-card answer, a Start tap at 2500, nothing else). Draft runtime change: `work/t305/0068-movie-end-request-DRAFT.patch` (not in `series`; 0067 was
taken by another task).

**What was built.** Runtime: `ps2_stubs::mpegSetMovieEndRequest(bool)` (level) and `mpegDecoderAvailable()`; while the request is up, `sceMpegGetPicture` drops
the queued pictures, feeds the decoder a sequence end code once, and channel 4 is run to its end without delivering a byte (`PS2Memory::ipuToSetDiscard`, also
at the CHCR write so that the game finds the channel idle); the Nodata callback is then queued as at a natural end. Host: `src/host/video_skip.h` (`VideoSkip`,
`skip_delay`) hides Start and Circle of player 1 while the game's player runs and turns a press that begins during it into the request; `DON_MOVIE_SKIP_AFTER`
requests it by itself; unset: with FFmpeg the movie plays until the press, without it 240 blanks; `DON_PLAY_MOVIES=1` = until the press, always (the
routes that tap Start at 2400 then go through the request, not the game's stop path). The game side no longer needs `DON_PLAY_MOVIES` to install the probe.
Tests that pass: `tests/runtime/mpeg_ipu_dma_test.cpp` (the stub's end request: no further picture, channel 4 completed with STR clear and D_STAT bit 4, the
tail dropped, a chain started under the request ends at its start), `tests/host/video_skip_test.cpp` (routing and delay, header only).

**What does not work.** With the request at blank 2500 the picture freezes on the last frame shown and the player never leaves (`0x55A860` stays non-zero to
blank 6000; the frame counter keeps running, every EE thread waits). Measured with temporary traces (removed): the stub is asked for a picture 13 times
after the request; each time channel 4 delivers a whole ring (524288 bytes) that is dropped and the Nodata callback runs once; then no `sceMpeg*` function is
called again (no `IsEnd`, `Reset`, `Delete`, `GetPicture`). The same with the sequence end fed to the decoder and without it (27 calls instead of 13).
So CRI's frame step is not entered again, and the game's own state machine (`0x346908`, called by the player's loop) never reaches its end.

**Why the callback alone does not end it.** `0x35D340` (read from the generated code, only addresses and the shape here): it takes the Sofdec DMA state `S`;
if `S+24 != 1` it reads channel 4 (`0x35C880`) and, only when `S+20 != 0` (a transfer was started) and the value read at `D+16` is 0 (channel idle), sets `S+24 = 1`
and calls `0x35D2A0` with a 16-byte constant, i.e. it sends one quadword (the sequence end) over channel 4; otherwise it returns 1 and nothing else. A natural
end has the ring empty, so CRI starts no new chain after that; when a skip drains the ring, CRI's server keeps reading the file and refills it, and the step starts
chain after chain (13 rounds, then the stall). The end is therefore not a property of the decoder's input but of CRI's own file/ring state: dropping bytes on the
decoder side does not end a movie whose file is still being read.

**Not done.** The regression routes were not run (the end does not work, so every route that reaches a movie would stall). The shared tree is untouched.
**Next candidates** (not tried): (a) end it on the file side, making the IOP's reads of the movie file report end-of-file after the request (what CRI sees at a
natural end; the ring then drains in seconds, it is not instantaneous); (b) fast-forward: do not drop pictures, do not pace them, with the game's loop at full
speed (bounded by its per-frame vblank wait, so not instantaneous either); (c) find what `0x346908`'s state needs after the callback (gdb on the state word
`0x55A870`, the CRI server threads t3-t5 are all in a wait with the stall on).

## 2026-10-14, follow-up 12: the console against the port around Start in the opening movie (T-305, sub-agent; measured, not fixed)

**Method.** Console: reference PCSX2 on its private Xvfb display (`tools/ref/pcsx2_auto.sh`, savestates to `work/ref/t305raw`), Japanese image, memory-card
dialog answered, savestate (F1) at about 75 s after it, inside the movie (`55A860` non-zero = player running), then Start (Return, 100 ms) and savestates
0.15 s, about 1 s and about 4 s later (script `work/t305/console/run.py`; extracted with `p2s_extract.py`; reader `work/t305/console/rd.py`). The savestate itself takes
about 0.6 s, so the "after" states are 45 / 130 / 350 vertical-sync counts (`0x3D2350`) after the first, not 10 / 60 / 240 blanks. Port: `build/t305-iso/don` rebuilt from the
current tree (journal 034 follow-up 11's host draft is not in `src/`; the first binary still had it and its `DON_PLAY_MOVIES=1` goes through the host's request, so it never reached the
game's own stop: the player simply stayed on, `0x3D9C10`=25 to blank 3280 — that is why the binary was rebuilt), 0063+0064+0065+0066, script `work/t305/stall/s.txt`,
`DON_PLAY_MOVIES=1 DON_TRACE_STATE=1 DON_STATE_DUMP=<file> DON_STATE_DUMP_AT=<blank>` (one run per blank, `work/t305/console/port.sh`). Thread table (status, current
priority): on the console the kernel's own table at `0x1A740` is in the savestate; in the port the kernel is the runtime's and RAM holds none (all zero there), so the thread
rows are **not comparable** (the table's rows 1-4 do not change in a way that can be read as a priority; not measured further).

| Value | Console before Start | Console +45 / +130 / +350 sync | Port before Start (blank 2478, 2484) | Port 2487 | Port 2490 | Port 2493 and later (to 2740) |
|---|---|---|---|---|---|---|
| `0x3D2324` (stop flag) | 0 | 0 / 0 / 0 | 0 | 0 | 0 | 0 |
| `0x3D9C10` (saved priority) | 25 | 24 / 18 / 18 | 25 | 24 | 25 | **1** |
| `0x3F0298+0x24` | 0 | 0 / 0 / 0 | 0 | 0 | 0 | **1** |
| handle 0 `+0x44` / `+0x48` | 0 / 4 | freed (slot 0 empty) | 0 / 4 | **1** / 4 | 0 / 4 | **1** / 4 |
| `0x55A860` | `0x00EB41A0` | **0** (player gone) | `0x00EC67E0` | same | same | same, never cleared |
| `0x55A870` | 0 (`0x012D1140` in dump 07) | 0 | 0 | `0x012E3780` | 0 | 0 |
| `0x3D2350` (vsync thread loop) / `0x3D2360` (class-6 loop) | 5115 / 4330 | 5160 / 4387, 5245 / 4387, 5467 / 4387 (**stops counting**) | counts | counts | counts | `0x3D2350` **frozen at 2488**, `0x3D2360` grows by about 10^4 per blank |

**Reading.** Up to blank 2490 the port agrees with the console in every value (the port even passes through `0x3D9C10`=24 and `+0x44`=1 at 2487, the same 24 the console shows at +45: the stop's
first flush runs on the main thread, priority 24, and completes: 25 / 0 / 0 again at 2490). The first value that differs is `0x3D9C10` at the **second** flush: **1** in the port (the saved priority is that of
the thread that runs it, so it runs with priority 1, i.e. on thread 5 after main's `ChangeThreadPriority(5, 1)` of follow-up 10), together with `0x3F0298+0x24`=1 and `+0x44`=1 left set. On the console the value
after the stop is 24 (a flush run on the main thread) and then 18, never 1, and the player is gone within 45 sync counts. `0x3D2324` (the stop flag) is 0 in all of them, so it is not the
discriminator. What the dumps do not show: the order of events between the console's first and second state (the savestate is too coarse); whether on the console thread 5 is suspended at that moment.
**Implication for the fix:** the divergence is not in a value the game stores but in who runs the second flush: on the console it runs on the thread that calls it with its own priority (24); in the port the thread
that runs it has been given priority 1 by `ChangeThreadPriority(5, 1)` on a thread the port keeps Suspended-at-25. The next measurement is `ChangeThreadPriority` on the console for a thread in the
Suspended state (does the console change the priority and keep the thread suspended, does it queue the thread, does it run it?): breakpoint in the console emulator on the kernel call (PCSX2 debugger, or a state taken
with the emulator paused by a breakpoint at `0x34E620`'s second call), or compare the runtime's `changeThreadPriority` for a Suspended thread with the kernel's. Nothing in the tree was changed; the previous binary (with the host draft) is kept as
`work/t305/console/don-draft-host`; `build/t305-iso/don` now matches `src/`. The emulator was stopped (`pcsx2_auto.sh stop`); the other Xvfb displays on the machine (:101-:103) are not mine.

## 2026-10-14, follow-up 13: the console's thread table, and the call that differs: `ReferThreadStatus` of a thread in the runtime's pacing wait (T-305, sub-agent; patch 0068 written, not in `series`)

**Console thread table** (savestates `work/t305/console/x_*`, kernel table `EEInternalThread` of PCSX2's `BiosDebugData.h`; the rows start at `0x1A724 + 0x4C*k` for the game's threads, the entry word is at +0x30; reader `work/t305/console/th2.py`;
the earlier `th.py` read the table at the wrong offset). Tids as the port prints them: t1 main (prio 24), t2 entry 0x314568, t3 (16), t4 (18), t5 CRI server (entry 0x314850).

| state | t1 | t2 | t3 | t4 | t5 | wakeup counts |
|---|---|---|---|---|---|---|
| before Start | RUN 24 | SUSP 8 | WAIT sleep 16 | WAIT sleep 18 | **WAIT sleep, prio 25** | all 0 |
| +10 / +60 / +240 after Start | RUN 24 | SUSP 8 | WAIT sleep 16 | WAIT sleep 18 | **WAIT+SUSPEND (sleep), prio 25** | all 0 |

**Port before the fix** (`DON_TRACE_THREADS=1`, `work/t305/stall/s.txt`): at vsync 2040 to 2460 t5 is *running* (never asleep) and its wakeup count grows 22, 48, 62, 84, 109, 132, 148, 157; at 2520 main calls `ChangeThreadPriority(5,1)` and t5 stays at 1/1 for good.
**Cause found** (temporary log in `WakeupThread`, removed): all 168 `WakeupThread(5)` come from `0x314AF0`, inside `0x314AB0`, which does `ReferThreadStatus(t, &s); if (s.status == 4 || s.status == 0xC) WakeupThread(t)`.
In 158 of them t5 is `Waiting` with reason **VSync**: the runtime paces `sceMpegGetPicture` (`MPEG.cpp`, `waitVSync`) inside t5, so the guest reads status 4 and wakes a thread that is not asleep; the kernel counts the wakeup, and later `SleepThread` calls return at once. The console's t5 never shows a wait in that state: that time is code running.
**The runtime call whose result differs: `ReferThreadStatus` (status 4 for a pacing wait).** Fix, patch `patches/ps2recomp/0068-refer-thread-status-pacing-waits.patch` (a wait of reason VSync, Mpeg or External is reported READY, or SUSPEND when also suspended; not in `series`, the shared tree untouched).
`ChangeThreadPriority`, `ResumeThread` and `SuspendThread` were not at fault (follow-ups 10 and 12 followed the symptom).
**Verified** (the isolated tree, same route): wake=0 in every table row; at Start t5 is `waitsusp prio 1/25` asleep at `0x3148E8`, as the console's WAIT+SUSPEND at 25; no stall (the game thread runs to the end of the script, frame 3060);
state dump at blank 2740: `0x55A860`=0 (player gone), `0x3D9C10`=18, `0x3D2350`=2736 counting, `0x3F0298+0x24`=0 (before: `0x00EC67E0`, 1, frozen 2488, 1). The console: player gone, 24 then 18. No unit test (a thread blocked in the pacing wait is not small to build); the regression suite was not run.

## 2026-10-14, follow-up 14: getting 0063 + 0064 + 0068 ready for the series (T-305, sub-agent; time box reached, one blocker)

**Applies from clean.** A scratch clone of `third_party/PS2Recomp` at the pin (`work/t305/scratch/PS2Recomp`): the present `series` (0057 to 0062, 0066, 0065, 0067) and then 0063, 0064, 0068 in that order all apply with `git apply`, no fuzz.
**Test for 0068** `tests/runtime/thread_status_pacing_wait_test.cpp`: a worker waits 100000 blanks through `eeWaitVSyncTicks` (reason VSync); `ReferThreadStatus` must read READY (2) and, after `SuspendThread`, SUSPEND (8); the control, a thread in `SleepThread`, must still read WAIT (4). Without 0068: `got 4` and `got 12` (2 checks fail, control passes); with it, passes.
**Runtime suite on the scratch tree** (series + 0063 + 0064 + 0068, FFmpeg ON): `DON_RT_TESTS_DIR=build/rt-t305 ... pytest tests/runtime -q` gives 32 passed.
**CMakeLists lines** (`tests/runtime/CMakeLists.txt`, done in this change): `thread_wake_return_test` and `thread_status_pacing_wait_test` join the `host_tests` list (they link `ps2_runtime`); `mpeg_ipu_dma_test` is appended when the new option `-DDON_RT_FFMPEG=ON` is given (it sets `PS2X_ENABLE_FFMPEG=ON` and the pkg-config path of `third_party/ffmpeg/linux`). Patch 0068's header now names its test.
**Host defaults** (`CMakeLists.txt`, `src/host/video_skip.h`, `host.cpp`, `don_overrides.cpp`, `docs/RUNNING.md`, `tests/regression/harness.py`): `DON_ENABLE_FFMPEG=ON` defines `DON_WITH_FFMPEG` for `don` **and for `don_host`** (the first build defined it for `don` only: the host kept the automatic skip, which looked like a result and was not; caught by the same route failing with and without `DON_PLAY_MOVIES`). With it the port presses nothing and the player's own Start skips the movie; `DON_MOVIE_SKIP_AFTER=N` makes the port press Start after N blanks in either build; without FFmpeg the skip after 240 stays the default; `DON_PLAY_MOVIES=1` still turns the pressing off. The harness sets `DON_MOVIE_SKIP_AFTER=240` unless the caller did (it has no effect on routes whose `env` has `DON_PLAY_MOVIES=1`).
**Regression, FFmpeg ON** (`build/t305-ser`, 1 failed, 5 passed): `save_and_load` fails in its second process ("then", the load): the frame loop stops at blank 2340 (white screen, an IOP line "E2004090216 : Specified handle is invalid." at the start of the movie) and Start taps from 2400 on (also seven of them, up to 2800) do not free it. Deterministic (3 runs). The same step passes on the FFmpeg OFF build. The first step of the route (save) and the other four routes pass.
**Regression, FFmpeg OFF** (`build/t305-off`; default_title_idle, fight_practice, save_and_load run): fight_practice and save_and_load pass; **`default_title_idle` fails**: the port's automatic skip (the route has no `DON_PLAY_MOVIES`) ends with the main thread (t1) waiting for good at 0x37A310 from blank 2220 (thread table: t2 suspended, t3, t4 waiting, t5 `waitsusp` at priority 25, all wake counts 0). It is the stall follow-up 7 reported for 0063 ("0063 stalls the port's skip"); 0068 fixes it with the decoder, not without one.
**So 0063 + 0064 + 0068 are not ready for the series as they are**: (1) without FFmpeg the port's skip stalls (a build that is the default today); (2) with FFmpeg, the load step of `save_and_load` stalls in the movie. Neither was investigated further.
**One run with FFmpeg, sound and no pressing** (`build/t305-ser`, `--sound on --wav`, `DON_MOVIE_SKIP_AFTER` unset, script: answer the card dialog, shots at 2600, 4000, 6000, 9000): the movie plays, the wav is 151.7 s, RMS 0 from 10 to 30 s (before the movie), 1800 to 4800 from 30 s on; the shot at blank 4000 is the title ("PRESS START button"), the one at 9000 the attract movie.

## 2026-10-15, follow-up 15: the load step of `save_and_load` with FFmpeg: what stops, not yet why (T-305, sub-agent; time box reached, NOT fixed)

**Reproduced** alone (`build/t305-ser`, `-k save_and_load`, 155 s): the save step passes, the load step fails the liveness check (last frame advance at vsync 2340, gap 1080).
**The movie is not the stall**: the route has `DON_PLAY_MOVIES=1`, the movie ends by itself at blank 2220 (GsSetCrt frameMode 0) in both steps, before the first Start tap (2400).
**The IOP line** `E2004090216 : Specified handle is invalid.` comes from the CRI ADX code (strings found in `CRI_ADXI.IRX` and `TSNDDRV.IRX`), printed at the answer to the ADX RPC (SID 0x90000200) function 0x409 that the game sends at the movie end (client call site 0x326068, first word 0x7BAF8, then the two IOP buffers 0x81660 and 0x81690 that the matching function 0x408 got at the movie start).
The two steps send the *same* 0x408 and 0x409 (same arguments, same order, traced with a temporary print in `SifCallRpc`, removed); the save step gets no error line, the load step does. So the IOP answers differently to an identical request: the ADX stream handle that 0x408 created is not the one the IOP finds at 0x409 (the reply buffers of those calls are not on the stack ABI the trace read: they were null, so the handle value was not compared).
**What the EE does next**: the main thread t1 stops for good in the game's sound-request wait loop (`0x37A308`, return address `0x37A310`: `sceGsSyncV(0)`, the SFSV frame call `0x377E58`, `0x315928`, then `0x377A90(slot) == 2`); it is a slot of the sound ring (`[[0x4B2DF8]+0xC30+4*slot]`) that never reaches state 2. In the load step the 0x8000 ring sent to SID 0x53465356 never changes after 0x409 (no `SNDF` function 0xA03/0x703 follows, as it does in the save step), i.e. the sound driver on the IOP does not take the request after the error.
**Not done**: which IOP-side state differs (the handle table of the ADX module after a run that loaded a save at start: the card load happens at blank 240, 1990 blanks before the movie), the console's answer, the fix and its test. Next step: log the reply of 0x408 (SifCallRpc's receive buffer through the runtime's own ABI selection, not the stack) in both steps, and `DON_TRACE_IOP` around 0x409.

## 2026-10-15, follow-up 16: without FFmpeg the stub decodes black pictures; the port's skip no longer stalls (T-305, sub-agent; patch 0070, not in `series`)

**Cause checked before writing.** With 0063 + 0064 the player's `sceMpegGetPicture` takes the stream from channel 4, but without FFmpeg `MpegFfmpegDecoder::feed` fails (it was a stub returning false), so `decodedFrames` stays empty and the call waits for ever. The player only needs pictures to arrive; the console's decoder always delivers. So the no-FFmpeg decoder now acts as one that outputs black pictures: it reads width and height from the sequence header (`00 00 01 B3`) and pushes one black picture (RGB 0) per picture start code (`00 00 01 00`, with a 3-byte carry between feeds). Pacing, the Nodata callback at the end of channel 4 and the sequence end are the unchanged path of a real decoder. The FFmpeg build is untouched.
**Patch** `patches/ps2recomp/0070-mpeg-black-decoder-without-ffmpeg.patch` (one hunk in `MPEG.cpp`, applies on the series + 0063 + 0064 + 0068; checked on a fresh clone). Tree: `work/t305/black/PS2Recomp`; build `build/t305-black` (`DON_ENABLE_FFMPEG=OFF`).
**Test** `tests/runtime/mpeg_black_decoder_test.cpp` (in `tests/runtime/CMakeLists.txt`, added when `DON_RT_FFMPEG` is OFF; `mpeg_ipu_dma_test` is the ON counterpart): a synthetic 64x32 stream with three picture start codes through a channel-4 chain; the first `sceMpegGetPicture` returns at once, 64x32, counter 0, black over the whole buffer, channel 4 completed. Without 0070: segfault (exit 139); with it: all checks passed.
**Regression, FFmpeg OFF** (`DON_BINARY=build/t305-black/don`, `-m slow -k "default_title_idle or fight_practice"`): `2 passed, 2 skipped` (the two skips are the `_with_sound` variants) in 307 s; `default_title_idle` is the route that failed in follow-up 14.
**Not run:** `save_and_load` and the FFmpeg-ON build (the load stall of follow-up 15 is independent of this); the full runtime suite.

## 2026-10-15, follow-up 16: the console with the same saved card, and the IOP objects at the 0x408/0x409 pair (T-305, sub-agent; time box reached, NOT fixed)

**Oracle** (reference PCSX2 on its own Xvfb, Japanese image, IOP and EE consoles on in a private copy of `pcsx2_auto.sh`, `work/t305/o16/`): the route's card folder cannot be used as it is (a PCSX2 folder card without `_pcsx2_superblock` is "not formatted"), so the save was made on the console
with the route's keys (card = folder `DONsave`, the game formatted it, BGM 1 / SE 2 saved, "Save ... completed"), the emulator was stopped and started again on that card. Result: the movie plays (about 80 s), ends by itself, the title shows
("PRESS START button" at about 127 s of emulator time); `emulog` has **no** "Specified handle is invalid" and no ADX error, neither in the save session nor in the load session. The console goes on; the port's load step does not.
**Port, the 0x408 / 0x409 pair** (temporary print in `SifCallRpc`'s completion, SID 0x90000200, buffers read after the call with the selected ABI, plus IOP RAM through `readIopMemory`; removed): both steps send the same requests and get the same replies,
0x408 -> handle `0x7BAF8` (buffers `0x81660`, `0x81690`), 0x409 with `0x7BAF8`; the handle block at `0x7BAF8` is identical after 0x408 (`1 2 2 0x7E000`) and after 0x409 (`0 2 2 0`) in both. So the handle the EE passes is not what is invalid.
**What differs** after 0x409: (a) the buffer descriptors `0x81670` / `0x816A0`: save `0x10, 0xC6D50, 0xC6D40, 0`, load `0x10, 0xC6D40, 0, 0` (and `0x7BB08`+8: `0x91` against `0x10`); (b) the error is printed from IOP code at ra `0x77980` (CRI ADX), a1=1, and **a2 = a3 = `0xB6530`**,
an IOP address that is not a handle block: before 0x408 it holds code (`nop; jr ra; addiu sp; ...`, in both steps), at the return of 0x408 its first word has been overwritten by a pointer (`0xF8E40` save, `0xF8640` load: 0x800 apart), at the end of 0x409 it reads `0xF9640 0x80 0xF9840 0x200` in the save step and
`0xF9440 0 0xF9840 0` in the load step. The sizes (`0x80`, `0x200`) are missing in the load step: the object the ADX code validates is empty there.
**Reading.** (1) Something stores into IOP RAM at `0xB6530`, which is inside the text of an IRX (the words around it are MIPS code), in both steps: the IOP-side address that CRI's code takes for an object is not a free block (a pointer that was computed from a heap or module base that the emulator
gives differently from the console, or a module image that the emulator places over memory the game later reuses). (2) The load step differs from the save step only in what the card load did to the IOP heap earlier (the two buffer pointers are 0x800 apart: the IOP allocations before the movie are shifted), which is why the identical EE requests fail only there.
**Not done**: which allocation lands on `0xB6530` (the IOP heap's `AllocSysMemory` / `AllocHeap` results around the card load and at 0x408, the module image ranges of `CRI_ADXI.IRX`), the console's value of `0xB6530` at the same moment (savestate with the saved card at the 0x408), the fix and its test.
Next step: log `AllocSysMemory`/`AllocHeapMemory` returns and module bases (`DON_TRACE_IOP` is not enough) in the load step and see which range contains `0xB6530`; compare with the console's IOP RAM at the same point (`work/t305/o16/` has the card and scripts; the console's log is `emulog-load.txt` there).
Tree left as it was (diagnostics removed, `build/t305-ser/don` rebuilt from it; the unmodified earlier binary is `work/t305/o16/don.before`). The emulator is stopped and `PCSX2.ini` is restored from `work/t305/o16/PCSX2.ini.bak`.

## 2026-10-15, follow-up 17: `0xB6530` is a live local of an IOP thread, not stale or overlapped memory (T-305, sub-agent; time box reached, NOT fixed)

**Method.** Temporary write hook in `IopMemory` (`write8/16/32`, `writeRam`, `zeroRam`: address, value, pc, ra of the writer) over `0xB6090..0xB68D0` and later `0xB6500..0xB6560`, plus a log of every heap `allocate`/`free` (all removed; tree and `build/t305-ser/don` rebuilt clean). Note `write16/32` have RAM fast paths: a hook only in `write8` sees nothing.
**Measured (load step, `save_and_load` `then`).** (1) The block `0xB6090` (size `0x840`) is allocated once (pc `0x786d8`, ra `0x72498`) and **never freed**; it is the next allocation after `0xB57C0` and just before the `0x41040` block at `0xB6900`, identical in the save and the load step. There is no overlap and no reuse: nothing writes into it through `writeRam` or `zeroRam`. (2) The writes into it are ordinary stores of a thread whose stack/frames live there (word stores with ra `0x74bd4`, `0x7225c`, ...: the pattern of frames being pushed and popped, `0xB6800..0xB6838` in the save step).
(3) `0xB6530` itself is written only by the code at pc `0x73b8c`/`0x73b94`/`0x73ba8` (ra `0x73a08`): `[0xB6534] = 0`, then `[0xB6530] = ptr` with ptr cycling `0xF8240, 0xF8640, 0xF8A40, 0xF8E40` (step `0x400`) in a tight loop, millions of times in each step (4.8 million logged stores before the error line in the load step). So it is a loop variable (a 4-slot buffer ring pointer plus a count) of a spinning IOP thread, **not** an ADX object: the sizes `0x80`/`0x200` and the zeros seen in follow-up 16 are just where that loop was when the snapshot was taken. The error line's a2 = a3 = `0xB6530` is a stale argument register or an unrelated stack slot, so the "empty object" reading of follow-up 16 is withdrawn.
(4) What is the same in both steps: the handle block at `0x7BAF8` (cleared by the loop at `0x78694`, set at `0x6F3A4`, `0x6EF38` toggles byte +1, cleared at `0x6F3E4`). The error line is printed right after the clear at `0x6F3E4` in the load step only.
**Consequence.** The cause is not in the heap. Open: (a) the loop at `0x73a08` is a spinning poll (millions of iterations): what it waits for (a DMA/IRQ/event the emulator does not deliver in the load step?) and whether it runs in the save step for as long; (b) the check at `0x77980` (ra of the printf) is the thing to read: dump the IRX code of `CRI_ADXI.IRX` around `0x77940..0x779A0` (module image from the disc, offline) and see which memory it tests before printing.
**Next step:** disassemble (offline, not versioned) `0x6F3C0..0x6F400` and `0x77940..0x779A0` from the module image, then log the registers and the memory read by the test before `0x77980`.
Hook logs were up to 5 GB (deleted); do not log every store of a hot range.

## 2026-10-15, follow-up 18: the error is an object used after its destroy: a scheduling race between the ADX RPC thread and a polling thread (T-305, sub-agent; time box reached, NOT fixed)

**Disassembly** (offline from `disc/MODULES/CRI_ADXI.IRX`, load base `0x6E000`, text file offset `0xA0`; nothing versioned). (1) The handle block `0x7BAF8` is not what the error checks. `0x6F3C0` is the destroy function of the 0x409 request (`lw s0,[a1]; free([s0+0xC]); [s0+0xC]=0; sb zero,[s0]` at `0x6F3E4`). (2) The message `E2004090216` is printed by `0x73EC0`, inside the function at `0x73E7C(a0 = object, a1, a2 = chunk)`: it checks `a0 != 0` (message ...214), `[a0+4] != 0` (...216, "Specified handle is invalid"), `[a0+0x20] != 0`. It is called only from the wrapper at `0x73E1C` (`0x73E54`), which brackets the call with `0x537C`/`0x53C8` (interrupts off/on). (3) The spinning thread of follow-up 17 (`ra 0x73A08`) is the sibling wrapper at `0x739C8`: it calls the same chunk code on the chunk at `0xB6530` (`{ptr, len}`, a stack variable, written at `0x73B8C..0x73BA8`). The `0xB6530` of the error line is therefore the chunk argument (a2 = a3), a correct value; the invalid thing is the object in a0.
**Measured** (temporary print at `0x73E7C`, `0x6F3C0`, `0x6F3E4`, `0x73EC0` and a watch on `0x7F5E0` in `step()`; removed, tree and `build/t305-ser/don` rebuilt clean). The polling thread walks a table of 0x40-byte objects `0x7F4E0, 0x7F520, 0x7F560, 0x7F5A0, 0x7F5E0, 0x7F620` calling `0x73E7C` on each (about 270,000 rounds per step). The object `0x7F5E0` is created at `0x735FC/0x73608` and destroyed (words 0 and 4 cleared by `0x736D8`) inside the 0x409 request, right after `0x6F3C0` starts, **in both steps**. The difference is where in a round the polling thread is at that moment: save step, the destroy lands after the call on `0x7F620` (end of a round), and the last call on `0x7F5E0` was before it; load step, it lands after the call on `0x7F5A0`, so the next call goes to the just-destroyed `0x7F5E0` (`[a0+4] == 0`), which prints the error (the printed `a0 = 0x80000` is the format pointer). So the "different answer to an identical request" of follow-up 15 is **a race**: the same destroy, a different interleaving with a thread that polls millions of times.
**Why it matters / not solved.** On the console the same interleaving would give the same error unless the polling thread cannot be running in the middle of a round when the RPC thread runs, i.e. it is not a free-running loop there. What the loop waits for was not found (the "thread entry `0x78734`" of doc 14 is an import stub, not code: that address needs rechecking). 4.8 M rounds per step means the emulator never blocks it; next: find which IOP call the thread makes between rounds (a `DelayThread`, a semaphore or a `vblank` wait that the runtime returns at once or never delivers), and check whether the RPC server thread can preempt it in the middle of a round (the `0x537C/0x53C8` bracket is per call, not per round; patch 0062 holds a thread only while it has interrupts off). A fix that makes the polling thread wait as on the console should remove both the millions of iterations and the race; no code was changed.

## 2026-10-15, follow-up 19: the "polling thread" has no wait to fix: it is the SPU2 transfer thread, paced by the transfer-end interrupt, and it spins because the transfers it starts are empty (T-305, sub-agent; step 1 only, no patch)

**Measured** (temporary import trace in `dispatchImport` and `dispatchSemaphoreImport`, keyed on library, ordinal and `ra`; removed, tree and `build/t305-ser/don` rebuilt clean; run: `save_and_load`, both steps). In the module code (`0x6E000..0x7C000`) the only imports made over and over are `intrman` 17/18 (`CpuSuspendIntr`/`CpuResumeIntr`, the nested lock at `0x732D0`/`0x7337C`, `ra` `0x733A0`/`0x73418`), `thsemap` 7 (`iSignalSema`, `ra 0x72898`) and `thsemap` 8 (`WaitSema`, `ra 0x72400`). There is no `DelayThread`, `SleepThread`, vblank or alarm call in the loop, so there is nothing the emulator "returns at once" in their place.
**What the loop is** (offline from `CRI_ADXI.IRX`): thread 10 (priority 39, entry `0x723C0`, created at `0x7245C`) does `WaitSema(6)`, then for each of the two SPU2 cores checks a flag set by `0x72870` (the transfer-end callback registered at `0x72988` with the libsd transfer-interrupt setter), clears it and calls `0x72128`, which starts the next transfer (`sceSdVoiceTrans`-like call at `0x729D0`, size = `[obj+0x18] << 9`). The callback `0x72870` sets the flag and `iSignalSema(6)`. In the trace `WaitSema` is reached 300,000+ times and the semaphore is already 1 each time (max 1), so it never blocks; the signals come half from thread 10 itself (an inline call path) and half from interrupt context (`thr=-1`). So the loop is paced by the host SPU2's transfer-end event, and each round ends at once because the transfer ends at once: consistent with follow-up 16 (sizes zero only in the load step): zero-byte transfers complete immediately, raise the end interrupt, and the thread starts the next one.
**Conclusion.** The premise of the task card (a missing blocking call) is false: the console loop is paced by transfer-end interrupts of non-empty transfers, not by a sleep. A blocking patch would hide the symptom (rule 4) and the polling race of follow-up 18 would remain. The open question is why the object's transfer sizes are zero in the load step (what writes `[obj+0x18]` of the SPU2 stream object, `0x72128` and its caller), and what the host SPU2 does with a zero-length `dma_write` (if the console would not raise an end interrupt for it, that is a second difference). Not investigated; no patch, no change in the series.

## 2026-10-15, follow-up 20: the SPU2 transfers are not empty, they end 24 times too soon on the sound-off path; patch 0071 gives them the console's time (T-305, sub-agent)

**Measured** (temporary print in the IOP emulator's `schedulePendingDma`; removed; route `save_and_load`, `--sound off`, FFmpeg build with 0063+0064+0068). The transfers the thread starts are **not** empty (follow-up 19's guess is withdrawn): all are 2,048 bytes (delay value 1,024 = 512 words), core 0 (irq 0x24). The fake path (no host SPU2, which is what `--sound off` and the regression routes use) raised the end `max(words * 2, 64)` IOP cycles after the start: 1,024 cycles; rounds were about 1,130 cycles apart (2.30 M starts in 2.6 G cycles, about 32,600 rounds per IOP second). PCSX2 (`SPU2/Dma.cpp`, `CYCLES_PER_WORD = 24`, per halfword; `DoDMAwrite` gives 24 cycles for fewer than two halfwords) ends the same transfer after 24,576 cycles. The host SPU2 bridge (`src/spu2bridge`, sound on) already calls PCSX2's own core, so its timing is the console's; only the fake path was wrong.
**Fix.** Patch 0071 (`iop_memory.cpp`): delay = `max(words * 2 * 24, 24)`. After: delay 24,576, rounds about 24,700 cycles apart (100,000 starts in 2.47 G cycles, about 1,490 per IOP second: 22 times fewer).
**Results.** `save_and_load` (slow, FFmpeg build `build/t305-ser`): fails before (frame loop stops, 1,080-vblank gap), passes after. `DON_REGRESSION_SOUND=1 -k alive_with_sound`: 6 passed. `tests/runtime/iop_spu2_dma_time_test.cpp`: fails without 0071 (4 checks: 1024 vs 24576, 64 vs 48, 256 vs 6144, 64 vs 48), passes with it. The whole suite was not run by this task (orchestrator, at integration).
**Why it works.** With a round every 24,700 cycles the transfer thread no longer walks its six stream objects between two instructions of the ADX RPC thread: the race of follow-up 18 is not removed from the module (it would still exist if a thread polled that fast), but the polling no longer happens at a rate the console never had. Not verified on the host-SPU2 path with a saved card (not exercised).

## 2026-10-15, follow-up 21: in the series; the race left open with an early Start

Written by the orchestrator. Patches 0063, 0064, 0068, 0070 and 0071 are in the series and FFmpeg is on in the Linux presets
(commit 028d3f6); the player folder built from it shows the opening movie in the right colours and reaches the title.
The full regression suite on that build: 4 passed, 2 failed. `fight_practice` and `survival_result` (env `DON_PLAY_MOVIES=1`,
their scripts tap Start about 60 blanks into the movie, while the screen is still white) stall at vsync 2580 right after
`E2004090216 : Specified handle is invalid`: the race of follow-up 18 (CRI_ADXI's 0x409 destroy lands mid-round of the
six-object polling thread). 0071 makes it rarer (the save_and_load route passes) but does not remove it.

Three hypotheses were checked and refuted the same night: the decoder stub waiting for a stream that never started (a
"no picture yet" answer changed nothing); the 0x409 handler running while a thread holds interrupts suspended (none
held at its entry); a semaphore or event-flag wait that cannot block inside the synchronously run handler (the handler
takes none). What stays: the port runs the RPC handler synchronously between IOP slices (IopRpcBridge::handleRpc via
executeGuestFunction), not in the module's RPC server thread, so it can land wherever the polling thread was preempted.
Next: log where the polling thread was preempted at each 0x409 (between objects or at the round's end), compare with the
console (does the console print the error with an early Start? the reference emulator, follow-up 12's method), and
consider delivering RPCs through the server thread as the console does. Documented for the owner in RUNNING.md.

## 2026-10-15, follow-up 22: the RPC handler ran at the wrong priority; patch 0072 runs the higher-priority IOP threads first (T-305, sub-agent)

**Measured** (temporary dump of every IOP thread's state, priority and pc at each `handleRpc`; removed; scratch tree `work/t305race/`, own build `build/t305-race`, route `fight_practice` cut at blank 3400). At the 0x409 that prints the error the polling thread is **thread 10, priority 39, state Ready, pc `0x73E28`** (the entry of the wrapper at `0x73E1C`, before its call on the next object): preempted between two objects, not blocked. The thread that serves the ADX RPC queue sleeps in `sceSifRpcLoop` with **priority 48** (thread 9, pc `0x6E83C`, Sleep). At the 0x408 of the same run thread 10 was blocked in `WaitSema` (state Semaphore, pc `0x72400`), so the pair is safe whenever the transfer thread is waiting.
**Cause.** `IopRpcBridge::handleRpc` runs the handler synchronously on a fresh CPU at the moment the EE side calls it, so it lands wherever a runnable IOP thread was left at the end of its 256-cycle slice. On the console the handler runs in the server thread (priority 48), which gets the processor only when every thread with a smaller number is blocked: a Ready priority-39 thread finishes its round (it blocks in `WaitSema`) before the destroy runs. Follow-up 21's "polled mid-round" is therefore not a console race at all; it is the port's missing priority rule. The console oracle was **not** run in this task (time box): the reasoning above is from the port's thread table and the kernel's rule; the console's own thread priorities were not read.
**Fix** (patch 0072, in `patches/ps2recomp/`, NOT yet in `series`: the task forbade editing it). `sceSifSetRpcQueue(queue, thread)` and `sceSifRpcLoop(queue)` record the server thread per queue; `handleRpc` looks up its priority and first runs, through the new `IopGuestExecutor::runThreadsUntilIdle`, every Ready thread whose priority number is at most the server's, for at most 200,000 IOP cycles, then the handler. A server with no known thread behaves as before. The handler still runs on a fresh CPU, not on the server thread's stack (not the console's mechanism in detail).
**Results.** Scratch tree = PS2Recomp pin + series + 0072 (applies from clean). `DON_BINARY=build/t305-race/don ... -k "survival_result or fight_practice"` (slow, FFmpeg): **2 passed** (the two sound-only variants skipped). `tests/runtime/iop_rpc_server_priority_test.cpp` (new, in `tests/runtime/CMakeLists.txt`): fails with the wait set to 0 (handler saw the worker inside its run), passes with the fix; `iop_no_preempt_test`, `iop_intr_suspend_test`, `iop_spu2_dma_time_test` still pass. The other four routes and the full suite were not run (orchestrator, at integration): the change adds up to 200,000 IOP cycles of IOP time per RPC to a server whose thread has a Ready higher-priority thread, which can move IOP timing for every RPC.
**Open.** Patch 0071 may now be redundant for this race (the polling no longer lands mid-round whatever its rate) but it is still the console's DMA time; keep it. Oracle on the console (early Start, IOP console in `emulog`) and reading the console's priority of the ADX server thread would confirm the premise.
