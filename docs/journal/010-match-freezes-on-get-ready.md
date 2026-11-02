# 010 · The practice match stops on the first "GET READY" screen

- **Date:** 2026-10-13
- **Task:** T-202
- **Status:** open

## A defect in the vector-unit glue that was live during these runs (name this first)

All the runs below were made with `build/linux-wire/don` and `build/linux-prof/don` built **before** the
fix of the defect in `src/host/vu_glue.h` described in journal 012: `HostVu` held a reference to a
`DonVuApi` table that `host.cpp` declares inside a block that had already ended, so its callbacks read a
dead stack slot. With `DON_VU1=microvu` alone the slot happened to keep its contents, which is why the
runs work at all, but **the contents of that slot depend on what the stack held before, that is on the
run's timing**: this is the first thing to suspect for the frame at which the intro advances in one run and
not in the others. A run of this route with a binary built after the fix is what tells the freeze and the
variation apart, and neither was done here.

## Symptom

With the new route `tools/run/scripts/to_match_1v1.txt` (the route of `to_main_menu.txt`, then the
single-player part of the reference route: Circle on ひとりでバトスタ, Cross back, Down twice, Circle on
プラクティス, Circle twice to confirm Naruto (1P) and Sasuke (Com), the stage list with
ゴーイングメリー号 selected, Circle) the port reaches the match: the loading screens run and the first
match frame shows the "GET READY" overlay with the HUD. From then on **every presented frame is
identical** until the script quits:

```
$ DON_VU1=microvu DON_VU_LOG=1 DON_HOST_LOG=1 build/linux-wire/don --headless --renderer software \
    --iso "$DON_ISO" --seconds 1800 --shots work/shots/match-r1 \
    --input-script tools/run/scripts/to_match_1v1.txt disc/SLPS_256.75      # exit 0, 14m07s
[don] GS: 7800 vblanks, 20.91 ms in the GS per vblank (max 2241.58), 2102.5 MiB of GIF data
[don-vu] VU1 (microvu): 1127055 programs started, 0 continuations, 2561581029 cycles, 1468740 XGKICK packets, 2090790800 bytes

$ md5sum work/shots/match-r1/match_{load_2,intro,1v1,2,3}.png
d1e6c5c19f21547108d1296ee519439e  match_intro.png    (frame 6840)
d1e6c5c19f21547108d1296ee519439e  match_1v1.png      (frame 7140)
d1e6c5c19f21547108d1296ee519439e  match_2.png        (frame 7440)
d1e6c5c19f21547108d1296ee519439e  match_3.png        (frame 7740)
d1e6c5c19f21547108d1296ee519439e  match_load_2.png   (frame 6540)
```

1260 frames (21 s of game time) with a byte-identical picture. The frame shows the HUD (NARUTO 1P with a
red bar, SASUKE Com with a blue bar) and the big "GET READY" text with "1P"/"Com" labels; the stage (the
Going Merry's deck, sea and sky on the console) and the two fighters are **not drawn at all** — the
background is the same magenta/purple geometry the title, the menu, the character select and the stage
list show (journal 008). `match_1v1` (frame 7140) against the console's dump replay:
`SSIM=0.1865 MAD=103.11`; the console's own screenshot gives the same figures to two decimals.

## Classification

`uv run --no-project python -I tools/run/classify_log.py work/orchestrator/t202/run-match-r1.log` ->
`NO_PROGRESS`; no warning or error line in the log at all (179 lines, the most frequent ones folded by
the classifier: none). No `MISSING_FUNCTION`, no `MISSING_BRANCH_TARGET`, no exception, no runtime
message; the run ended by the script's `quit`, not by an error.

## Investigation

* The guest is **not idle**: the GS time per vblank rises from 11-13 ms in the character select to
  24.6 ms at frame 6600 and stays at 21-24 ms to the end (max 2241.58 ms, one vertical blank spent more
  than two seconds inside the GS calls), and the run sends 2.1 GiB of GIF data. A guest spinning in a
  wait loop that draws nothing would not do that.
* The picture does not change, so whatever the guest draws, the frame that is latched and shot is the
  same one every time: either the guest re-draws the same static frame forever, or it draws new frames
  that never reach the latched picture.
* Frames around the stop: 5940 the stage list (Circle), 6000-6480 the loading and the intro (the picture
  changes every 60 frames), 6540 the first "GET READY" frame, then nothing.
* **The stop is not deterministic: it depends on the run's diagnostics, not on the binary.** Four runs
  of the same script, same `DON_VU1=microvu`, same shots:

  | run | binary | diagnostics | frame 6600 |
  |---|---|---|---|
  | r1 | build/linux-wire/don | none | `md5 d1e6c5c1…` GET READY, frozen to 7800 |
  | prof | build/linux-prof/don | `DON_PROFILE=work/profile/match.txt DON_TRACE_THREADS=1` | `md5 62a8f537…` **FIGHT** (the intro moves) |
  | prof2 | build/linux-prof/don | none | `md5 d1e6c5c1…` GET READY, frozen to 7800 (byte-identical to r1) |
  | trace | build/linux-prof/don | `DON_TRACE_THREADS=1` | see below |

  Frames up to 6300 are byte-identical in all of them, and r1 and prof2 (different binaries, no
  diagnostics) are byte-identical *everywhere*, including the VU1 summary (1127055 programs started,
  2561581029 cycles, 1468740 XGKICK packets). So the scheduler patch does not change what the game does;
  the run that goes past "GET READY" is the one with `DON_PROFILE` and `DON_TRACE_THREADS` on, i.e. the
  slowest host. Shots: `work/shots/match-r1/frame_006540.png` (GET READY, frozen) against
  `work/shots/match-prof/frame_006600.png` (FIGHT). Which of the two diagnostics is responsible, and
  through what (host timing, the sampler's own thread, the trace's printing slows or reorders something),
  is not determined here.
* Thread table at vblank 6600 of the linux-prof run (`DON_TRACE_THREADS=1`), the run in which the intro
  advances:

  ```
  [don-threads] vsync 6600 running=0 threads=5
  [don-threads]   t1 ready prio 0/24 pc=0x0031587C ra=0x0031587C entry=0x00100008 wait=0/0 susp=0
  [don-threads]   t2 susp  prio 1/8  pc=0x00314568 ...
  [don-threads]   t3 wait  prio 1/16 pc=0x0010C1C8 ...
  [don-threads]   t4 wait  prio 1/18 pc=0x0010C1C8 ...
  [don-threads]   t5 waitsusp prio 1/25 pc=0x0010C1C8 ...
  [don-threads]   sema 1 count=1 max=1 waiters=0
  [don-threads]   sema 2 count=1 max=1 waiters=0
  ```

  Nobody is blocked on a resource and both semaphores are free: the main thread is ready and running.
  The thread table of a *frozen* linux-wire run was not captured (the run with the trace was stopped
  early), so this is the table of the run that goes on; it is reported as what it is.
* A fragment of the code around 0x0031587C read from the ELF with a small rabbitizer script
  (`work/orchestrator/t202/disasm.py`) is an ordinary function: it increments a counter at 0x003D1208,
  reads a flag at 0x003D23FC and calls other functions — not a tight poll of a hardware register, so the
  address on its own does not name what is being waited for.
* Profile of the linux-prof run (`DON_PROFILE=work/profile/match.txt`, 184238 samples of the game thread
  over 927 s of wall time and 187 s of its processor time, 0 lost; the reader's symbol names are 4 KiB
  early until another agent's fix, so only the shares are reported):

  | subsystem | share |
  |---|---|
  | I/O processor emulator | 47.2 % |
  | other runtime code | 19.8 % |
  | scheduler | 15.6 % |
  | C and C++ libraries | 6.4 % |
  | recompiled game code | 5.7 % |
  | graphics library (libdon_gs) | 1.8 % |
  | vector unit (microVU) | 0.0 % |

  In the match stretch the time is in the port's own machinery (the IOP emulator, the scheduler, the
  timer advance), not in the game's drawing code or in the GS library; patch 0034's own counter reports
  3,537,830,718 dispatches for the run, that is hundreds of thousands of EE dispatches per vertical blank.

## What the frozen frame shows, and what is missing

The frame is right where the console's match begins: the HUD (the two bars, the NARUTO 1P and SASUKE Com
plates with their portraits) and the big blue "GET READY" with the 1P/Com labels under it. What is
missing is the whole 3D scene: the Going Merry's deck, the sea, the sky and the mast of the console's
stage, and the two fighters themselves (on the console they stand on the deck while "GET READY" is on
screen). Behind the HUD there is the same flat magenta/purple triangle geometry the title, the menu, the
character select and the stage list draw (journal 008, and its T-202 follow-up for those screens).

So on the way to the match the port draws every 2D element correctly (logos, portraits, icons, text,
HUD, menu plates) and nothing of the 3D world, and this frame is where that becomes fatal: the intro
never advances past the first frame.

## Cause

Not determined. What is measured is that the stop is **not a deadlock** and is **not deterministic across
runs of the same script**: no thread waits on anything, the semaphores are free, the GS keeps receiving
data, and one of the four runs (the slowest host, with `DON_PROFILE` and `DON_TRACE_THREADS` on) goes past
"GET READY" to "FIGHT" at the same frame where the three others stay still. So the game can pass this
point, and what is missing is not a function: it is a wait whose outcome depends on run-to-run timing.
Whoever takes this next should look for it there (and should expect the frame at which the intro advances
to move with the host's speed). The classifier says `NO_PROGRESS` and the runtime prints no error at all.

## Fix

None yet. This is a blocker for T-202: the match cannot be played or measured past this frame.

## Verification

Not fixed. The route and the shots that show the stop:
`tools/run/scripts/to_match_1v1.txt`; `work/shots/match-r1/` with `build/linux-wire/don` (the named shots
and one shot every 60 frames) and `work/shots/match-prof/` with `build/linux-prof/don`; the logs
`work/orchestrator/t202/run-match-r1.log` and `run-match-prof.log`; the profile
`work/profile/match.txt` (written by `build/linux-prof/don`).

## Temporary diagnostics

`DON_TRACE_THREADS=1` and `DON_PROFILE=work/profile/match.txt` on a second run of the same route
(`work/orchestrator/t202/run-match-prof.log`, `work/shots/match-prof/`, the binary `build/linux-prof/don`)
for the thread table and the profile of the match stretch. Nothing was changed in the code for this
investigation, and nothing was left running.

## Follow-up (T-304, 2026-10-13 afternoon): with 0034, microVU and sound on, the route stops on the menu, before the match

The configuration that is to be the port's default from tomorrow (patch 0034 in the runtime,
`DON_VU1=microvu`, `--sound on`) was run on this route twice
(`bash work/orchestrator/t304/run-match.sh 1|2`, the route
`work/orchestrator/t304/to_match_sound.txt` = this script with a shot every 60 blanks from the stage list
on; shots in `work/shots/match-sound-1` and `-2`). **Both runs are byte-identical, and both stop on the
main menu, 2,300 frames before the match.**

* both exit 0, by the script's `quit` at frame 8460; 8400 vblanks presented; wall 239.7 s and 234.0 s (the
  machine shared, load averages 5–8, the other two agents running the game);
* the pictures, byte for byte: `main_menu` (3060) `9f78898663fd…`, `charselect` (3600) `6b4aceb66311…`,
  and then **`menu_again` (4140), `charselect_practice` (4980), `stage_select` (5880) and every shot from
  6000 to 8400 are one and the same picture, `20ad79e1b438…`** — 47 shots over 40 s of game time with a
  byte-identical picture;
* that picture is the **main menu**: SSIM 0.5114 / MAD 53.47 against `work/gsdump/02-menu-replay.png`, the
  same figure the ordinary menu screenshot gets in the follow-up of journal 008 above;
* the recording is **silent after second 52**: sound only in seconds 40–52 (the title and menu music, peak
  0.020691), and nothing at all afterwards (`max peak after 60 s: 0`).

So with sound on the route never reaches the match: the picture stops changing at the menu, right after
the Cross that leaves the single-player character select, and the taps that come after it (Down, Down,
Circle, Circle, Circle, Circle) change nothing, while without sound the same route reaches the match in the
runs of this entry above. The "intro that waits for the announcer's voice" idea is **not** the explanation
for this stop: there is no voice in the recording and the stop is long before the intro.

### Where the two configurations diverge, and what the stalled guest is doing

The same frames of the two runs, byte for byte (mine with sound, this entry's `match-r1` without):

| frame | screen | with sound | without sound (`match-r1`) |
|---|---|---|---|
| 240 (and 60…2400 periodic) | dialog, logo, video | identical (43 of 45 periodic shots) | — |
| 2640, 2700 | title | **0.9965 SSIM / 0.15 MAD** (the title's animated background) | — |
| 3060 | main menu | `9f788986…` | `9f788986…` (identical) |
| 3600 | single-player character select | `6b4aceb6…` | `6b4aceb6…` (identical) |
| **4140** | menu after Cross | `20ad79e1…` **and every later shot** | `be762ca9…` (the menu, and it goes on) |
| 4980 | practice character select | still `20ad79e1…` | `812f78e6…` |
| 5880 | stage list | still `20ad79e1…` | `7ca15d83…` |

So the two configurations are identical up to and including the character select and diverge exactly at
the return to the menu (frame 4140). With sound the guest then sits on the menu for 45 shots / 40 s of game
time.

What the stalled guest is doing (run 3, `DON_TRACE_THREADS=1`, `DON_DUMP_RAM`, log
`work/orchestrator/t304/match-3.log`, dump `work/orchestrator/t304/match3-ram.bin`):

```
[don-threads] vsync 8400 running=0 threads=5
[don-threads]   t1 ready    prio 0/24 pc=0x0031587C ra=0x0031587C   (the game's main thread)
[don-threads]   t2 susp     prio 1/8  pc=0x00314568
[don-threads]   t3 wait     prio 1/16 pc=0x0010C1C8
[don-threads]   t4 wait     prio 1/18 pc=0x0010C1C8
[don-threads]   t5 waitsusp prio 1/25 pc=0x0010C1C8
[don-threads]   sema 1 count=1 max=1 waiters=0
[don-threads]   sema 2 count=1 max=1 waiters=0
[don-iop] vsync 8400 cycles=5162405438 instructions=165579480 instrPerSecond=933934 modules=14 rpcServers=8
[don-iop]   service MCSERV active=0   service dbcman active=0   service libsd active=0
```

* **Not a deadlock**: the main guest thread is `ready` (the scheduler runs it every vblank), both
  semaphores are free with no waiters, no thread waits on a resource another thread holds.
* The main thread sits in the same function as in the frozen runs of this entry (pc=0x0031587C, the
  function with the counter at 0x003D1208 and the flag at 0x003D23FC). The flag is 0 in my stalled dump
  **and** in the console's RAM at a match in progress (`work/ref/x/04/ee_ram.bin`), so it is not a
  "waiting for a flag to be set" wait; the counter differs (0x20FE vs 0x2971) as a counter at another
  moment should. `0x003D1210` differs too (`02010001` vs `02010501`), a byte that looks like a *selected
  stage/mode* byte (0 at the menu, 5 in the console's match).
* The **IOP keeps running** at 0.93–1.03 M instructions/s right to the end (the sleeping IOP of journal
  011, not a stopped one), and no RPC server is left `active`, so no SIF RPC is stuck.
* The **sound stops**: the recording of this configuration has sound only in seconds 40–52 (the title and
  the arrival at the menu, peak 0.020691) and **nothing at all after second 53** (the character select and
  everything after it). The menu BGM that plays at the menu does not come back when the menu returns.

Put together: the stop coincides with the **first BGM stop/start** of the route (leaving the menu for the
character select and coming back). With sound on, the music stops there and never resumes, and the guest
stops advancing at the return to the menu, spinning and drawing the same frame, with nothing blocked. The
likely shape is a handshake with the game's own sound driver (CRI/`sdrdrv`) that the port's chip does not
complete — the driver's stop happens (the music stops) and whatever the game waits for before going on does
not. **This is a hypothesis, not a proven cause**: what is measured is the divergence frame, the silent
recording, the not-blocked guest and the running IOP. Nothing was changed for it.

For this entry's own question (does the intro advance?) the answer stays as it was: with the *default*
(the runtime's interpreter) and without sound, this route reaches "GET READY" and freezes there; with
patch 0034, microVU and sound together it does not get that far.



## Follow-up (2026-10-13, journal 020): on the build with 0037 and 0038 (before 0039)

Without sound, `DON_VU1=microvu`, the route `work/3d/match_long.txt`: three runs, each passes "GET READY",
fights two rounds, and its 156 shots are byte-identical to the others and to the side build's. The stop on
"GET READY" was not seen in these three runs (the earlier three in four were on builds where the host was
late at every frame of these screens and without 0038). Not repeated on the build with 0039.
