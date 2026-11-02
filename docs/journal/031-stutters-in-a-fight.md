# 031 · The stutters in a fight (T-305)

2026-10-14. The owner played in a window (Vulkan, vertical sync on, real sound) and reported micro-jumps.

## What was added

`DON_PACING_LOG` (host, `src/host/pacing_stats.h`, test `tests/host/pacing_stats_test.cpp`) and patch 0056
(`EeScheduler::pacingWaitNs()`, the nanoseconds spent in the two `wait_until` calls). For each blank it
records the interval and its split: renderer (the GS vsync call), pacing wait, and the rest (the game
thread's work). `DON_PACING_LOG=N` records from blank N on. The worst sound write on the main thread is
reported too.

## What could be reproduced

- `--headless --renderer vulkan` with DISPLAY unset works, on the real card (AMD Radeon RX 6800 XT, RADV).
  The shader and pipeline caches are kept between runs (`build/gs/data/PCSX2/cache`, `vulkan_shaders.*`,
  `vulkan_pipelines.bin`), so pipelines are not compiled again in every session.
- A window under Xvfb does not: the bridge fails with "No DRI3 support detected - required for
  presentation". No headless Wayland compositor is installed; Xephyr would need the owner's display.
  So **the windowed present (FIFO, vertical sync) was not measured**: the closest run is headless Vulkan on
  the real card, which has no present.

## Measurement: practice match, 3,651 blanks from blank 7400 (about 60 s), scripted inputs (`work/t305/fight.txt`, local), sound on

| Renderer | median | p99 | p99.9 | max | > 20 ms | > 33 ms | > 50 ms |
|---|---|---|---|---|---|---|---|
| software (run 1, machine busy) | 16.66 | 19.31 | 40.74 | 78.27 | 26 | 5 | 1 |
| software (run 2) | 16.67 | 18.20 | 21.52 | 21.97 | 19 | 0 | 0 |
| Vulkan, headless | 16.67 | 16.76 | 19.16 | 20.41 | 1 | 0 | 0 |

In every worst blank the pacing wait is 0 and the renderer call is 0.1 to 0.5 ms: the lateness is the game
thread's own work (guest code and GIF handling, where a pipeline compile would also show), 18 to 22 ms in the
worst. Run 1's 78 ms blank did not repeat (other agents were using the machine). The worst sound write on
the main thread is 0.01 to 0.02 ms: the sound device is not blocking. Vulkan was the smoother of the two.

## Diagnosis

Without a display, nothing measured explains the owner's report: headless Vulkan has 1 blank in 3,651 later
than 20 ms. What is left, and not measurable here, is the present of a windowed run. The loop sleeps to a
60 Hz grid of its own and then the renderer's present (FIFO) waits for the display's refresh: two clocks. On a
display that is not 60 Hz (75, 120, 144 Hz) FIFO shows each picture for a whole number of refreshes, which
looks like exactly the "micro-jumps" described; on a 60 Hz one the two clocks drift by one frame every few
seconds. This is a hypothesis, not a measurement.

## What the owner can do to settle it

Run the window with `DON_PACING_LOG=1` (`tools/launcher/launch.sh en-jpvoice -- --sound on`, variable in the
environment) and send the summary line and the ten worst: the "renderer" part of each blank is the present's
wait. Also `DON_HOST_VSYNC=0` (present without waiting) shows whether the hitches go away, and the display's
refresh rate is needed. No change was made to the pacing or the present mode: nothing certain to change yet.

## Follow-up, 2026-10-14: a real window, measured

**A window without the owner's display.** `tools/run/headless_desktop.sh` starts `gnome-shell --wayland
--headless --virtual-monitor 1920x1080@R` in its own D-Bus session and runtime directory (DISPLAY unset), and
the game opens a real window in it with the Vulkan renderer on the RX 6800 XT: start-up lines
`VK_KHR_wayland_surface`, `Creating a swap chain with 4 images in present mode VK_PRESENT_MODE_FIFO_KHR`,
`GS open ... (vulkan renderer, window)`. The compositor reports the monitor at the requested rate. Limits: the
rate is honoured up to 60 Hz (30 and 50 Hz throttle the game as a real display would); at 75, 100 and 144 Hz
the compositor reports that rate but repaints on each commit and does not quantize (release gaps stay at
16.7 ms), so the judder of a 60 fps game on a faster display is not reproducible here. `--x11` (the compositor's
own Xwayland) was written but not tried. No sound device is visible inside (the private runtime directory hides
the sound server), so sound is off in these runs; the sound write is not on the game thread's path.

**What the window changes (same 60 s fight, 3,651 blanks from blank 7400, Vulkan).** `DON_PACING_LOG` now also
prints the mean split and, per blank, how much of the work is the GS library's transfers.

| Case | mean work (GS transfers) + renderer + pacing wait | p99 | max | > 20 ms | > 33 ms |
|---|---|---|---|---|---|
| headless, no window | 13.22 (1.92) + 0.10 + 3.35 | 16.98 | 21.0 | 3 | 0 |
| window, FIFO (the old default), 4 runs | 14.38 (2.07) + 2.29 + 0.00 (one run) | 17.6 to 19.4 | 25 to 49 | 6, 9, 17, 28 | 0, 1, 1, 0 |
| window, no-wait present (mailbox) | 13.36 (1.88) + 0.18 + 3.13 | 16.81 | 22.3 | 4 | 0 |
| same, after the change (default) | 13.03 (1.84) + 0.15 + 3.49 | 16.77 | 23.9 | 2 | 0 |

(FIFO at 59.94 Hz: 28 late, max 25 ms; at 75/144 Hz the same as 60.)

**Is the two-clocks beat there?** Not as a regular period, and not as a wait that grows: the present's wait
never exceeded 21 ms and no late blank repeats at a fixed spacing. What is there is smaller and real: with the
present waiting for the display, the renderer part averages 2.3 ms (up to 8 to 21 ms in single blanks) and
eats the loop's whole slack (pacing wait 3.3 ms to 0.0), so the game thread, whose work is about 13 to 14 ms of
the 16.7, has nothing left to absorb its own slow blanks (the 18 to 25 ms ones: guest and IOP work, only 2 to
3 ms of it is the GS) and a late blank is a late picture. Late blanks went from 3 to 4 (no wait) to 6 to 28
(FIFO), the maximum from about 22 ms to 25 to 49 ms.

**The change.** `DON_HOST_VSYNC` is now off by default in a window: the present uses the swapchain's mailbox
mode (confirmed in the log: `present mode VK_PRESENT_MODE_MAILBOX_KHR`; no tearing) and the loop's own clock
is the only pace. `DON_HOST_VSYNC=1` selects FIFO, the old behaviour. Cost: if a driver offers no mailbox mode
the renderer falls back to immediate (tearing); RADV on Wayland offers it. The headless regression suite does
not see this change (without a window the vsync mode was already off); it was not run.

**Not verified.** The owner's set-up: X11 (DRI3 present, not Wayland), their window manager and compositing,
and their monitor's rate (a display above 60 Hz shows a 60 Hz game unevenly whatever the present mode: only
a variable-refresh display or a matching rate removes that). Whether the owner perceives the improvement.
Run-to-run spread of the late blanks is large (6 to 28 in four FIFO runs); the machine had other agents running.

## Follow-up 2026-10-14 (T-203): where a fight's time goes

Measured with `DON_PROFILE` (1 kHz, thread CPU time) on the 60 s practice fight (3,651 blanks), Vulkan headless, sound on,
as the full run minus the run that stops at the route's end (same script, deterministic). Mean game-thread CPU 12.2 ms per
blank, by subsystem (ms per blank):

- scheduler (`EeScheduler::run`, `processPendingEvents`, `accountCycles`, branch dispatch, function lookups): 3.7
- other runtime code (memory helpers, `advanceEeTimers`, VIF1 data, ...): 2.5
- recompiled game code: 2.0
- graphics library (libdon_gs): 1.3
- samples outside the module map (microVU JIT code and stubs): 1.1
- IOP emulator 0.55; SPU2 mix and host sync about 0.3 (sound in total about 0.9 ms, 7 %); C/C++ libraries 0.6; unknown 0.4.

The runtime's per-dispatch plumbing, not the game, is the largest block: about 5 ms per blank against 2 ms of game code.
Inside `EeScheduler::run` the two hottest places were `currentThread()` (an `unordered_map` lookup, a hash and a division, at every
iteration) and the debug-context stores.

The ten worst blanks (`DON_PROFILE_BLANKS=1` writes the blank of every sample): 17.5 ms of samples against 11.4 on average,
and every subsystem scales together (game code 3.5 vs 2.0, graphics 2.25 vs 1.3, VIF1 1.1 vs 0.65, timers 0.75 vs 0.35,
sound unchanged). Not one subsystem spiking; no loading read, no sound burst. A slow blank is a heavier scene.

Change made (patch 0060, kind (a)): `currentThread()` caches its last answer, and `run()` tests the checkpoint flag before
calling `processPendingEvents()`. Fight mean work 13.3 -> 11.5 ms per blank, thread CPU 119.8 -> 103.3 s, worst blank 25.7 ms
(one run each; p99.9 17.8-19.3, later than 20 ms: 13 before, 1-2 after).

Tried and dropped: merging `hasFunction` + `lookupFunction` into one table read (`findFunction`). Measured 11.33 ms, but
the state lines then differed from the baseline in the IOP cycle counters (sound off) so it is not kept. NOTE: the same
differences appeared again after reverting it, so they come from the working tree having IOP changes from another task
(patch 0059 in progress), not from `findFunction`; this was not separated. The state comparison of patch 0060 that counts
(identical in all four cases: quit70 and the route, sound off and on) was made before those changes were in the tree.
The regression suite was not run.

Not done (kind (b), the orchestrator's decision): the cycles charged per dispatch and how often the sound processor runs.

## Follow-up 2026-10-14 (T-303): the sound several seconds late

Report from the owner: in a window, with the music and effects now playing, the sound comes several seconds late.

**Measured** (`DON_AUDIO_LATENCY=1`: every 5 s of host time, the sound queued for the device = ring + SDL stream, the frames
produced per second, the blanks per second; `SDL_AUDIO_DRIVER=dummy`, which consumes at the device's rate and plays nothing;
the main-mode route `work/soak/r8.txt`, 300 s, Vulkan; the window cases in the private desktop of `headless_desktop.sh`):

| Case (before the change) | queued max / mean | blanks per s min / mean / max | frames per s mean | dropped |
|---|---|---|---|---|
| window, mailbox | 89 / 21 ms | 59.2 / 59.98 / 60.2 | 47,997 | 0 |
| window, `DON_HOST_VSYNC=1` | 49 / 20 ms | 54.1 / 59.74 / 60.2 | 47,801 | 0 |
| headless with the device opened (`DON_AUDIO_DEVICE=1`) | **1,245 / 319 ms** | 29.2 / 48.9 / 73.5 | 39,136 | 385,994 |

On this machine both window runs kept up with 60.0 blanks per second, so they showed no delay. The headless run, on a
loaded machine, shows it: the game fell to 30 blanks per second for a minute and a half (the sound had holes), then from
t=225 s ran at 63 to 73 blanks per second for more than a minute and the queue sat at 1.23 s with frames dropped all the
time. Reproduced on demand by suspending my own process for 10 s at t=25 s (`work/t303/stall.sh`, not versioned): the queue
went to 1.24 s, 414,000 frames were dropped, and after the game was back at 60 blanks per second the queue still held 0.8 s
at t=80 s, draining only by the difference of the rates.

**Cause.** The game thread's 60 Hz clock is absolute: when the game falls behind (a load, a loaded machine, a stall) it runs
without waiting, above 60 blanks per second (up to 128 here), until it has caught up. Nothing paced it to the sound: the
chip is advanced by the game's own cycles, so the sound is produced at the speed of the game and played at 48,000 frames per
second. The excess piled up in the ring (1 s, then it drops its oldest frames) and SDL's stream (0.25 s), and stayed there:
a late sound by that amount for as long as the catch-up lasts and, since the queue only drains by the difference of the rates,
long after it. Last night the present waited for the display (FIFO), which capped the catch-up at the display's rate and kept
the game thread slower, so it fell behind less often; the mailbox default lets the game catch up at full speed. The owner's
"several seconds" is this 1.25 s plus whatever their sound server's own buffer adds, which this machine cannot measure.

**Fix.** With a device, the game thread waits at every vertical blank (1 ms at a time, `HostGs::set_vblank_hook`) while more
than 60 ms of sound is queued (`DON_AUDIO_TARGET_MS`); a device that does not drain for 8 blanks in a row turns it off with
one line; `DON_AUDIO_PACING=0` restores the old behaviour. Chosen over dropping or stretching samples because it changes
nothing in the sound and nothing in what the game computes: it only holds the game back, and the sound becomes the clock
exactly when the game would otherwise outrun it (a game slower than real time is not touched).

After (same cases; the stall test is the proof of the bound):

| Case (after) | queued max / mean | blanks per s min / mean / max | frames per s mean | dropped |
|---|---|---|---|---|
| window, mailbox | 77 / 35 ms | 53.0 / 59.99 / 61.1 | 48,001 | 0 |
| window, `DON_HOST_VSYNC=1` | 37 / 21 ms | 51.0 / 59.81 / 60.2 | 47,860 | 0 |
| headless with the device | 33 / 19 ms | 59.4 / 59.99 / 60.2 | 48,001 | 0 |
| headless, 10 s stall at t=25 s | 77 / about 60 ms after the stall | 60.7 after (was 128, 92, 79) | 48,600 | 0 (was 414,296) |

The headless "after" run did not meet the slow phases of the "before" run (the machine was less loaded), so the stall rows
are the comparison that matters.

**State lines.** `DON_TRACE_STATE=60` on `work/det/quit70.txt`, sound on: `iopCycles=38186506 iopInstructions=1328160
ramHash=f88d021904cda385`, identical with no device, with the device and the wait off, and with the device and the wait on.
Sound off: the line is not touched by this change (no audio, no wait), but it differs from the one of T-203's earlier
baseline (`iopCycles` 39,055,054 against 38,903,537 now) because the working tree has IOP changes of other tasks; not
separated here.

**Not solved, found on the way (the orchestrator's note about recordings).** Three `--sound on --wav` recordings of the same
route (`work/soak/r8.txt` up to blank 3700, headless, software renderer, no device): two identical and one 27 frames longer.
The difference is not frames lost or added by the ring: the host never drops for the WAV and the library's counter is not
involved. In the third file the sound that starts at 38.97 s (frame 1,870,549) starts 27 frames (0.56 ms) later than in the
other two, and everything after is shifted, so a register write reaches the chip at a different chip time depending on the
host's timing (how the IOP's cycles are batched to the chip, i.e. where `advance` is called relative to the writes). It
happens with no device, so it is not the delay's mechanism. One candidate to check: host-dependent boundaries of the IOP
batches in the scheduler.

**Not verified:** the owner's real sound device and server (their buffer is on top of the 60 ms); the regression sound check
(see the report: it fails on a frame loop that stops from blank 2220 to 5280 with sound, in headless runs where this change
is not active; it passed at the end of journal 011).

## Follow-up 2026-10-14 (T-303): the recording repeatable with sound

Question: with `--sound on --wav` two runs of the same route gave different recordings (27 to 139 frames apart, identical
state lines). Patch 0038 made the guest independent of host timing; was the sound processor's side or the chip's clock not?

**Where two runs part: nowhere on the guest side.** `DON_SPU2_CALLLOG=<file>` (new, `spu2_glue.cpp`) writes every advance of
the chip, every register write and every DMA with the chip's own cycle. Three runs of the route up to blank 3700 (headless,
software renderer, no device): the three call logs are byte-identical (138,460,140 bytes, 8,391,555 advances), so the sound
processor's batches, the slices, the pacing waits and the vertical blanks drive the chip at exactly the same cycles every time:
nothing there depends on host time. The recordings of those three runs were still different: 11,842,660, 11,842,104 and
11,842,276 bytes; run 2 had its first sound at frame 218,525 (4.5526 s) where run 1 had it at 218,664, i.e. 139 frames of
silence missing before it; run 3 equalled run 1 on the common length.

**Cause.** The frames are lost after the chip, in the output ring of the sound library. `don_spu2_reset()` (called from the game
thread when the guest resets the IOP's sound chip, which the game does around 4.5 s and again near 38.9 s) cleared the ring by
setting its head and tail to 0. Whatever the producer had written and the host thread had not pulled yet was discarded: the host
pulls every 8 ms, so up to about 380 frames, 27 and 139 in the runs seen, different every time. (The reset could also race a
pull in flight.) The orchestrator's three files and the 27-frame one differ the same way.

**Fix.** The ring is emptied only in `don_spu2_open()`; `core_power_on()` (open and reset) leaves it alone, so audio produced
before a reset is still pulled, and the reset no longer touches the consumer's counter from the producer's thread
(`src/spu2bridge/don_spu2.cpp`, header comment updated). The chip's state is still reset as before. The library had to be
rebuilt (`tools/pipeline/build_spu2.sh`; `spu2_selftest`: 24 checks, 0 failed). The Windows library is built by the Windows
build and needs the same rebuild.

**Verified.** Six recordings of the route up to blank 3700 (`--sound on --wav`, headless, software renderer): three on an idle
machine and three with 15 busy loops of mine on the 16 processors: all six 11,843,072 bytes, md5 `f0fe1b9e267cc00e...`
(before: three runs, three different files). State lines of `quit70` unchanged: sound on `iopCycles=38186506
iopInstructions=1328160 ramHash=f88d021904cda385`; sound off `iopCycles=38903537 iopInstructions=34280130
ramHash=1bc3b23d1246d942` (the same as before the change; no code of the guest changed). Host and input tests: 25 passed.

**Not verified:** the regression suite's sound check (patch 0063 of another task was in the shared tree, `apply -R --check`
passed, so it was not run); the recording with a device; a longer route than 62 s of game.

## Follow-up 2026-10-14 (T-203): scheduler cuts that cannot change the game: none kept

Question: after patch 0060 a fight's blank costs about 12 ms of 16.67; the candidate cuts were the double function-table read,
the per-dispatch debug-context stores and the EE timers advanced at every dispatch.

- **EE timers: already lazy.** Patch 0035 does it: `advanceEeTimers` adds the cycles to a pending counter and only runs the
  arithmetic when the next event is reached or a timer register is touched. What is left per dispatch is a call, two additions and
  a comparison (the 2,213 samples on one instruction of `accountCycles` are that call's entry). No lazy-versus-eager test was
  written: there is no eager path left to compare with.
- **Cut 1, `findFunction`** (`hasFunction` + `lookupFunction` merged into one table read, dispatch history pushed only when found,
  same behaviour): built and measured.
- **Cut 2, timer fast path inline** (`advanceEeTimers` in the header: no event reached and no mask pending, otherwise the old
  function): built and measured.
- **Measurement** (fight route `work/t305/fight.txt`, headless, Vulkan, sound on, `DON_PACING_LOG=7400`, the machine carrying
  other agents' builds, load 3 to 4), mean work per blank in ms, three interleaved rounds:
  baseline 12.17, 12.03, 13.56; cut 1 (11.38, 12.69; round one ran the baseline by a failed binary swap, discarded);
  cut 2 12.15, 12.47, 11.51. The same binary moves by 1.5 ms between runs, more than any difference between binaries. Earlier
  single runs: baseline 12.45; cut 1 13.10, 12.66; cut 1+2 12.35, 12.08.
- **Result:** no cut shows a gain beyond the run-to-run noise (the saved work is a few hundred microseconds at most, the order of
  magnitude the profile predicted: about 0.2 ms), so by the rule of the task none is kept. The shared tree is back at the series
  (md5 of its diff equals the baseline's); no patch was written. The neutrality runs and the regression suite were not run: there
  is no change to prove neutral.
- **What is left:** `EeScheduler::run` is 20% of the samples by itself and the dispatch rate of the game's own loops (a few
  hundred thousand per blank) is what costs; the cuts that would matter change the dispatch count (running generated callees
  directly instead of returning to the run loop, the "dispatch fusion" of docs/knowledge), which does touch what the scheduler
  decides between dispatches and needs its own task and proof. Measuring such a gain also needs a quiet machine or a counter
  (instructions per blank) instead of wall time.
