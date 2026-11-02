# 011 · The first look at the game's sound: the chip is initialised and fed, and the runs crash before the screens that have music

- **Date:** 2026-10-13
- **Task:** T-304
- **Status:** open

> **Correction (same day, journal 018).** Both crashes recorded in this entry have their cause
> elsewhere: crash 1 is the VU1 glue holding a dead reference to the host's table (journal 012) and
> crash 2 - the one seen near vertical blank 1740 and again later, "one run in three" - is
> `tools/pipeline/build.sh` rewriting a mapped `libSDL3.so.0` in place while the run was taking
> screenshots (journal 018). Neither is a defect of the sound configuration. The measurements in this
> entry stand; the sentences that read a crash as a sound problem (or as a consequence of the VU1 fix)
> are corrected by this note.

## What was run

```
set -a; . ./local.env; set +a
DON_AUDIO_LOG=1 DON_TRACE_SPU2=1 DON_TRACE_IOP=1 build/linux-dev/don --headless --renderer software \
    --sound on --wav work/audio/menu.wav --seconds 1200 --shots work/shots/sound-menu --shot-every 600 \
    --input-script work/audio/to_main_menu_sound.txt disc/SLPS_256.75
```
(`work/audio/to_main_menu_sound.txt` is the committed route with 600 extra vertical blanks at the title
before the Start tap and 1200 at the main menu before `quit`.) The run **crashed with SIGSEGV after ~30
seconds of emulated time** (exit 139), leaving a 30.0 s WAV. Shorter runs (`--seconds 30`) end normally and
give the same data. Variants (each `--seconds 25`):

| combination | exit |
|---|---|
| `--sound on` (with or without `DON_TRACE_SPU2`) | 0 |
| `DON_VU1=microvu`, `--sound off` | 0 |
| `DON_VU1=microvu`, `--sound on` | **139** (SIGSEGV at ~0.85 s) |
| `--sound on`, no VU1 | 0 at 25 s, **139** at ~30 s of emulated time |

`DON_VU1=microvu` could not be used for the sound run because of the first crash; the task's shot check
(title/menu) was therefore done in the earlier, sound-less runs (journals 008/009, where the screens are
known to be reached).

## The chip is initialised, once

`DON_TRACE_SPU2=1` (a `[don-spu2]` line per emulated second, and the control registers every line):

```
[don-spu2] t=1.0s writes=1142 (+1142) reads=473 (+473) dma0=73/149504B dma1=0/0B dmaReads=0
           events1=0 events4=72 events7=0 frames=47867 (+47867) topRegs=v0:240
[don-spu2] t=2.0s writes=1142 (+0)    reads=567 (+94) dma0=167/342016B (+94/192512B) events4=166 frames=95715
...
[don-spu2] t=30.0s writes=1142 (+0)   reads=3192 (+94) dma0=2792/5718016B (+94/192512B) frames=1439820
[don-spu2] writes by group: v0=240 c0=175 k0=12 r0=100 p0=0 v1=240 c1=145 k1=12 r1=100 p1=118
```

* **All 1142 register writes happen in the first emulated second** (the `libsd`/`sdrdrv` initialisation:
  240 voice-register writes = 24 voices x 10 registers per core, 12 key-on/off, 175/145 core-control,
  100 reverb, 118 post-block) and **not one write afterwards** for the rest of the run.
* Register *reads* continue at ~94/s: the IOP polls the chip.
* **A DMA stream into core 0 runs continuously**: ~94 transfers/s of ~2048 bytes = **192 KiB/s**
  (exactly 48000 x 2 ch x 2 bytes: 48 kHz stereo PCM), each with its DMA-end event (`events4`). Core 1
  never gets a transfer. **`events1` (the SPU2 interrupt) is never raised.**
* The chip produces its full 48000 frames/s (`frames` = 47867 in the first second, ~47980 after), i.e. it
  is clocked correctly, and the host pulls all of them (`pull()`): the WAV has them all and is **exact
  silence** (`wav_stats.py`: `rms: 0.000000`, `silent: 1.000000`, 30.0 s of 48 kHz stereo; per-second
  peak and rms 0.000000 for every second).

## What the control registers say (end of the 30 s run)

```
core0 ATTR(0x19A)=0xFF80 DMA=off          MVOL=0x3FFF/0x3FFF KONever=0x00FFFF KOFFever=0x00FFFF
      TSA(0x1A8)=0x0000 ADMAS(0x1B0)=0x0001 ESA=0x000F EEA=0x000F
core1 ATTR(0x59A)=0xFF81 DMA=on  SPU2=on  MVOL=0x0000/0x0000 KONever=0x00FFFF KOFFever=0x00FFFF
      TSA=0x0000 ADMAS=0x0000 ESA=0x0000 EEA=0x0000
every voice of both cores: VOLL=0x0000 VOLR=0x0000 PITCH=0x3FFF ADSR1=0x0000 SSA=0x0000
```

So: the master volumes of core 0 are at maximum and neither core is muted; **all 24 voices of both cores
are keyed on and off once, with volume 0, envelope 0 and sample address 0** (the silent reset the drivers
do at start-up), and after that nothing is programmed again. Core 1's master volume is **zero**, so even
its DMA mode could not be heard.

## The IOP

With the host SPU2 installed, the IOP **sleeps**: `[don-iop]` shows `cycles` advancing by 36,864,7xx per 60
vertical blanks while `instructions` advance by only ~0.73M (about 2%, 0.19M instructions/s) - i.e. the CRI
driver's thread no longer burns the IOP (the stand-in sound service had `cycles == instructions`, journal
007). As a side effect the port runs at ~46 vertical blanks per second at the memory-card dialog instead of
~16: the spin *was* the frame cost.

## The two crashes (both new tonight, with the current build)

1. `--sound on` with `DON_VU1=microvu`: SIGSEGV after ~0.85 s, a call through a **null function pointer**:

```
Program received signal SIGSEGV, Segmentation fault.
#0  0x0000000000000000 in ?? ()
#1  PS2Memory::vu1CodeWritten(unsigned int, unsigned int)
#2  PS2Memory::processVIF1Data(unsigned char const*, unsigned int)
#3  PS2Memory::processPendingTransfers()
#4  PS2Memory::writeIORegister(unsigned int, unsigned int)
#5  PS2Runtime::Store32(...)
#6  FUN_002d3a60_0x2d3a60(...)          <- the game writes the VU1 microcode
```
   So with the sound library loaded, the VU1 "code written" hook is not installed (or cleared) when the
   game first uploads a microprogram. Each alone works, so it is the *combination* (sound + host VU1); the
   host VU1 and its hook are another agent's files, so nothing was changed.
2. `--sound on` alone: SIGSEGV after ~30 s of emulated time, i.e. at **~1740 vertical blanks** (the log's
   last lines are `[don-spu2] t=30.0s ...` then `[don-iop] vsync 1740`; the WAV stops at 30.0 s). The same
   run **without** `--sound` reaches the title and the menu without crashing (journals 008/009), so the
   sound changes the timing and exposes this. Its backtrace is **not obtained**: a gdb run with
   `--sound on --seconds 200` ran more than 8 minutes without crashing (under gdb, on a shared machine), so
   it looks timing-dependent. 1740 vertical blanks is just before the Sofdec/video set-up, i.e. the window
   in which journal 009 found the replaced `sceMpegCreate` writing the game's MPEG structure over a float
   routine of the game's code. Whether the two are the same event is **not determined**; a backtrace (a
   core dump, or a quieter machine) would say.

## Conclusion

* The chain **works up to the chip**: the modules initialise it (once), the real `libsd`/`sdrdrv` feed
  core 0 a 48 kHz stereo stream continuously (192 KiB/s) with DMA-end feedback, and the chip is clocked and
  produces its frames. Nothing is heard because **nothing keys a voice with a volume, an envelope and a
  sample address after the initialisation** - but that is measured only over the first ~30 seconds of game
  time, i.e. the boot, the health warning and the memory-card dialog, where the console has no music
  either. **The title and the main menu (the screens with music) were never reached in a sound run**:
  they need ~45-80 seconds of game time and the run crashes at ~30.
* So the **first thing to fix is the crash** (item 2 above; item 1 for anyone who wants the host VU1
  *and* sound). Then the same run repeated past the title and the menu can answer whether the game and the
  driver key a voice for music (`DON_TRACE_SPU2=1` would show the voice writes and the key-ons at those
  seconds, and the WAV would stop being silent if they do).
* What is **not verified**: the sound of the title/menu (blocked by the crash); whether the 192 KiB/s stream
  is the music, the SDR driver's default transfer or the (black) movie's audio - its consumer was not
  identified (the DMA comes from IOP channel 4, started by the game's own `libsd`/`sdrdrv` code, since the
  HLE libsd service is off with a host SPU2); whether the missing voice programming at the title is a
  driver defect or the game never asking (needs the title, which needs the crash fixed); and the second
  crash's backtrace.

## After the fix: the measurement that was blocked

Crash 1 above is **fixed and understood** (journal 012): the VU1 glue held a reference to the host's
`DonVuApi`, which `host.cpp` had declared in a block that ends before the game thread starts; with sound
loaded the main thread's audio path reused that stack and the table read back as zeros. The reading in
"The two crashes" item 1 - "the hook is not installed (or cleared)" - is **wrong**: the hook was installed
and the table's `write_micro` was valid; the null call was one level deeper, inside the glue's own
trampoline, which read the dead table. `src/host/vu_glue.h` now gives the object its own copy of the
table. With that, `DON_VU1=microvu --sound on` runs, and the vector unit is genuinely used.

The route of this journal, with the fix:

```
set -a; . ./local.env; set +a
DON_TRACE_SPU2=1 DON_VU_LOG=1 DON_VU1=microvu build/linux-dev/don --headless --renderer software \
    --sound on --wav work/audio/menu2.wav --seconds 1200 --shots work/shots/sound-menu2 --shot-every 600 \
    --input-script work/audio/to_main_menu_sound.txt disc/SLPS_256.75
```

**exit 0**, the route's `quit` (4860 vertical blanks) reached; 3888174 audio frames = **81.0 s of emulated
time** (the chip is clocked by the IOP's cycles, 768 cycles per 48 kHz frame), so 4860 blanks / 81.0 s
emulated = 60 vertical blanks per **emulated** second: the blanks and the chip's clock agree. It does not
say the run was at real-time speed; only the wall time of the run does, and it was not timed here (see
"With patch 0034, microVU and sound together" below for a timed run). `[don-vu] VU1 (microvu): 831411
programs started, 0 continuations, 1568381643 cycles, 831411 XGKICK packets, 1217500752 bytes`.

Before the fix this command died at ~0.85 s; the earlier "the WAV is exactly silent" run was
`--sound on` *without* the route, stopped at 30 s, i.e. before the screens that have music.

**There is sound, and it is the music of the title and the menu.** Peak and RMS per second of the WAV
(work/audio/wav_levels.py): seconds 0-39 silent, then from second 40 to the end (81), peak 0.008-0.021
and RMS 0.002-0.005 (about -34 dBFS peak, the loudest second is 41 at 0.0207). Second 40 is the title
(2700 blanks at 60/s ~ 45 s including the boot); the sound continues through the main menu to the quit.
`tools/verify/wav_stats.py`: RMS 0.001966, 52.9 % of the samples non-silent, none saturated.

**Voices are keyed on.** `[don-spu2] SUMMARY writes=1142 reads=7974 voiceWrites=480 dma0=7574/15511552B
dma1=0/0B dmaReads=0/0B events1=0 events4=7573 events7=0 frames=3888174`, and 16 voices started, core 0
voices 0-7 and core 1 voices 8-15. The stream to core 0 carries real data: 15 511 552 bytes in 81 s
(~191 KiB/s, the same 48 kHz stereo rate as journal 011's first measurement) and the chip turns it into
the audible output above. The `voice N started` lines print the registers **at the summary**, so their
values (VOLL=VOLR=0, PITCH=0x3FFF, ADSR1=0, SSA=0) are the state after the game stopped the music, not
the key-on instant; the 480 voice-register writes and the audible WAV are the evidence that the driver
programmed them. `events1` (the SPU2 interrupt) is still **0** for the whole session: the game's driver
uses the DMA-end event (7583 of them) and never the chip's interrupt, and the music plays without it.

Still open in this entry, now with the two engines available: whether the quiet level (-34 dBFS) is the
mixer's own gain (one core's MVOL, the other at 0) or the driver, and what the 192 KiB/s stream is
(the DMA is started by the game's own `libsd`/`sdrdrv` on IOP channel 4; the HLE libsd service is off
with a host SPU2).

## With patch 0034, microVU and sound together (2026-10-13, afternoon)

All of this is one configuration: patch 0034 in the runtime, `DON_VU1=microvu`, `--sound on`. The machine
was shared with two other agents (load average 5–11, one to four `don` processes while these ran), so the
wall figures carry that spread; every run below is from `work/cores` with `ulimit -c unlimited`.

Commands (three identical runs; the binary was relinked by another agent between them, so each records its
own mtime and md5, and a copy of it next to the profile):

```
DON_VU1=microvu DON_VU_LOG=1 DON_HOST_LOG=1 DON_TRACE_SPU2=1 DON_TRACE_IOP=1 build/linux-dev/don \
  --headless --renderer software --sound on --wav work/audio/menu3x.wav --seconds 900 \
  --shots work/shots/i0034-all-x --shot-every 300 \
  --input-script work/audio/to_main_menu_sound.txt disc/SLPS_256.75
```

| run | binary | exit | wall | result |
|---|---|---|---|---|
| 1 | `build/linux-dev/don` 12:48 | **139** | 127.15 s | SIGSEGV at ~3300 vblanks, core read below |
| 2 | md5 2c925d6d… (12:59) | 0 | 252.74 s | the route's `quit` (4860) |
| 3 | md5 2c925d6d… (12:59) | 0 | 263.47 s | the route's `quit` |

### Blanks per wall second, per screen (from the shots' file times)

Runs 2 and 3 agree within 5 %; the table is run 2, with run 3's figures in brackets. The 60.0 of the first
stretches is exact in both runs.

| frames | screen | blanks/s run 2 (run 3) |
|---|---|---|
| 240–2400 | memory-card dialog, warning, logo, the opening video | **60.0** (60.0) |
| 2400–2700 | the video → title transition | 11.3 (11.1) |
| 2700–3000 | **title screen** | 9.7 (10.0) |
| 3000–3300 | title (the route stays 600 blanks longer) | 10.0 (10.0) |
| 3300–3600 | title → main menu | 12.5 (12.0) |
| 3600–4800 | **main menu** (three stretches) | 12.6, 12.8, 12.3 (10.5, 11.2, 11.7) |
| 0–4800 | whole route | 19.7 (18.9) |

So the port is at **real time (60.0) through the boot, the dialog, the logo and the video**, and **5–6 times
slower than real time (10–13) at the title and the menu**.

* The 60.0 stretches are **held by the host clock**, not coincidence: eight consecutive stretches sit at
  exactly 60.0 for 40 s, and the game thread uses only **64.289 s of processor time in 252.510 s of wall
  time (25 %)**, measured by the profiler (`thread GameThread`, 1000 Hz, 62816 samples, 0 lost).
* What limits the title and the menu is the **GS**, not the game thread: the run reports
  `[don] 4800 vblanks, 35.84 ms in the GS per vblank (max 126.68)` — about 36 ms of GS work per vblank at
  those screens, well over the 16.7 ms of a real-time frame. The GS runs on the host thread, so it does not
  appear in the GameThread profile; the game thread's own cost there is far below a core.
* Inside the game thread (profile, subsystem shares): scheduler **31.3 %**, other runtime code 40.2 %,
  recompiled game code 9.6 %, IOP emulator 5.4 %, C/C++ libraries 3.4 %, GS library 2.5 %, outside the
  module map 3.2 %, unknown 4.1 %; the runtime's own VU1 interpreter 0.0 % (as expected with microVU).
  Top symbols: `PS2Memory::advanceEeTimers` 26.2 %, `EeScheduler::run()` 21.5 %,
  `EeScheduler::processPendingEvents` 6.4 %, `PS2Runtime::hasFunction` 6.1 %,
  `PS2Runtime::lookupFunction` 4.5 %, `EeScheduler::accountCycles` 3.0 % — the **scheduler and the timer
  bookkeeping are ~68 % of the game thread**, the game's own code 9.6 %.

### Is it stable and repeatable?

* The **pictures are byte-identical** across the three runs: every named shot (`title`, `main_menu`,
  `main_menu_late`, `memcard_prompt`) has the same md5 in all runs that wrote it, and so do the frames
  300 to 3000 shot every 300 blanks (10 of them) — including run 1, the one that later crashed.
  `title` = `046e02a05641…`, `main_menu` = `9f78898663fd…`, `memcard_prompt` = `761466465467…`.
* The **two clean runs' recordings are byte-identical** (`menu3b.wav` and `menu3c.wav`, md5
  `8603ce5695c01f…`): 81.004 s, RMS 0.001966, 52.9 % of samples non-silent, none saturated, loudest second
  41 (0.020691), and the same first second with sound (41, i.e. the title). Run 1's recording
  (`menu3.wav`, cut at the crash) is 55.004 s with the same loudest second and peak.
* **One crash in three runs**, and it is the one thing here that is not repeatable; see below. It is the
  same family as crash 2 recorded earlier in this journal (a SIGSEGV during a sound run, mid-run), so the
  sentence "not reproduced tonight" above is now "seen once more, at ~3300 rather than ~1740 blanks".

### The crash of run 1 (core read; still not diagnosed)

`work/cores/core.63685`, 616 MB, written at 12:56. The process died with SIGSEGV; the crashing thread's
`rip` is `0x66f80` — **unmapped, and inside the guest's address range** (the runtime's own area below
`0x100000`), i.e. a call or jump through a pointer that held a guest address. Its stack
(`rsp = 0x738c0c5fa338`, just below the mapping of `build/vu/libdon_vu.so`) holds the ASCII
`frame_00` / `3300.png` — the screenshot being written at that moment — and return addresses that
symbolise to `HostGs::on_thread_begin`, `HostVu::on_thread_begin` and `HostVu::emit_summary` (startup and
shutdown frames of the game thread). The shot `frame_003300.png` is exactly the one **missing** from the
shots folder while 240 … 3000 are there, and the run's last log lines are the per-second lines up to
`[don-spu2] t=55.0s` and a GS progress line at 3300 vblanks (about 55 s of emulated time).

**Caveat that limits this**: another agent relinked `build/linux-dev/don` between the crash and the read
(12:48 → 12:56), so gdb could not map the main program's segments and the frame chain of the crashing
thread could not be symbolised; only the stack's strings, the registers and the guest-range `rip` survive.
Runs 2 and 3 (a newer binary) did not crash. So this is **evidence**, not a diagnosis: the crash happened
in the game thread while it wrote the screenshot of frame 3300, and the faulting address is a guest
address, not a host one.

### The pictures against the console's stills, and against the earlier microVU runs

| picture | against | before this configuration | now |
|---|---|---|---|
| `title` (2700) | `work/gsdump/01-title-replay.png` | 0.5326 / 40.92 | **0.5326 / 40.94** |
| `main_menu` (3600) | `work/gsdump/02-menu-replay.png` | 0.5162 / 52.23 | **0.5162 / 52.23** |

So sound and patch 0034 change neither figure; the wrong title/menu background of journal 008 is unchanged.
Against the earlier microVU shots (`work/shots/wire-title-microvu/`, no sound, no 0034): frames
300–2400 are **byte-identical** (SSIM 1.0000, MAD 0.00), frame 2700 is 0.9965 / 0.15 (the title's animated
background advances slightly differently with sound), and 3000/3300 differ (0.08 / 88.3) only because with
sound the route is still on the title at those frames where the earlier route was already in the menu.

### The sound's level: it is the game's own data

`DON_TRACE_SPU2=1` now prints, per emulated second, the peak |int16| of the DMA data the game sends to
core 0 (`dmapeak0`), after the earlier register bug was fixed (core 1's master volume is at 0x788, not
0x764). Measured:

* seconds 1–40 (no music): `dmapeak0 = 0` (the stream is all zeros);
* seconds 41 onwards (the title and the menu, the seconds with music): `dmapeak0 = 650 … 682`, and the
  session maximum `dmapeak0max = 675` (run 1) / `682` (run 2) → **0.0206 … 0.0208 of full scale**, and
  `dmapeak1max = 0` (nothing is sent to core 1).
* the recording of those minutes peaks at **0.020691** (second 41) and its per-second peaks are 0.008–0.021.

The samples the game sends peak at 0.021 of full scale and the recording peaks at 0.021: the level is
**the game's own**, not something our path lost. The path is PCSX2's own mixer output: `spu2Output()` is
called after `ApplyVolume(clamp_mix(Out), Cores[1].MasterVol)` and core 0's output goes into core 1 through
`ExtVol` (Mixer.cpp 553–577), the bridge only converts (`/ INT16_MAX`) and applies PCSX2's own DC blocker,
and core 0's master volume is at full scale (`MVOL=0x3FFF/0x3FFF` in every line of the trace). What is
**not** verified: whether those DMA bytes are raw PCM or ADPCM — if the music driver sends ADPCM (ADX),
the peak of the encoded bytes is not the level of the decoded voice, and `dmapeak0` happens to be close to
the output peak; the coincidence would then be misleading. Nothing was changed except the diagnostic.

### The match with sound: the route stalls on the menu (Part 3)

The same configuration on `work/orchestrator/t304/to_match_sound.txt` (the route of journal 010 with a
shot every 60 blanks from the stage list on) was run three times: three times exit 0 by the script's
`quit`, 8400 vblanks, wall 239.7 / 234.0 / 230.0 s. **It never reaches the match**: the picture stops
changing on the **main menu** at frame 4140 (byte-identical in all three runs, `20ad79e1b438…`, the same
picture as the console's menu to SSIM 0.5114) and stays there for 47 shots and 40 s of game time. The
recording has sound only in seconds 40–52 (the title and the menu, peak 0.020691) and **nothing at all
from second 53 on** — no announcer voice, so journal 010's "the intro waits for the announcer's voice" is
not the reason here. Framewise, this configuration and journal 010's no-sound run are **byte-identical up
to and including the character select** and diverge exactly at the return to the menu (4140). The guest is
not blocked (main thread `ready`, semaphores free, IOP at ~1 M instructions/s, no active RPC server), it
spins and redraws the same frame — the full evidence, the thread table and the RAM comparison with the
console's match dump are in journal 010's follow-up. Working hypothesis, not proven: the stop coincides
with the route's first BGM stop/start, and the handshake with the game's own sound driver is what does not
complete.

Because of this stop and of the one crash in three runs of the menu route above, **the defaults were not
changed** (Part 4 of the task): `--sound` still off and `DON_VU1` unset still the runtime's interpreter
until the stall and the crash are understood.

### The stop moves with the IOP/EE batch size (measured 2026-10-13, T-304)

The orchestrator's hint, tested on this same route with the same configuration
(`work/orchestrator/t304/batch-experiment.sh`; `DON_IOP_BATCH_EE_CYCLES` forces the scheduler's batch,
8 = the pre-0033 feed of one dispatch, default 2048). All four runs: exit 0 by the script's `quit`, 41
shots from frame 6000 to 8400 with **one and the same picture** (`m_*`) - none of them reaches the match.
What moves is how far the *menu* sequence gets before it stops:

| batch | wall | distinct pictures among main_menu(3060), charselect(3600), menu_again(4140), charselect_practice(4980), stage_select(5880) | where it stops |
|---|---|---|---|
| 8 | 626.4 s | **1** | at the main menu (3060); nothing after changes |
| 64 | 150.7 s | **1** | at the main menu (3060) |
| 512 | 359.5 s | **5** | **past all five**: the menu, the character select, the menu again, the practice select and the stage list all render; it stops in the match stretch |
| default (2048) | 210.6 s | 3 | after the character select (menu_again already frozen, as in journal 010's follow-up) |

So the point where the game stops is **selected by how the IOP's work is interleaved with the EE's**, and
not by one fixed screen: the console's timing must always resolve it in the way that goes on. This is the
measurement to redo on a build that has patches 0037 (the PEXEW lane swap, which fixes the missing 3D
world) and 0038 (timely EE timer interrupts) - on those the host is no longer late at these screens, which
is exactly the variable this table changes.


## Crash 2 (`--sound on` alone, ~1740 vertical blanks): not reproduced on the first night

Crash 2 was not seen again, and the window was crossed twice:

| run | vertical blanks reached | exit |
|---|---|---|
| `--sound on`, the route, no VU1, `--seconds 300` (wall) | **2580** (past 1740), `[don-iop] vsync 2580` | 0 |
| `DON_VU1=microvu --sound on`, the route of this journal | **4860** (the route's `quit`) | 0 |
| `DON_VU1=microvu --sound on`, no route, `--seconds 25` | 25 s | 0 |
| `--sound on`, no route, `--seconds 45` (wall) x4 | ~540 (the dialog is slow) | 0 |

No core file was produced (runs from `work/cores` with `ulimit -c unlimited`; the core pattern is
`core`, and none appeared). So its backtrace is still **not obtained** and journal 009's inference about
the replaced `sceMpegCreate` could not be re-checked against `MPEG.cpp` (that needs the stack).

One hypothesis that cannot be settled without the stack: crash 2 may have been crash 1 seen in the
video/menu phase. Journal 011's original T-304 command had `DON_VU1=microvu --sound on` and died at
~30 s with a 30.0 s WAV, and crash 1 is exactly "the first VU1 microcode upload after the main thread
reused the stack where the VU1 table lived": that call is at boot in some runs and at the video/menu
setup (the 1740-blank window of journal 009) in others. Against the hypothesis: journal 011 recorded a
`--sound on` crash **without** a VU1, which no VU1 table can explain. What is known stands: it was seen
with a sound run, never without sound, and it is timing-dependent.

## Follow-up (2026-10-13, journal 020): the stop with sound, on the build with 0037 and 0038 (before 0039)

With `--sound on` the guest is stopped at the title in all runs: the frame-loop counter (`DON_TRACE_FRAMES`)
stops at 2280, the picture from blank 2700 to the end is the same washed title, whatever follows in the script
(menu route x3, match route x3: identical pictures and WAVs in each set; `DON_IOP_BATCH_EE_CYCLES` = 8, 64,
512 and unset all stop at 2280). The stop no longer moves with the batch size and is deterministic. Without
sound the counter stops at 2880 and a Start tap after about blank 3120 does nothing (routes whose tap is at
2760 are not affected). The WAV has music from second 40 at a peak of ~0.02. Not measured on the build with
0039 (cut by the orchestrator).

## Follow-up (2026-10-13, after journals 020/023): sound on the build with 0039, videos skipped, no other variable

Build: `build/linux-dev/don` of 17:08 (0039 in, videos skipped, microVU default). Headless, software renderer,
`--sound on --wav ... DON_TRACE_SPU2=1 DON_HOST_LOG=1`, shots every 120 blanks. Scripts `work/integ/s0039_menu.txt`
and `s0039_match.txt` (the route of `to_match_1v1.txt` with every step after the title moved by -560 blanks).

1. **To the menu and 20 s there** (Start at 2200): exit 0, wall 67.0 s for 4010 blanks (60 per second: with sound
   the run is paced by the sound clock; `[don] GS: 4010 vblanks, 0.68 ms in the GS per vblank (max 5.88)`;
   fast path 99.98 % of 2.34e9 dispatches). The menu is up from ~2700 and stays alive (the shots from 3120 to the
   end are one picture: the static menu). Recording `work/audio/s0039-menu.wav`: 66.8 s, rms 0.0588, silent
   52 %, saturated 0; **sound from second 35** (the title, blank ~2100), peak 0.53 at second 36, then 0.27-0.35
   with rms 0.06-0.09 through the menu. `dmapeak0` (the samples the game DMAs to the chip) is 0 until 34 s,
   17883-18296 at 35-38 s and 8-11 thousand afterwards, of 32768. Before 0039 both the recording and the DMA
   data peaked at 0.02: the music is now ~15 to 25 times louder, at a plausible level.
2. **To a fight with sound**: exit 0, wall 120.8 s for 7240 blanks (60 per second). The route goes through
   the title and the main menu (blank 2500) to the single-player character select (picture up at ~2880) and
   **stops there**: from the shot at 2880 to the last (7200) the picture is the same character select
   (37 identical shots; Cross at 3100, the Down taps and every Circle after it changed nothing), so the stage
   list, "GET READY" and "FIGHT" were never reached. The vertical blanks, the IOP and the music go on to the end
   (recording `work/audio/s0039-match.wav`, 120.7 s; peak 0.27-0.37 per second to the end; `dmapeak0` ~11000
   to second 120). Last `[MC]` lines: the three usual `GetInfo/Sync` lines; no `[don-spu2]` error. Not
   investigated (not asked). Whether the same stop happens without sound was not tested.

## Follow-up (2026-10-13, T-304): the stop at the character select, measured; the cause is not found

Same binary as the previous follow-up (0039, videos skipped). Scratch: `work/integ/iopdump.py` (a gdb script
that prints the IOP kernel's maps from `IopKernel::beginNextReady` at chosen vertical blanks, counted at
`ScreenshotWriter::on_vsync`), `s0039_stop.log`, `s0039_bt.log`, `s0039_match_nosound.log`.

1. **Without sound** the route `s0039_match.txt` (Start at 2200) does not reach the character select: the
   picture is the title from the shot at 2040 to the end (43 identical shots, exit 0, 198 s) and Start does
   nothing. The same with Start at 2060 (`s0039_match_early.txt`, 42 identical shots, 154 s). So without sound
   the game stops earlier, on the title (the "title left alone stops answering" defect), and the two stops
   cannot be compared by this route: the stop at the character select was reached only with sound (whose
   run is paced at 60 blanks per second). Not tested: a route with sound that presses Start earlier, or a
   route without sound that reaches the character select in some other way.
2. **IOP kernel table** (gdb, `IopKernel` maps; layout of the thread struct found by dumping, ids and states
   are reliable, the priority and wait-id columns of the first script were not): at vertical blank 2500
   (healthy menu) and at 3300 (frozen) the tables are **identical**: 12 threads. t1 (entry 0x10ec0, pc
   0x103c4, module 1 at base 0x10000) waits on an event flag, t3 (entry 0x12680, pc 0x11a14, module 2) waits on
   an event flag, t8 (entry 0x78758, pc 0x6E184, cri_adxi at 0x6E000) is in a delay, t10 (entry 0x78734, pc
   0x72400, cri_adxi) waits on semaphore 6 with count 0, t9 (entry 0x785F0, the RPC server of service
   0x90000200) sleeps, the rest sleep. Semaphores 1 to 5 have count 1, 6 has 0; event flags 3, 4, 5 have bits
   set. So the IOP's state does not tell the frozen game from the healthy one.
3. **EE thread table** (`DON_TRACE_THREADS=1`): identical at vsync 2400, 2760, 3000 and 3240 (t1 ready at
   0x31587C, t3 and t4 waiting on semaphore 1, t2 and t5 suspended), as in the earlier no-sound runs.
4. **The chip** (`DON_TRACE_SPU2=1`, the run of the previous follow-up): from second 1 to the end the driver
   makes **no writes** (`writes=1142 (+0)` every second, including at 35 s, when the music starts), polls
   0x1B0 about 94 times per second (always 0001), and core 0's transfer runs steadily (94 DMA completions and
   about 192,000 bytes per second, `events4` +94, until 120 s). The attribute registers at the end:
   core 0 `0xFF80` DMA off, core 1 `0xFF81` DMA on. Nothing changes around second 48 (blank 2880).
5. **Not measured**: the remote calls (the log shows no replies; the bridge state in gdb was not read), the
   guest pc of the main thread at the stop (the game thread's host stack at vblank 3300 is the scheduler's own
   event loop, `EeScheduler::run` -> `processEvent` -> `notifyHostGsVsync`, so it does not show the guest
   wait; the profile of a run to blank 3700, `work/integ/prof.bin`, has 70 % of the samples in the scheduler
   and the dispatch of guest branches (`EeScheduler::run`, `hasFunction`, `lookupFunction`) and the guest
   functions 0x2d4790, 0x136560 and 0x2d47b4-0x2d47c8, a tight loop)).

**What the measurements carry**: nothing in the IOP's or the chip's state changes at the freeze, so there
is no sign that a thread waits for an IOP or chip event that never comes (measured: the same tables before
and after). The EE side stops the game's frame loop (journal 011's earlier finding) while the scheduler goes
on dispatching a tight guest loop (profile). That the freeze is not a sound defect is **not** shown either
(point 1). A sentence of the form "thread X waits on S" cannot be completed from these data. The next
measurement is the guest program counter and the semaphore or flag the main thread waits on at the stop,
read from the runtime's EE thread state with gdb, and the same at the title stop without sound.

## Follow-up (2026-10-13, T-304): sound with the game's own video player (`DON_PLAY_MOVIES=1`)

`DON_PLAY_MOVIES=1 DON_TRACE_SPU2=1 DON_HOST_LOG=1 build/linux-dev/don --headless --renderer software --sound on
--wav work/audio/pm-match.wav --shots work/shots/pm-match --shot-every 120`, script `tools/run/scripts/to_match_1v1.txt`
(committed timing). Exit 0, wall 130.2 s for 7800 blanks (60 per second: the sound paces it).

- It **stops at the title**: the video player is passed (shots at 2040 to 2400 show the fade after the video;
  the one at 2280 is black), the title appears washed, in its fade-in state (md5 b37047...), at about blank
  2520, and the 45 shots from there to 7800 are that same picture. The memory-card dialog shot is fine; `main_menu`,
  `charselect`, `charselect_practice`, `stage_select`, `match_load_*`, `match_intro` and `match_*` are all that
  title: Start did nothing. The menu, the practice select, the stage list, "GET READY" and "FIGHT" were never reached.
- This is the same picture as the old sound stop (before 0039, journal 011's first follow-up), now with
  patch 0039, so the stop with sound is **not** caused by the skip of the videos.
- Recording: 130.0 s, rms 0.0628, silent 31 %, saturated 0. Silent to second ~35, music from second ~35 to 42
  (peak 0.50 at second 42), then 0.32 to 0.36 peak, rms 0.07, steady until the end (130 s); the data the game
  sends to the chip (`dmapeak0`) is 0 to second 40 and ~10,500 to 11,100 from 48 on. That music runs on over the
  stopped title; no announcer or fight sound exists since the game never leaves it.
- `[don] GS: 7800 vblanks, 0.34 ms in the GS per vblank`.
- Not verified: whether the stop happens without sound on this path (the no-sound route with `DON_PLAY_MOVIES=1`
  was not run in this piece).

## Follow-up 2026-10-13: what the title waits for with sound (four memory images; partial)

Same binary, same route (`DON_PLAY_MOVIES=1`, Start tap at 2400, nothing after), A = `--sound on`, B = no sound. Dumps
(game memory, not versioned) `work/integ/title{A,B}{3000,3060}.bin` (`DON_TRACE_STATE=60` is needed so the state line, which
carries the dump, is printed at those blanks). Shots: A is the same washed title at 2940/3000/3060 (one md5); B animates.

Measured:
- The scene interpreter (via the thread ring at 0x535C00, index at 0x535BE8) is **not running at all in A**: the ring index is
  406 in both A dumps (identical), while B's cycles. Scene state word 0x535BF0 is 9 in A, 2 in B. The title thread (base
  0x19E7E00, wait counter 0x1600) and the frame counter of the object at 0x19E7DA0 (+0x1C: 0x30 in A, both dumps; 0x246 then
  0x282 in B, +60) do not move in A. So no script opcode is "waited on": the scene loop itself is not being entered.
- The per-frame loop `FUN_00131CA0` therefore sits inside a per-frame function. The candidate that matches: `FUN_002D46B8`
  (display list / GIF packet flip). It loops `do { cb(lVar3, n) } while (lVar3 != 0)` with `lVar3 = (w[0x4C9F08] != w[0x4C9F0C])
  || b[0x4C9F00] != 0`; `cb` = `w[0x4C98D8]` = 0x136560, a spin counter at 0x4D6900 (no sleep, no yield). Values:
  - A3000 and A3060: `4C9F08=69CD88`, `4C9F0C=69CD90` (differ, identical in both dumps), `4C9EE0=69CD80`, `4C9EE4=5AA140`,
    `4C9E9C=1020000`, `4C9E90=0`. The spin counter at 0x4D6900 is 0x5FFD61 then 0x16E320 (it wraps at 0x989681): it is spinning.
  - B3000 and B3060: `4C9F08=69CD80`, `4C9F0C=6B92D0`, `4C9E90=70002480`, `4C9EE4=5AA460`; counter small (0x13B3D, 0x13B39):
    the loop is not spinning, it was caught while the queue moves.
- The loop ends only when the queue pump `FUN_002D3A60` (called with 1 or 2 from an interrupt handler, -1 from here; its
  state is gp-relative) consumes the pending entry and updates 0x4C9F08/0x4C9F0C. In A that never happens.

So the condition is: the main thread waits in the GS packet queue for a completion that arrives through an interrupt-driven
pump, and with sound that completion does not come. It is **not** a sound-state word and not 0x150AB0 (not tested).

Not verified / supposed: which interrupt calls `FUN_002D3A60` with 1 or 2 (a GIF/GS DMA end or a GS FINISH/SIGNAL are the
obvious guesses), and why sound changes it. A plausible reading is that with `--sound on` the EE scheduler is paced
differently (60 blanks per second exactly) and an event of the DMA/interrupt path is lost or masked while the thread spins
with interrupts in a window (`DI`/`EI` pairs around the loop are in the function). The earlier IOP-side observations (no thread
blocked) agree with a host-side lost event, not a game-side wait.

Next piece (short, gdb on the game thread, no code change): at blank ~3000 with sound, read `INTC_STAT/INTC_MASK`, `DMAC`
channel 2 `CHCR`/`D_STAT`/`D_PCR`, and the EE `Status` register (EI bit, IM bits) and the callbacks registered by
`AddIntcHandler`/`AddDmacHandler`; compare with the same read in B. A scan for the writers of 0x4C9F08 found none by absolute
address (the code reaches it through gp).

## Follow-up 2026-10-13 (later): the DMA delivery path, and a correction of the previous follow-up

**Correction.** The previous follow-up said the game's display-list queue is stuck (`w[0x4C9F08] != w[0x4C9F0C]` in
both A dumps). That was an artefact: the two A dumps were taken 60 blanks apart, and the queue alternates between two buffers
every blank, so the head (0x5A9F08 or 0x69CD88) is the same at every even distance. Measured under gdb in the stopped state
(`work/integ/hits2.py`, `stuck.py`, blanks 2600 to 3020, `--sound on`, `DON_PLAY_MOVIES=1`): every blank the VBlank handler
`FUN_002D3FD0` runs (a0=2) and clears the busy byte 0x4C9E9F, the queue pump `FUN_002D3A60` runs three times (a0=-1, then a0=1
= the VIF1 DMA end-of-transfer interrupt, then a0=-1 from the main thread) and the head moves; `FUN_002D46B8` is entered once per
blank and returns. So the **DMA end-of-transfer interrupt is being delivered in the stopped state**, and the statement "the
queue pump is waiting for a lost interrupt" is **not supported**. What stays stuck in the same run: the scene thread ring index
(0x535BE8 = 406 at 3000, 3010, 3020) and `FUN_00138B00` is not entered (0 hits in 30 blanks). A run with breakpoints on the
queue functions from the start looked alive in the scene as well in one sample; the stop is timing-dependent (one gdb run
stopped, another did not), as before.

**Delivery path (read, third_party/PS2Recomp/ps2xRuntime/src/lib):**
1. `ps2_memory.cpp:2248-2257` (`completeDmacChannel`), `:1935-1967` (`processPendingTransfers`, GIF/VIF0/VIF1) set the channel's
   pending bit in D_STAT (and bit 31 when status & mask) and call `queueCompletedDmacCause` (`:1973`, mutex-protected vector).
   The D_STAT mask bits (16 and up) are not consulted for delivery.
2. `ps2_runtime.cpp:1547` `drainCompletedDmacHandlers` takes the vector (`consumeCompletedDmacCauses`, `:1979`) and calls
   `dispatchDmacHandlersForCause` for each cause. It is called only after `Store32` (`:2212`) and the GIF store paths
   (`:2271-2281`); `Support.h:1380-1434` (`sceDmaSend`) has its own copy. A cause queued by another kind of store waits in the
   vector until the next `Store32`; it is kept, not lost.
3. `EeScheduler.cpp:1379` `dispatchIrq`: **drops the cause (no pending state kept) when its bit is clear in `m_enabledDmacMask` /
   `m_enabledIntcMask`** (`EnableDmac`/`DisableDmac`, `EnableIntc`/`DisableIntc`, `setIrqCauseEnabled` `:1361`), and when no
   enabled handler is registered for it. Candidate 1: a transfer that ends while its cause is disabled is lost for good; the
   console keeps it pending and delivers when it is enabled. In the stopped state both masks are 0xFFFFFFFF, so this did not
   happen here.
4. `EeScheduler.cpp:1226` `queueInvocation` appends to `m_pendingInvocations`; `run()` (`:278-290`) starts one only when
   `running->invocations.empty()`, so a cause that arrives while a handler runs waits (kept). It is started at the next point
   where a guest function returns to the scheduler (`checkpointDue`, `:378`).
5. **Candidate 2, not proven:** nothing in this path reads the guest's interrupt-enable bit. The recompiled `DI` is only
   `ctx->cop0_status &= ~0x10000` (generated `FUN_002d46b8`, 0x2D47D0), and the scheduler never tests it, so a queued handler can
   start inside the game's own critical sections (the game calls the pump with a0=-1 between `DI` and `EI`). Measured: 0
   re-entrant pump calls in 1500 blanks under gdb (`work/integ/reent.py`), so no evidence it happens.

**Registers in the stopped state (gdb, blank 3000/3030; the runtime's I/O map found at PS2Memory+0x40):** D_STAT = 0x306
(pending bits 1 and 2 and bits 8, 9; no mask bit), CH1 CHCR = 0x70000045 (STR clear: idle), `m_completedDmacCauses` empty,
enabled masks 0xFFFFFFFF, DMAC handlers: id 3 (cause 1) and id 4 (cause 2), both `0x2D3A60`, enabled; INTC handlers: cause 2
`0x2D3FD0`, cause 3 `0x138EE0`, cause 9 `0x136BE0`, cause 11 `0x117210`, all enabled. The handlers were re-registered once
(ids 1/2 with argument 0xE14C50 at blank 300, ids 3/4 with 0x69CE10 at 3000). INTC STAT/MASK and the EE Status register were not
read. This matches none of the candidates: nothing is waiting in the cause list and nothing is masked.

**Defect: not found. No patch 0041.** The earlier thread-table evidence and this one agree that the game's main thread is alive
and the interrupts are served; what is unknown is why the scene loop (ring 406, `FUN_00138B00` not entered) does not advance.
Next measurement (gdb, no code): break on the guest function that calls `FUN_00131CA0`'s per-frame body from the stopped state and
read where the main thread is (guest pc and call chain of the main EE thread at the stop), which the earlier pieces never got.

- 2026-10-13: the picture of how the port executes the game's code and of the stop, put together from this and the neighbouring entries, is in [knowledge/20-execution-model-and-the-stop.md](../knowledge/20-execution-model-and-the-stop.md).

## Follow-up 2026-10-14 (T-304): "voices but no music or effects" in a window: what is in the mix and where it is lost

Owner's report: practice matches in a window, no music, no effects, voices yes. Runs headless, software renderer, `--sound on --wav`,
`DON_TRACE_SPU2=1 DON_AUDIO_LOG=1`, English with Japanese voices (`~/.local/share/don/variants/en-jpvoice`, the launcher's folder), default
video skip, route: dialog answered at 330, title up ~2300, Start at 3500, menu 3800-4400, Down, Down, Circle (Practice), character/Com/stage
confirms (5160, 5460, 6000), fight inputs 6720-10320 (Right 25, Square x3, Cross, Square every 130 blanks), quit 10410. Scratch and scripts in
`work/sound/` (`route_in.txt`, `route_no.txt` = same without fight inputs, `an.py`, `burst.py`). The wav's second = blank / 60.
Binaries: runs r1, r2, o1, o2 `72a4f6c0b70f` (the owner's); r4 `6c03c4c49c11` and the gdb runs `6c03c4c4...`/`7d0ccd44...` (rebuilt by someone
else in between). The owner's memory-card folder `~/.local/share/don/memcards/mc0`, `mc1` **is empty** (two empty directories made at 23:33), so
"a copy of their card" and "an empty card" are the same thing; both were used (`work/sound/mc-copy`, `mc-empty`), same result.

**What is in the mix (r1, owner's configuration; levels are rms of the mono mix over half seconds):**

| Phase | Blanks | Wav s | Level | Content (how decided) |
|---|---|---|---|---|
| boot to title fade | 0-2300 | 0-38 | 0.000 | silent |
| title | 2300-3500 | 38-58 | 0.065, never below 0.048 in 34 half-seconds, std/mean 0.29 | **continuous**, 60% of the energy below 300 Hz, 15% in 2-6 kHz: a sustained bed, not bursts. This is the title music (music-like by shape; not listened to) |
| main menu | 3800-4400 | 63-73 | **0.0000, exactly silent** (20/20 half-seconds) | nothing: no music in the menu |
| Practice select, character, stage | 4400-6000 | 73-100 | 0.0044; 47 of 50 half-seconds silent | one burst 73.75-76.5 s (blank 4425-4590) peak 0.126, another at 6180; short clips, mid-band: voices |
| loading | 6000-6660 | 100-111 | 0.038 | bursts 103-105.75 s (6180-6345) and 109-111.5 s (6540-6690): voice clips (character name, "ready/fight") |
| fight with inputs (r1) | 6700-10400 | 112-173 | 0.0036; 116 of 122 half-seconds silent | **two** bursts in 60 s: 150.25-151.75 s (blank 9015) and 158-158.75 s (9480), peak 0.13 |
| fight without inputs (r2) | same | | 0.0013; 120 of 122 silent | **one** burst: 142.75-143.5 s (blank 8565) |
| Japanese disc, same route (r4, empty card) | same | | title 0.064, menu 0.000, fight 0.0042 | same phases; one burst at 9675 (peak 0.19); the select/load bursts identical to r1 (same blanks and peaks) |

Reading: at the title there is music; in the menu, the character/stage select and the fight there is **no continuous sound at all**, only
isolated clips of 0.75-2.75 s. The clips in the fight do not fall at the blanks of the hits: my Square taps were at 6750, 6762, 6774 ... every
130 blanks and the bursts are at 9015, 9480 (r1), 8565 (r2); the half-seconds around the hits are as silent as in the run without inputs
(0.0013 against 0.0036 mean, both dominated by the 1-2 bursts). **No effect at any hit** was found; the isolated clips are voices (their
timing does not follow my inputs; in r2 the opponent attacks alone). Which character says what was not checked.

**What the chip is asked (r1, r2, r4 identical):** `DON_TRACE_SPU2` says 1142 register writes **all in the first second** (240+240 voice
registers, 12+12 key-on/off, core control, reverb) and **none from second 1 to the end, 173 s** (`writes=1142 (+0)` in every line). So
in the whole route, including the fight: **0 key-ons, 0 volume writes, 0 voice programmed after the initialisation.** The only thing the chip does is
the AutoDMA stream into core 0: 94 transfers/s of 2048 bytes = 192 KiB/s = 48 kHz stereo PCM, steady through every phase (also in the
menu where the wav is silent: the stream goes on carrying zeros). Core 1 never gets a transfer. So **everything audible, the title music and
the voice clips, arrives as PCM on that stream; effects and any voice-bank sound that would be keyed on the chip never arrive.**

**Which module does what (docs/knowledge/05, 14, 16):** `cri_adxi` plays the ADX tracks of `BGM.AFS` (music and voices; it writes PCM to the stream);
effects are `SE.AFS` banks (`snd/se/ps2/system.dat`, `battle.dat`) played through the SNDF RPC of `sndfi` (SID 0x534E4446, EE-side wrapper
`0x377E58`) and/or the Sony synthesizer (`modhsyn`, `modmidi`, `modmsin`) which would key voices on the chip through `libsd`. All of these
modules are loaded and **all their imports have handlers** (docs/knowledge/14: 0 unsupported in the sound modules), so none is "a module the port
does not run" by that table. `DON_TRACE_IOP=1` lists only three RPC services (MCSERV, dbcman, libsd, all `active=0` at every sample): no line for
`sndfi` or `cri_adxi`, so the listing does not tell whether SNDF requests arrive.

**The options screen (item 3, `work/sound/o1`, `o2`):** Options is reached with Up from the first menu item and Circle. With the copy of the owner's
card (o1) and with an empty card (o2): AUDIO MODE **STEREO**, BGM **3**, SE **3** (the selected value is the white one; checked by moving
BGM: four Lefts make OFF white, one Right then makes 1 white). **Not at zero, not OFF.** So the game's own settings are not the cause; the
owner's card holds no settings at all.

**Not measured (time box reached):** whether the game ASKS for effects (a count of calls to the SNDF wrapper `0x377E58` and of the music
changes at the menu). I started that count under gdb twice; both runs counted 0 hits **but the breakpoint was written with upper-case hex
(`FUN_00377E58_0x377E58`) and the symbol is `FUN_00377e58_0x377e58`**, so it never resolved: the 0 means nothing. Redo with the lower-case name.

**Paragraph.** Both music (everywhere except the title) and effects (everywhere) are missing; the title music and the voices play. After the
chip's initialisation the game and the IOP drivers never program a voice (0 key-ons in 173 s, fight included), so nothing is ever keyed for
an effect and the only sound is the 48 kHz stream that `cri_adxi`'s ADX decoding fills: it carries the title track and the voice clips and
carries zeros in the menu, the select and the fight. Measured: the stream is steady, the volumes are not zero (BGM 3, SE 3), the card is
empty, Japanese and English discs behave the same. **Supposed (not measured):** the first place where it goes wrong is on the IOP side before
the chip: either the game never sends the SNDF/music requests after the title (the EE side), or `sndfi`/`cri_adxi` receive them and do not
play (their file reads of `SE.AFS`/`BGM.AFS` for the SE banks and the menu/fight tracks fail or wait). The next piece is the count of
SNDF RPC calls (`0x377E58`) and of `cri_adxi` start requests per phase, plus the IOP's reads of `SE.AFS`/`BGM.AFS` per phase.

## Follow-up 2026-10-14 (T-304, second piece): on which side of the wire are the effects and the music lost

Same route and variant as the previous follow-up (`work/sound/route_in.txt`, en-jpvoice, empty card, blanks: boot to 2300, title 2300-3500,
menu 3800-4400, select 4400-6000, load 6000-6700, fight 6700-10400), run under gdb with breakpoints that all resolved (`info breakpoints` printed
their addresses before the run): `FUN_00377e58_0x377e58`, `ps2_stubs::sceSifBindRpc`, `ps2_syscalls::SifBindRpc`, `ps2_syscalls::SifCallRpc`,
`ps2_syscalls::sceSifCallRpc` (one calls the other: each call appears twice in the raw log, 98 lines = 49 calls),
`ps2x::iop::detail::IopImportRegistry::resolve` (every call of a module into another module's export, e.g. into `libsd`),
`IopCdvd::Impl::readSectors`, `ps2_stubs::sceCdRead`/`sceCdStRead`. Scripts: `work/sound/ee_iop.py`, `ee_q.py`, `init.py` (not versioned; their
output is `work/sound/ee_iop.out`, `ee_q.out`). The binary changed between runs (md5 `780d0d39`, `7d0ccd44`); nothing of the results differs.

**1. What the main processor sends (measured).**
- The per-frame sound function `0x377E58` is entered every frame in every phase (100 entries per 100 blanks from blank 130 to the end).
  **It does nothing**: its first test is the word `[0x4B2DE8] == 1`, and that word is **0 at every one of those entries** (in 70-100 entries per
  100 blanks, all 74 bins from blank 100 to 7600: `flag1=0`). Behind that test are everything it would send: the copy of the effect
  request ring (`[[0x4B2DF8]+0xAA4]` requests -> `0x59F640`) and `sceSifCallRpc(0x5A01D8 = SFSV client, 0x8000, async, ...)`. No call with SID
  `0x53465356` (SFSV) was ever made, and `sceSifCheckStatRpc` was never called by it.
- Remote calls that left the main processor in the whole run: **49, all before blank 2273.** By server: `0x80000701` (sdrdrv's SID) 6 calls
  (0x8000 once, 0x8010 four times; boot, blank 11-13); `0x90000200` (cri_adxi) 41 calls (blank 13: `0x400`x16, `0x422`x16, `0x2`x2, `0x403`, `0x408`x3,
  `0x409`, `0x40A`, `0x40C`; later one `0x408` at blank 2031 and one `0x409` at blank 2272, the title's music request); `0x534E4446`
  (SNDF, sndfi) 3 calls at blank 21 (`0x100`, `0x300`, `0x1300`; `0x100` is the init that `FUN_00376038` waits on). **After blank 2272 nothing leaves
  the main processor in any phase: title (2300-3500), menu, select, load, fight: 0 calls.** So in the menu the game asks for no music, and in the fight neither a
  hit nor anything else sends an effect request. The one music request of the whole run is the title's (blank 2272, `cri_adxi` `0x409`).
- What the main processor does read from the disc (`sceCdRead`, the game's LSN is the argument minus 0x100000; LSN ranges from the ISO directory:
  DATA.AFS 3135-29119, BGM.AFS 29120-77254, SE.AFS 77255-80596): **SE.AFS: one read of one sector, at boot (its header); nothing afterwards.** BGM.AFS:
  3 small reads at the title (blanks 2315-2342, 54 sectors), 3 in the menu/select (4413, 4415, 4513), 6 at loading, 4 in the fight (9008, 9471, 9472, 9473; 10-28
  sectors each). Those small BGM reads fall at the moments of the voice clips measured in the wav (4425, 6180, 6540, 9015, 9480): **the voice clips are read by the
  main processor from BGM.AFS at the moment they are heard** (no RPC to cri_adxi goes with them: how the clip reaches the stream is not measured).
- The flag `[0x4B2DE8]` is written in one place in the executable (scan of all `sw ...,0x2de8`): `0x376104` in `FUN_00376038`, the sound-effect driver's
  init: `if (DAT_004b2de4 != 0) { [0x4B2DF8] = 0x5A14C0; reply = FUN_00376f88(0x100 ...) (the SNDF RPC 0x100 whose status word is read at 0x59F540); ...; if (status == 0) [0x4B2DE8] = 1; }`.
  Measured at blank 150: `[0x4B2DE4]`=1, `[0x4B2DF8]`=0x5A14C0 (so the init started and issued the RPC), the status word at 0x59F540 = 0 **at that moment**, and
  `[0x4B2DE8]`=0. **Not determined:** what status the init read when it ran (the buffer may have been written afterwards; the store at 0x376104 was not
  caught), i.e. whether the init finished with a non-zero status or never reached its end.

**2. The sound processor's side.**
- `active=0` of `libsd` in `DON_TRACE_IOP` means the port's high-level `libsd` service is switched off on purpose when a host SPU2 is installed
  (`iop_subsystem.cpp`, `serviceActive`: `if (iopHostSpu2() && service.name()=="libsd") return false`), the real `libsd` module runs instead. It is not the reason
  nothing is keyed.
- Calls into the `libsd` module's exports (the import registry, counted per phase; ordinals as in the module's table, **names not checked**): at boot ordinals 4 (2), 5 (10),
  9 (4), 11 (14), 18 (1), 19 (4), 23 (4), 26 (1), 28 (1), 20 (3573); **after boot only ordinal 20**: title 1875, transition 469, menu 937, select 2501, load 1093, fight 5782 calls (about
  45-95 per blank, the stream-position poll that matches the 94 register reads per second). No other `libsd` export is called after boot: nothing programs, keys or
  uploads. This agrees with the chip trace (no register write after the first second). So the IOP-side modules are alive (the real `libsd` runs, its threads poll) but are
  **never asked** to key a voice: the requests that would make `sndfi`/`modhsyn` call `libsd` never arrive.
- Not measured: whether `sndfi`'s RPC server answers (the status word read above suggests the `0x100` call was answered); `modhsyn`/`modmidi`/`modmsin` were
  never addressed by the main processor (no call to their servers in 49 calls).

**3. The sample data.** The chip's DMA writes are 16,199 transfers of exactly 2048 bytes (33,175,552 bytes = the stream) in 173 s; **no other transfer to the chip's
memory** (no bank upload, core 1 none), and no data-port write after the first second. The effect banks (`SE.AFS`) are never read after the header (above). The
IOP's own disc reads were not seen: the breakpoint on `IopCdvd::Impl::readSectors` never fired (that function may not be the path of the IOP's reads; not investigated).

**4. The first place the chain breaks.**
- **Effects:** measured: the game never sends an effect request after boot because its per-frame sound function returns at once on the flag `[0x4B2DE8]` which is never 1,
  and no bank is ever read or uploaded. Supposed (from the code of `FUN_00376038`): the flag stays 0 because the init `FUN_00376038` does not complete with status 0 (the SNDF `0x100` call
  at blank 21 returned a non-zero status or the init did not finish), i.e. a defect in the answer of the `sndfi` module or of the port's RPC transport to SNDF. Probable fix place: the SNDF
  (`0x534E4446`) RPC path: read the status that the init receives (the store at 0x376104, the reply buffer 0x59F540 at the return of `FUN_00376f88`), then see why `sndfi`'s reply is
  not 0 (module not running that server, a call it makes that the port answers with a default, or a file it cannot find).
- **Music:** measured: one request to `cri_adxi` for the title (blank 2272, `0x409`) and it plays; in the menu and after, no request and no BGM.AFS read for music, so the menu/fight
  music is never asked for. Supposed: the menu/fight music (a sequenced or ADX bed started by the same SNDF/BGM machinery that the dead flag disables) shares the cause with the
  effects; the voice clips take another path (read from BGM.AFS at the time, no RPC). This is not proved; the title-vs-menu comparison with the console (`work/ref`) was not made.
- Not verified: the reply status itself, `sndfi`'s thread state, and the reference console's key-on history.

## Follow-up 2026-10-14 (T-304, third piece): why the effects driver never starts, and patch 0057

**Defect (measured): the IOP's heap was full.** `sndfi` answers the SNDF call `0x100` (the game's `FUN_00376038`, which stores 1 in
`[0x4B2DE8]` only if the reply word is 0) from its handler `0xD1F4` (module offsets): it takes the interrupt state, calls `AllocSysMemory(0, [state+0x4C0], 0)`
(`sysmem` import 4) and returns **-2** when that returns 0. Under gdb (`work/sound/rpc100.py`, `alloc.py`; breakpoints on `sceSifCallRpc`, `FUN_00376038_0x376038`,
`IopMemory::allocate`, all resolved) the reply of `0x100` is `0xFFFFFFFE`, `FUN_00376038` returns -2, the flag stays 0 (the status word read 0 at blank 150 only because
the later call `0x300` reuses the buffer `0x59F540`), and the 0x1600 call that would follow was never made. Every `IopMemory::allocate` of the first 21 blanks:
31 blocks from `0x120000` to `0x1EC600`, then `sndfi`'s three requests, `0x4280`, `0x8000`, `0x8000` -> **0, 0, 0**. The heap is `HeapBase 0x120000 .. HeapLimit 0x1F0000`
(`ps2xIOP/src/emulator/core/iop_memory.h:25-26`; `IopMemory::allocate`, `iop_memory.cpp`, a bump allocator that refuses above the limit); the modules load in an arena below
the heap that ends at `0x9BD00` (knowledge 14), so `0x9BD00..0x120000` (540 KB) were never used.

**Steps of the start-up (game side):** `FUN_00376038`: if `[0x4B2DE4]`: `[0x4B2DF8]=0x5A14C0`; `FUN_00376f88(0x100,..)` = a blocking (mode 0) call on the client `0x5A01B0` (SNDF),
reply words at `0x59F540`; if the status is 0: call `0x1600`, set `[0x4B2DE8]=1`. The calls `0x1300` (before) and `0x300` (after) belong to the callers. The per-frame
function `0x377E58` then sends the async `0x8000` on the SFSV client `0x5A01D8` (mode 1) every frame. It was stuck at the first step, at blank 21, for the whole run.

**Patch 0057** (`patches/ps2recomp/0057-iop-heap-starts-above-the-modules.patch`, not added to `series` as asked): `HeapBase` 0x120000 -> 0xB0000 (margin of 0x14300 above the modules' end).
Test `tests/runtime/iop_heap_capacity_test.cpp` (replays the 31 measured blocks and sndfi's three): fails without the patch (3 blocks refused), passes with it. The patch applies
on a clean tree with the series (checked in `work/wt/t304-heap`) and is applied in the shared tree; build `DON_JOBS=4 bash tools/pipeline/build.sh linux-dev`:
`[build] linux-dev done in 0h 00m 51s`, binary md5 `46c8bc85...` (the owner's before: `7d0ccd44...`).

**After the patch** (same route, en-jpvoice, empty card, `--sound on`, 173 s; gdb check of blank 21: `0x100` reply 0, `FUN_00376038` returns 0, `[0x4B2DE8]=1`,
`0x1600` called, then `0x8000` on SFSV every frame (36 in blanks 22-57), `0xA00`/`0xA01` on SNDF):

| phase (wav seconds) | before: silent half-seconds / chip writes | after |
|---|---|---|
| title 41-58 | 0/34 (the PCM music) | 0/34 (same) |
| main menu 63-73 | 18 of 20 silent (rms 0.004 after); 0 writes | **unchanged**: 18/20 silent, 0 chip writes in 64-72 s |
| select 75-100 | 47/50 silent | 30/50 silent, 414 writes |
| load 100-111 | clips | 7/22 silent, 366 writes |
| fight 112-173 | 1-2 clips in 60 s, rms ~0 | **0/122 silent, rms 0.136**, 54,684 chip writes (about 880 per second), 32 voice starts logged, 1.9 MB uploaded to core 1 (65 DMA, the banks) |

Totals of the chip's trace: before 1142 writes, 0 key-ons, no upload; after 59,441 writes (49,321 voice writes), `dma1` 65 transfers / 1,895,040 B. The wav was analysed with
`work/sound/an.py`; **not listened to**: what is measured is level and activity. Whether the fight's sound is right (effects at the hits, music) is for the owner's ear.

**Still silent (measured) and not explained:** the main menu: no chip write, no music. Not determined whether the game asks for menu music at all (the new RPC count by phase
was cut by my time limit at blank 556 and is not reported); the next piece is the same SFSV/SNDF counts by phase on the new binary, and the `0xA00` request's argument at the menu.
Not verified: that the margin (0x14300 above the modules) holds for every disc variant; sample-accurate comparison with the console.

**2026-10-14 (T-304):** sound is now on by default (`--sound off` keeps the old silent run). The freeze that made it opt-in is fixed (patches 0054, 0055); the fights have effects and music (0057). A missing SPU2 library or audio device without an explicit `--sound on` is one `[don]` line and the run goes on silent. The runner of `tests/host`, `tools/run/smoke.sh` and `tools/run/gdb_trace.sh` pass `--sound off`.

## Follow-up 2026-10-14 (T-304, fourth piece): the IOP heap in a long session

Question: the allocator only grows (`m_heapCursor` never goes down, `freeAllocation` leaves it, `maxFreeMemory` = limit - cursor): does a long session walk it to `HeapLimit 0x1F0000`?
Run: the route of the main mode (`work/soak/r6.txt`, binary md5 `46c8bc85...`, with patch 0057), under gdb with breakpoints on `IopMemory::allocate` (with its return value),
`IopMemory::freeAllocation`, and `ScreenshotWriter::on_vsync` (the blank counter), all resolved; `--sound on --wav`, `DON_TRACE_SPU2=1`, empty card, 640 s of wall time under gdb,
which reached blank about 30,600 (510.7 s of audio): match 1, the interlude and match 2, **not the replay** (not reached). Scripts: `work/sound/heap.py`, `heapbt.py`, `freecnt.py`; log `work/sound/heap.out`.

**Measured:** 45 allocations and 1 free in the whole run. All the allocations come before blank 700 or are `0x8000` blocks at blanks 21, 29, 2301, 2587, 3157, 3175, 3193, 3537, 3573, 3620,
3655, 3697; **from blank 3697 to 30,600 there is no allocation and no free**: the highest address in use stays `0x1E18C0` (heap limit `0x1F0000`: **0xE740 = 59 KB left**) through
match 1 and match 2. The ten `0x8000` blocks are the game's own `sceSifAllocIopHeap(0x8000)` (backtrace: `FUN_00378eb0` called from `FUN_0037A2A0`, the sound-bank loader of knowledge 16
section on `system.dat`/`battle.dat`); the game never calls `sceSifFreeIopHeap` or `sceSifFreeSysMemory` in blanks 0-3720 (counted, breakpoints resolved).
**So the heap does not grow per match**: the second match, on another stage, asked for nothing. Not verified: whether other game modes or stages not in this route do allocate (each new
bank-loading step above did), and whether the replay allocates. Margin is thin: one more `0x8000` fits, two do not. **No patch 0058 written** (the allocator's weakness is real but
this route does not exercise it; a first-fit allocator with a "largest gap" query would remove the thin margin if another mode needs it).

**Sound per phase (r6, 20-s windows, mean rms / silent half-seconds):** boot 0-40 s silent; title 40-80 s 0.083-0.089 / 7 then 2 silent; match 1 (80-270 s) 0.138-0.176 / 0 silent, chip writes
about 17,000-19,700 per 20 s; interlude 280-345 s rms 0.042-0.043 / 0 silent, only 230-380 chip writes per 20 s (a continuous bed, almost no effects); match 2 (360-510 s) rms 0.127-0.154 / 0 silent,
18,400-19,400 writes per 20 s. So the second match has the same sound as the first by level and chip activity. Not listened to.

Item 3 (does the game ask for menu music): not reached in the time box.

## Follow-up 2026-10-14 (T-304, fifth piece): patch 0058, a refused heap request is reported and freed space is reused

`patches/ps2recomp/0058-iop-heap-reuse-and-refusal-report.patch` (`iop_memory.cpp/.h`): `allocate` places a request in the first gap that fits from `HeapBase`;
`maxFreeMemory` answers the largest gap; new `totalFreeMemory`; a refused request prints `[ps2xIOP] heap request refused (#N of this size): size= alignment= fixed= largest free gap= total free=`
on stderr (first four per size, then every hundredth). Who asked is not printed (a gdb backtrace on `IopMemory::allocate` gives it). Base unchanged; fixed-address requests behave as before.
Test `tests/runtime/iop_heap_reuse_test.cpp`: 12 checks fail on the tree without the patch, pass with it; `iop_heap_capacity_test` passes on both. Build (4 jobs): `linux-dev done in 0h 00m 54s`, md5 `f208a16e...`.

Route to the fight (`route_in.txt`, empty card, `--sound on`): same picture as after 0057 (fight rms 0.136, 0/122 silent half-seconds, 59,441 chip writes, 49,321 voice writes), **0 refusal lines** in the log.

Main menu (`route_menu.txt`: title, START, wait ~40 s, enter practice, back with Triangle, wait ~40 s; 165 s of audio; rms per 5 s): title 40-62 s 0.09-0.04; **menu 65-95 s: 0.000 in every window**;
character select 100-110 s 0.057-0.074; back in the menu 120-165 s: **0.000 in every window**. So the main menu is silent always on the port, not only for its first seconds, and also after coming back.
Not found: whether the game asks for menu music (requests to the two sound servers during the menu); that is the next piece. Not listened to.

## Follow-up 2026-10-14 (T-304, sixth piece): the silent main menu, and no music after the title

No code changes, no builds. Binary md5 `f208a16e...` (0058) for both runs. Scratch: `work/sound/off/` (savestate readers), `menu_req.py`, `full_req.py`, `full_req.out`.

**Console evidence (existing dumps, `work/ref/x/01..06`, no emulator run).** `SPU2.bin` is the raw `DataBlock` of `spu2freeze.cpp`: id, 64 KB register image, 2 MB sample memory, version, then `Cores[2]` at byte 2162696
(`V_Core` 8128 bytes, `Voices` at +488, `V_Voice` 264 bytes: ADSR at +24 with `Value` +48 and `Phase` +52, pitch +80, SSA +88; offsets from `offsetof` on the pinned sources).
State 02 is the main menu (screenshot checked). Voices keyed on (phase attack, envelope at max, volume non-zero): title 2 per core, **menu 7 on core 0 and 6 on core 1**, character select 10 and 9, matches 24 and 24.
Core 0 has `AutoDMACtrl` = 1 and input volume 32767 in every state: the stream (ADMA) input is on in the menu, and its buffers in SPU RAM carry a signal (rms about 1,900 title, 2,700 menu, 2,100 select).
In the menu, voices 0 and 1 (SSA 0x10008, over silent data) are the same pair as in the title. **More important:** the EE's stream ring buffer (seven 16 KB slots at EE 0x515540..0x52D540) holds sequential music-archive sectors in every dump;
matched against the disc image by their first sector: title ISO sectors 29418-29463, menu 30279-30324, select 31914-31959, 1v1 match 38787-38832. The console keeps reading the music archive forward through the menu, the select and the fight.
So on the console the menu (and the fight) have music from the stream, like the title. Not listened to; this is state, not audio.

**Port, blanks 0-7000 (route `route_in.txt`, gdb breakpoints on the SIF RPC calls and `sceCdRead`).**
- The game does ask: SNDF (sid `534e4446`) `0xA03` at blank 2321 (title), `0x703` at 3633 (stop at the title-to-menu change), **`0xA03` at 3707 (menu)**, `0xA04/A05/A06` at 4705-4743 (select), `0x703-706` then `0xA03/A04` at 6010-6096 and `0xA02` at 6136 (load/fight).
  Between them the main processor sends only the per-frame `0x8000` poll to SFSV (about 200 per 100 blanks; the fourth word of its payload is a constant per phase). No call to the `0x90000200` server (`0x400-0x409`) after blank 2272.
- Reads of the music archive. Title: a start at 2298-2335 (a `0xAB`-sector read, then `0x19` and `0x1C` sectors into 0x515540/0x521D40), then **a nine-sector read at 2342 and an eight-sector read about every 18 blanks, to 3618** (the stream is refilled).
  Menu: `0xA03` at 3707 is followed by the same two start reads (`0x19` at 3708, `0x1C` at 3710: LSN 0x106A76, 0x106A8F) and **then nothing**: no refill for the next 700 blanks. The same shape at the select (4744/4746), and at the load/fight (6103/6104: `0x19`, `0x1C`; then none).
- So in the menu, the select and the fight the game starts a stream, the first two reads happen, and the refills that the title gets never come. The port's chip shows no stream activity there (0 writes in the menu, measured before).
- Not found: what blocks the third read. The title's start was preceded by a `0x409` call to the `0x90000200` server (blank 2272) and by `0x408` at 57 and 2031; the menu's start has none. Whether that call is the difference, or the third read waits for something from the IOP side, is not measured.

**Paragraph.** On the console the main menu has music, the same kind of stream as the title, by the savestates: the stream input is on, its buffers carry signal, and the stream's read position in EE memory advances through the music archive from title to menu to select to match.
On the port the game does ask for it (a start request at 3707 and the two start reads), and the stream then stalls after its first two reads: the refill reads that the title gets never happen. So the silence is a defect, not what the game does,
and it is the same defect in the fights (no music in the fight as the owner reports; the continuous level there is effects and voices).
Next piece: backtrace the EE caller of the nine-sector refill read at blank 2342 (title) and find what condition the menu's stream never meets (an IOP reply, an `0x90000200` call, a status word the SNDF `0xA03` start leaves unset).
Not verified: that the console's stream positions are "the music" (state, not audio); the mapping between console ISO sectors and the port's LSNs (about +2490 from the title); the replay and other modes.

## Follow-up 2026-10-14 (T-304, seventh piece): why the second music stream stalls (diagnosis, no patch)

Binary md5 `f208a16e...` (0058), route `route_in.txt`, gdb with breakpoints/watchpoints on the generated functions (host frames name the guest functions), scripts `work/sound/{bt_cd,wp,wp2,wp3,wp4,snap,snap2,calls,cnt,dtx}.py`.

**The refill's chain (title, where it works).** `sceCdRead` for the 8-sector refill is reached by `0x331610` (per-frame) > `0x3134d8` > `0x3132b8` > `0x3132e0` (a state machine that calls the library's servers in turn) > `0x3248b0` > `0x3244e8`;
the request that fills the read (lsn, count, destination: the object at EE `0x3DDBF0`) is written by `0x323f30` < `0x326840` < `0x322890` < **`0x31b1f0`** (the per-stream exec) < `0x31b618` < `0x31b820` < `0x31b7f8`. The same chain issues the start reads in the menu: the machinery is the same and runs every frame.
There are two stream objects (96 bytes apart): `0x3DC878` (a side stream) and **`0x3DC8D8` (the music)**. State word 0 is `0x01000201` while reading; the refill is issued when the ring has room (its fill word at +0x34 advances by 0x1000 per refill at the title).
Title: the object is created at blank 2333 by `0x31aec8` (state 2) from `0x31ce78` < `0x320740`, after the game itself started the music at blank 2272 through `0x363f60` (called from title-scene code `0x345f30>0x345758>0x35f5c0`; args: handle `0x141e0a0`, 4, 1; then fade `ff000000`)
> `0x350090` > `0x350168` > `0x31cab0` > `0x31cae0` > `0x32cea8`, which makes the only main-processor RPC to the `0x90000200` server (rpc `0x409`) of the title's start. After that the music's output joint is connected: the joint object at EE `0x5462A0` has its words +8 and +12 filled (`0x3DF0C8`, `0x3DFBE0`) and the consumer drains the ring.

**Menu (second stream).** At blank 3633 the game calls SNDF `0x703` (stop) and the joint's downstream words go to 0 (`0x5462A0`+8/+12 = 0, and the side stream's joint `0x5462A8` all zero). At 3707 SNDF `0xA03`, and at 3708 the same server opens the new file on the same music object: `0x31ce78` writes its descriptor
(`0x415`, lsn `0x331800`, 0x663 sectors; file1/lsn1 watchpoints, same writers as at the title) and the two start reads happen. But **`0x363f60`, `0x350090`, `0x350168`, `0x31cab0`, `0x31cae0` and `0x32cea8` are never called again** (breakpoints on all, blanks 2200-3760): no start of the ADX handle and no `0x409`.
So the object is reading with no downstream: state stays `0x01000201`, the last request length stays `0x1C` and the ring's fill word stays `0x1A800` from 3716 to 3800; no refill is due because nothing consumes. The select (4744) and the fight (6103) show the same two start reads and nothing more.

**Defect, as far as it is measured:** the second and later streams are opened (file descriptor, two reads) but the game never runs its music-start wrapper `0x363f60` (the ADXT start that connects the output joint and tells the transfer server) for them. Why not is not known: the SNDF `0xA03/0xA04` requests do arrive, the reply to the poll is not examined.
Candidates (not measured): the menu code calls `0x363f60` under a condition that is false on the port (a flag or reply from the sound processor, e.g. the SFSV poll's reply or the SNDF `0x703` answer); or the call exists and is reached through a path that does not return (a wait on the IOP); or the game's music start for later tracks goes through another entry that fails before `0x350168`.
Next piece: find the callers of the music-start wrapper in the menu code (guest-side: the caller `0x35f5c0` and what selects it; break on `0x35f5c0` and `0x345f30` family at blanks 3600-3720 and read their arguments and the return values of the poll to SFSV and of `0x703`), compare with the title's call.
No patch written. Not verified: the console's own call sequence at the menu (not run); whether the poll's reply differs between the title and the menu (the replies were not read).

## Follow-up 2026-10-14 (T-304, eighth piece): what gates the music after the title (diagnosis, no patch)

Binary md5 `be40828685d4...` (rebuilt by another agent after follow-up 7), same route, gdb watchpoints and breakpoints on generated functions (scripts `work/sound/{wp5..wp10,dec,dtxs,err,pl,snap3..5}.py`).

**Correction to follow-up 7.** `0x363F60` is not a music start: it is a loop over nine plug-in slots, and `0x35F5C0 > 0x345758` is the *destroy* of a handle; the one `0x409` (`0x32CEA8`) is the CRI transfer layer's own *destroy* (frees the channels, then RPC 9). The title's call at 2272 tears the old handle down. The second stream does not need that path. The "joints" at `0x5462A0/A8` are the file handles of the two stream objects (open `0x3223F8`, close `0x322588`), not an audio connection.

**The real pipeline** (EE side, all in the game's copy of the CRI library): stream object `0x3DC8D8` reads the file into a ring (SJ `0x3EA0A0`, 0x1A800 bytes at `0x515540`) > decoder handle h1 `0x3D9CE0` (`0x317968 > 0x317630 > 0x317580`: `0x316FC0` feeds, `0x317328` decodes) > PCM output ring (SJ `0x3EA0E0/0x3EA120`, 0x4000 bytes each) > transfer channels at `0x3DDC78` (`0x32D4E8 > 0x3254F0 > 0x325180`, `sceSifSetDma` to the sound processor). The ring is refilled only when the decoder consumes it.

**What is the same in the menu.** The player object `0x3D12D8` is started by `0x320740` at 3708-3710 exactly like the title at 2333-2335 (its state byte goes 1 > 2; the transfer channel is started by `0x32D038` at 3710; the decoder and the PCM output ring work the same up to 0x2000 samples, 3711-3712 against 2337-2340).

**Where it differs.** In the title the player's state byte moves 2 > 3 at blank 2339 (three frames in `0x320B68`) and `0x320CF0` takes over, the PCM drains, the decoder carries on. In the menu the player stays in state 2 (`0x320B68` runs every frame 3711-3725 and later) and never reaches 3: the decoder's output ring holds 0x2000 samples (`+0x34` of the handle = 0x2000 at 3716 and 3800, unchanged), the SJ's input is full (0x18124 bytes free to read), `0x317328` is not called after that, and the transfer channel (`ch2/ch3` of the table at `0x3DDC78`) is inactive (`+1` = 0) with the `+0x50/+0x51` flags of the `0x3E8B90` records at 0 (title: 1). So nothing drains and nothing is consumed: the two start reads are the only reads that ever come.
`0x320B68` leaves state 2 when the PCM ring holds at least min(0x4000, 2*player+0x48) bytes (`0x316528`/`0x316540` on the ring object `player+0xC`), or when the decoder's own state byte is 3 (end). The values of these two terms in the menu were not read (time box).

**Not found:** the term that is false in the menu (ring fill count against the threshold, or the decoder's state), and why it differs from the title when the data path is the same up to 0x2000 samples. Next piece: break on `0x320B68` with a0 = `0x3D12D8` at 3711-3716 and 2336-2338, and print `0x316528`/`0x316540` of `[a0+0xC]`, `[a0+0x48]`, `[a0+0x70]` and the handle's `0x317bb0`; the title's three frames show which term flips on the third frame; and `[a0+0x48]` (set in `0x320740` from the new file's header: sample rate, channels) may be wrong for the menu file.
No patch. Not verified: the console's values for these words (the dump `work/ref/x/02` has them at the same addresses: handle `0x3D9CE0`, player `0x3D12D8`, SJ `0x3EA0E0`; not compared).

## Follow-up 2026-10-14 (T-304, ninth piece): which term holds the player in state 2 (diagnosis, no patch)

Binary md5 `be40828685d4...`, same route; gdb scripts `work/sound/{terms,wp11,wp12,stop,units}.py`.

**The term.** `0x320B68` leaves state 2 when the PCM ring's free count `(0x4000 - sum of chunk lengths in list 0 of the object at [[player+0xC]+8]+8) / 2` reaches `min(0x4000, 2*[player+0x48])` (`0x32D260`, vtable method `0x32F7C8`). `[player+0x48]` is 0x980 in both cases (threshold 0x1300). Title, three frames: sums 0x4000, 0x2D00, 0x1A00 give free 0, 0x980, 0x1300: the flip is on the third frame, as the transfer channel returns chunks. Menu: the sum is 0x3C00 (chunks 0x1000, 0x1C00, 0x1000) and never changes, so the free count stays 0x200. Nothing returns chunks because the transfer channels (`0x3DDCF8`, `0x3DDD38`) were never started again: their start gate `0x32D328` requires the ring's sum to equal `[dc+0x2C]` (0x4000), and it is 0x3C00. The decoder is not at fault (it stops because the output ring is full).

**Where 0x400 goes.** The first stream's stop at 3633 (`0x32D038 > 0x325758`) is entered with the transfer channel mid-flight (`+2` = 1): a chunk (0xB40 on one channel, 0x400 on the other) is at the sound-processor side. The stop waits on `sceSifDmaStat` (the port always answers -1, no wait), puts the in-flight chunk back through the vtable method `+0x20`, and zeroes `+0xC/+0x10`. In the menu the ring ends 0x400 short; the console at the same screen has a single 0x400 chunk on list 0 (different state, not comparable word by word). Completion of chunks is also driven by the unit table at `0x3DE4A0` (`0x325CE8`): the console's units hold their own counters (`+0x3C` of the channel). I dumped the console's unit and channel words (script `units.py` written, port side not run for lack of time).

**Not found:** which chunk is lost (the 0x400-byte difference), and whether it is the `0x325758` put-back, the unit completion `0x325CE8` (polling a counter the sound processor side should write) or the port's `sceSifDmaStat` always returning -1. Next piece: run `units.py` at 2420/3630/3632/3640/3716 and compare with the console words printed above; watch the chunk lengths of list 0 of `0x3EE078`/`0x3EE0A8` from 3625 to 3640 with `0x325758` entry/exit sums.
No patch.

## Follow-up 2026-10-14 (T-304, tenth piece): the chunk lost at the stop (diagnosis, no patch)

**HANDOVER (read this first; everything below is for the music after the title).**
1. Route `work/sound/route_in.txt`, headless software, `--sound on`. Blanks: title music starts 2333; the old stream is stopped at 3633 (`0x32D038 > 0x325758`); the menu opens its new file at 3708 (`0x31CE78`), start reads follow, then nothing.
2. Objects (EE addresses): player `0x3D12D8` (byte +1 = state; +0xC = transfer channel `0x3E8BF4`/`0x3E8B90`; +0x48 = 0x980); decoder handle `0x3D9CE0`; stream object `0x3DC8D8`; read ring SJ `0x3EA0A0`; PCM rings (ring-buffer SJ) `0x3EA0E0`/`0x3EA120` (+0xC data, +0x10 free); destination chunk pools (list SJ, nodes of 16 bytes, length at +12) `0x3EE078` (left) / `0x3EE0A8` (right); transfer channels `0x3DDCF8` / `0x3DDD38` (+0x10 in-flight length, +0x14 destination, +0x2C DMA id); unit table `0x3DE4A0` (stride 0x44; +0x14 = block whose +0x3C counter the sound side writes: `0x3EF93C`, `0x3E9A7C`; +0xC = record buffer `0x3EF0C0`/`0x3E9200`, 16-byte records: chunk ptr +8, len +12).
3. Functions: `0x320B68` player state 2 to 3 (needs free count of `0x3EE078` >= min(0x4000, 2*[player+0x48]), `0x32D260`); `0x32D328` start gate (ring sum must equal `[dc+0x2C]` = 0x4000); `0x325180` per-channel send (takes a destination chunk, DMAs, queues a record); `0x325CE8` unit completion (counter acked: calls `0x330710`, which puts the record's chunk back, stream 0); `0x325758` channel stop (waits `sceSifDmaStat`, puts the in-flight chunk into stream 1 of `0x3EE078`); `0x32F880`/`0x32FA30` list SJ get/put (stream a1; get-all uses a2 = 0x7FFFFFFF).
4. Scripts: `work/sound/chunks.py` (ring lists, channels, units at chosen blanks and at the stop's entries), `work/sound/ops.py` (every get/put on `0x3EE078` with the call chain, blanks 3631-3637; edit the window and address).
5. Measured: the left pool sums 0x3C00 after the stop and stays; the right pool sums 0x4000. Everything else is the same on both sides.
6. Suspected, not measured: see below.

**Measured this piece.** At the stop (3633) the left channel has a 0xB40-byte chunk in flight (destination `0x126100`), the right one 0x400. `0x325758` puts each into stream 1 of its pool (`3E6C90:B40`, `3E6DA0:400`). Next frame (3634) a consumer takes stream 1 whole (`0x32F880`, a1 = 1, a2 = 0x7FFFFFFF, a3 = `0x3D4A80`); its caller is `entry_00330848`, entered through `dispatchGuestBranch` (the scheduler thread), a guest routine for which Ghidra has no function; it is an indirect callback and runs every frame in normal playback too. After that get, the right chunk (0x400) is returned to stream 0 the next frame (right pool: 0x1740 > 0x1B40 > 0x1F40 > ... > 0x4000); the left chunk (0xB40) is not returned: left stream 0 goes 0xC00 > 0x1000 by the 0x400 record only. From 3636 the records `128040/0x1C00` and `125C40/0x1000` come back (`0x330710`), the left pool ends 0x1000+0x1C00+0x1000 = 0x3C00 and nothing more is due (the units keep ticking, the counters at `0x3EF93C`/`0x3E9A7C` advance one per frame, written by `0x325CE8` and by the sound side, so the acknowledgement path is alive: suspect 2 is not it).
The three suspects: (1) `sceSifDmaStat` always -1 (`ps2xRuntime/.../Stubs/SIF.cpp:326`): the stop never waits; with a transfer still counted in flight, the console puts the chunk back only after the DMA ends. Not shown to matter: the right chunk is handled the same way and is returned. (2) the unit counters: alive, not it. (3) the 0xB40-length chunk: a length that is not a multiple of 0x400 (0x340 + 0x800 merged at the get) is returned by the stream-1 consumer on the right (0x400) and not on the left (0xB40). This is the lead: what `entry_00330848`'s callee does with a stream-1 chunk of 0xB40 against 0x400.
**Next piece:** find the callee reached from `entry_00330848` that calls `0x32F880` (a1 = 1) and read what it returns to stream 0 for each chunk (break on its entry: the chunk, what it puts back, with 0xB40 and with 0x400). If it returns the chunk by length rounded or bounded by something that the port computes differently (a division or a conversion), that is the defect. The console's dump `work/ref/x/02` has the pool words at the same addresses for comparison (`0x3EE078` list: a single 0x400 chunk).
No patch; no rebuild; the shared binary was not touched.

## Follow-up 2026-10-14 (T-304, eleventh piece): the 0x400 is not lost at the stop; it is lost at blank 3318, in the sound side's replies (diagnosis, no patch)

Binary md5 `be40828685d4...`, route `work/sound/route_in.txt` with `--shots` (without it `ScreenshotWriter::on_vsync` is never hit), gdb scripts `work/sound/p11/{rec,nodes,un,un2,un4,un5,pl}.py`. Several runs: the pool states below are the same in all of them (the 0xB40 chunk of the tenth piece was not seen again; in these runs the left chunk in flight at the stop is `127440:800`).

**The stop is innocent.** At 3633 the left in-flight chunk (`127440:800`) is put into stream 1 of the pool, collected at 3634 into the record buffer `0x3EF0C0`, and comes back whole inside the reply `125c40:2000` at 3636. The right chunk (`12a600:400`) likewise. Nothing is dropped by `0x325758`, by the stream-1 collector (`0x3307B0`, a callback of the unit at `0x3DE4A0`, called from `0x325CE8`; it copies every chunk of stream 1 into a 16-byte record) or by `0x330710` (which puts each reply record back into stream 0). Left pool at 3640: `127c40:1000, 129040:c00, 125c40:2000` = 0x3C00 (base 0x125C40, size 0x4000); the hole is **`128c40:400`**. Right pool: `12aa00:3340, 129d40:cc0` = 0x4000.

**The pipeline.** Each blank, `0x325CE8` (1) when the counter at `[unit+0x14]+0x3C` has advanced runs `0x330710` on the record buffer: the buffer then holds the *sound side's reply* records (stream 0, destination = the route entry `0x3EEE38` left / `0x3EEE4C` right, chunk address and length), which are put back into stream 0; (2) runs the collector, which fills the buffer with the chunks the EE has just sent (stream 1, destination `0x82108` left / `0x8211C` right: IOP-side object addresses), and DMAs it to the IOP. The sound side answers in the same buffer with the chunks it has consumed. The replies are the pool's chunks in ring order (`125c40`..`129c40`, wrapping), but cut at its own boundaries, not at the EE's sends: a sent chunk may come back as two replies. In steady state the pool is 0x3C00-0x4000 in the pipeline plus one free chunk at the head.

**Where the hole appears.** Logged every blank 2352-3561 (`un4.py`, parsed with the replies in arrival order): the replies are contiguous in the ring (each starts where the previous one ended) up to blank 3317, and from **3318 on every lap skips `128c40:400`**: 3315 `127c40:400`, 3316 `128040:800`, 3317 `128840:400`, then 3318 `129040:400` (the EE had sent `128840:800` at 3309: the first half comes back at 3317, the second half, `128c40:400`, never). Because nothing sends it, the free list never holds it again; from 3318 the left pool's steady state is 0x3C00 and it still is after the stop. The first 960 blanks of the title music are contiguous. The same gap recurs each lap (3318, 3328, 3337, ... 3558).
So the loss is **a reply the IOP-side code does not give** for the second half of a chunk it returned in two halves, at a point in the middle of normal playback, not a defect of the stop; the music keeps playing because the pool is only needed whole when a new stream starts (`0x32D328`).

**Corrections to earlier pieces.** The ninth and tenth pieces put the loss at the stop (`0x325758`, `0x325758` plus the stream-1 consumer, the 0xB40 chunk); it is earlier, and the pool is already short when the stop comes. The "consumer reached through `entry_00330848`" is the stream-1 collector `0x3307B0` (the fragment `0x330848` is its loop test): it only copies chunks into records, it does not decide lengths. Suspect 1 (`sceSifDmaStat` always -1) is not involved.

**Not found, and the next piece.** Why the sound side does not return `128c40:400` at 3317-3318. The sound side is the IOP's CRI driver (IOP-side objects at `0x82108`/`0x8211C`; the replies are written by IOP code into the buffer `0x3EF0C0` after the EE's DMA): watch the IOP RAM words of that object (the chunk list and the consumed-bytes counter) from blank 3305 to 3320 (the IOP emulator keeps IOP RAM; `ee_iop.py` and `ee_iop2.py` in `work/sound` show how it was read before), and find (a) what is different at 3317 from the 960 laps before (a chunk of a length that is not a multiple of 0x400 sent just before: the right side sends `0x8c0`, `0xcc0`, `0x340`, `0x4c0`, `0x740` chunks around then; or a counter wrap), (b) whether the ADMA position the SPU2 side reports to the IOP (a runtime call: the SPU2 bridge's position/ack, not the EE's transfer-status call) advances in a unit that skips the half. Then compare with the console's `work/ref/x/02` IOP object (docs/knowledge/10 says where IOP RAM is in the savestate).
No patch; no rebuild; the shared binary was not touched.

## 2026-10-14, twelfth piece: the lost half is not an interrupt lost at the DMA hand-over

Reading tested: the sound side returns one 0x400 block per half-buffer-end event, and one event was
folded into another around blank 3317. Measured with breakpoints on `IopMemory::startSpu2Dma`,
`IopMemory::completeSpu2Dma` (return value read at its return address) and
`IopIntrman::dispatchInterrupt`, blanks 3300-3326, binary md5 `e42befa019a6773fd57e3048640993c2`:

- Every blank has the same number of starts, completions (all returning 1) and dispatches of IRQ 0x24
  (DMA channel 4, SPU2 core 0): 42 of each over the 27 blanks, 1 or 2 per blank. Core 1 never moves.
- Nothing is different at 3317 (1/1/1). The only irregularity is two blanks in a row with 2 (3318, 3319),
  which is a catch-up, not a loss: the totals still agree.
- So the three counts (chip transfers started, finished, handler entries) agree through the window, and the
  event-folding reading is **false**: `pendingDmaInterrupts` keyed by irq (iop_emulator.cpp, `schedulePendingDma`)
  could fold two starts, but no start is overwritten here.

Not measured: the byte count of each DMA (BCR) and the driver's own count of blocks returned. Next piece:
log the length of each start (`m_hardware[chcr-4]`) in the same window and compare with the left chunk list
(IOP object `0x82108`): the chunk sent whole as 0x800 at 3309 should show as one 0x800 DMA or two 0x400 ones.
If it is one 0x800 DMA, the driver counts one completion for two blocks, and the fault is in the driver's
bookkeeping of a DMA longer than 0x400 (the guest), not in the runtime. No patch 0059.

## 2026-10-14, thirteenth piece: the lengths, the chip's clock, and a divergence found in the IOP's interrupt delivery

Binary md5 `e42befa019a6773fd57e3048640993c2` for the first run, `5201e7ad8e277ee4d6b73748d661699d` for the rest (rebuilt by someone else in between). Scripts: `work/sound/p11/tl.py`, `io.py`, `ic.py`.

**Lengths (blanks 3300-3326).** Every transfer to the chip is core 0, `0x800` bytes: 42 of 42 (breakpoint on `don_spu2_dma_write`). Nothing is different at 3317. The left chunks the EE sends are `0x400` or `0x800` long, the right chunks also `0x740`, `0x8c0`, `0x340`; the sound side returns ring ranges cut at multiples of `0x400` whatever the chunk boundaries (for example a sent `125c40:800` comes back as `125c40:400` and the second half joins the next return `126040:800`). The right side's returns are contiguous through the window; the left's are contiguous up to `128840:400` at 3317 and then jump to `129040:400` at 3318 (the hole `128c40:400`).

**"Catch-up" is not one.** 42 transfers in 27 blanks is 1.56 per blank: 0x800 bytes per 10.67 ms is 48 kHz x 4 bytes, the sample-rate cadence, so pairs of blanks with 2 transfers (3311-3312, 3318-3319) are the normal rounding of 1.56 and not a late run.

**How the chip is clocked.** Not in a burst: `IopEmulator::Impl::runCycles` (`third_party/PS2Recomp/ps2xIOP/src/emulator/iop_emulator.cpp:609-646`) calls `syncHostSpu2()` before every execution slice (`kDefaultSlice` = 256 cycles) and after the last, advancing the chip by exactly the cycles elapsed (`don_spu2_advance`, `src/spu2bridge/don_spu2.cpp:508`); the EE feeds the IOP one slice at a time (`EeScheduler.cpp:85`). The chip is never more than 256 IOP cycles behind, so a double crossing without a driver run between is not possible; the "run in a burst" reading is also false.

**A divergence found (not yet shown to be the cause).** `IopIntrman::dispatchInterrupt` (`ps2xIOP/src/emulator/imports/iop_intrman.cpp:95`) runs the handler as a nested call between two slices without looking at the guest's interrupt state (`CpuSuspendIntr`/`CpuDisableIntr` set `IopMemory::m_interruptControl`, offset `0xe0`); only the I_STAT/I_MASK path (`iop_emulator.cpp:278`) honours it. Measured at the entry of `dispatchInterrupt`, blanks 1-3500: of 5468 handler entries (5449 for IRQ 0x24, 19 for 0x28), 5237 were taken with that flag at 0 (5230 + 7) and 231 with it at 1 (219 + 12). The flag is one global, not saved per thread, so a thread that suspends and then blocks leaves it at 0 for the others: the 96% does not by itself prove that handlers cut critical sections. What it does show is that the emulator cannot tell. On the console the DMA-end interrupt waits until the thread that suspended interrupts resumes them; here it enters at the next 256-cycle boundary wherever the thread is.

**What this suggests, not proven.** A left chunk's bookkeeping updated in thread context (shrinking the head chunk after the first half is consumed) and in the DMA-end handler (the second half), with the handler entering inside the thread's critical section at one slice boundary, would lose exactly one half and never recover. It fits "one lap in 960", "left only" (the left list is the one being edited when the handler enters) and "determinism" (the cycle count is fixed). It is not shown: I could not read the IOP-side chunk list (the `0x82108` of the records is a SIF destination id, not an IOP address: IOP RAM at that address holds EE-style words `3eee24 1 81600 3ee078`).

**Next piece.** (1) Make the flag per-thread (saved and restored at the context switch of `IopKernel`) and defer a pending DMA/SPU2 interrupt while the *running* thread has it at 0; test: a thread that does CpuSuspendIntr, a pending DMA end and a handler that must not run until CpuResumeIntr (tests/runtime, patch 0059). (2) Prove it on the route as before. If the hole still appears, the cause is elsewhere in the driver and the IOP list needs reading (find the object by searching IOP RAM for the chunk values `128840`, `800` after blank 3309).

## 2026-10-14, fourteenth piece: patch 0059, the IOP's interrupts respect the suspend state; the hole is still there

**What the emulator kept where, before.** `IopMemory::m_interruptControl` (offset `0xe0`, the I_CTRL register) is the only suspend state. `CpuSuspendIntr` (intrman 17), `CpuResumeIntr` (18), `CpuDisableIntr`/`CpuEnableIntr` (8, 9) write it. It is one global: `IopThread` has no field for it and the thread switch (`IopKernel::beginNextReady`/`endTimeslice`) does not touch it. The only reader was the I_STAT/I_MASK path (`checkInterrupt`, `iop_emulator.cpp`); the DMA/SPU2 handlers go through `servicePendingDmaInterrupts`, called after every guest instruction by `runCpu`, which did not read it. A handler entry did not change it and its return restored nothing. Measured at the entry of `dispatchInterrupt`, blanks 1-3500: 5237 of 5468 entries with the flag at 0 (96 %).

**The patch** (`patches/ps2recomp/0059-iop-interrupts-respect-suspend.patch`): the state is per thread (`IopThread::interruptsEnabled`, saved at `endTimeslice`, loaded at `beginNextReady`; a thread that blocked or ended leaves the processor idle with the state enabled); `servicePendingDmaInterrupts` returns at once while the running context has it at 0 (the interrupts stay pending, taken at the first check after the resume); inside a handler the state is 0 and the interrupted state returns with it; a host SPU2 event raised while the same interrupt is waiting is counted (`pendingExtraInterrupts`), one entry per raise. Not covered: timer and alarm callbacks. Test: `tests/runtime/iop_intr_suspend_test` (a thread suspends, writes A, runs more than forty slices, writes B, resumes; two interrupts raised in the middle: no handler before the resume, two after, none saw A != B; a second thread stays interruptible while the first sleeps suspended): 5 checks fail on the base tree, all pass with the patch. `iop_host_spu2_test`, `module_buffer_load_test`, `iop_heap_capacity_test`, `iop_heap_reuse_test` pass with it.

**On the route (binary `fc12320a7a5247c5e35fb7f62cbf6491`, built with the patch).**
- Handler entries over blanks 1-3500: 5449 for IRQ 0x24 and 19 for 0x28, the same totals as before (5468): none lost, none added, so no handler is deferred for long. The "share with interrupts suspended at entry" can no longer be measured at that point (the handler runs with the state 0 by design); by construction no handler starts inside a suspended section.
- The returned blocks (`un4`, blanks 2352-3561): **the hole is still there**, identical to before the patch: `gap at b=3318 prev=128840:400 next=129040:400 (missing 400)` and the same one every lap after (the lost chunk is never put back, so every lap skips it). The left pool is still 0x3C00 at the stop, and the menu's player was not re-checked (nothing changed in what feeds it).
- Recording (`--wav`, the full route, 173.5 s): two runs of the patched binary give the same SHA-256 (`57da4996…`), so the chip is still driven deterministically. The pre-patch binary's recording differs in hash (`45bbf5e1…`) and in some peak levels late in the route, but has the same silent stretches (5-second windows with peak 0 at 10-30 s, 65 s, 95 s). Sound in the menu is not back.

**Conclusion.** The loss is not an interrupt entering a critical section: with every handler kept out of suspended sections the same block goes missing at the same blank. The patch stays (correct on its own; it matches the console). The hole is one chunk lost once, at blank 3318, in the sound side's bookkeeping.

**What to read next.** The IOP-side CRI driver object that holds the left stream's chunk list; its address is not `0x82108` (that is the SIF destination id in the records). Find it in IOP RAM by searching for the chunk values the EE sent (`128840` with length `800` at blank 3309) and watch the chunk list and the consumed-bytes counter from 3309 to 3318: the question is what the driver does when the head chunk `128840:800` is consumed in two 0x400 pieces while the next chunk (`129040:400`) is already queued. Compare with the console's IOP RAM in `work/ref/x/02` for the same object.

## 2026-10-14, fifteenth piece: the defect found, a thread with interrupts suspended was preempted (patch 0062); the music is back

**Where the list is.** The sound driver's object is at IOP `0x81600`; its queues of extents (`{addr, len}`) have their heads at `+0x18` (queue 0) and `+0x1c` (queue 1), the free-node head at `+0x14`, and the 16-byte nodes (`next, 0, addr, len`) in the table at `0x125b40` (the left channel's; the right channel's is at `0x129c40`). The code (guest, run faithfully by the emulator): `0x74bf8` takes up to n bytes from a queue's head (splitting the head node), `0x74f2c` puts a leftover back at the front (merging when adjacent), `0x74d90` appends an extent at the tail (merging when adjacent, else taking a free node); the three wrappers (`0x74b94`, `0x74ed8`, `0x74d3c`) bracket them with a nested lock, `0x7337C`/`0x733C8` (a counter, `CpuSuspendIntr` on the first entry, restore on the last). The step function at `0x76920` asks for `(samples + 1) * 2` bytes from queue 1 (so `0x402`, one sample of look-ahead), appends the `0x400` it used to queue 0 and gives the 2 spare bytes back; a second context pops queue 0 whole (`n = 0x7fffffff`, stack near `0x11bde8`) and returns the extents to the main processor.

**What happened at blank 3316.** Watching the node writes (hardware watchpoints on the nodes' addr/len words, the guest pc read from the emulator's CPU state): the step appended `{128840:400}` to queue 0 as a new node; the second context popped that node and put it on the free list; the step's second append (`{128c40:400}`) then merged into the tail it had found *before* the pop, a node already on the free list (`128840:800, next = 125b70`). The next append took that free node and overwrote it: `128c40:400` was never returned. The loss repeats because it is never put back (the pool stays at 0x3C00).

**The defect (in the port).** The append is inside the driver's `CpuSuspendIntr` lock, and the second context is a thread: `IopKernel::beginNextReady` picks the best ready thread at the end of every slice of at most 256 cycles without looking at the preempted thread's interrupt-suspend state. On the console the scheduler runs from interrupts and system calls: a thread with interrupts suspended is never switched out. Patch 0059 had made the suspend state per thread and kept handlers out of sections, but the thread switch itself still ignored it (as the thirteenth piece suspected, one level up: not a handler, a thread).

**The fix** (`patches/ps2recomp/0062-iop-no-preemption-while-suspended.patch`; 0061 is the pad patch): `endTimeslice` remembers a thread whose slice ended while it was still runnable (not yielded, not blocked) with interrupts suspended; `beginNextReady` runs it again before any other until it enables interrupts, blocks or ends. Delay wake-ups still happen on time (the woken thread waits). Test `tests/runtime/iop_no_preempt_test`: a priority-30 thread suspends interrupts, writes A, runs ~47 slices, writes B, resumes; a priority-20 thread woken by a 20 us delay in the middle must not run before the resume and never see A != B: 2 checks fail on the tree with patches 0001-0060, all pass with 0062; `iop_intr_suspend_test` and `iop_host_spu2_test` pass with it (built on the host with 2 jobs, folder of its own).

**On the route** (binary `ce723b9aed849d8e6f076e876ff220e0`, build `linux-dev done in 0h 00m 07s`):
- The returned blocks (`un4`, blanks 2352-3561): no gap (`parse5.py` prints nothing; before: `gap at b=3318 ... (missing 400)` and one per lap after).
- The menu's player (`0x3D12D8`, byte +1, sampled every 120 blanks): state 3 from blank 2400 on and in the fight up to 10200 (before: 2, never 3); the EE-side left list sums to 0x400-0xc00 in a steady rhythm (before: 0x3C00/0x4000 stuck).
- Recording of the full route (173.5 s, `--wav`): per 5 s peak, the windows at 65 s and 95 s that were exactly 0 now carry sound (peak 10409 and 11056, 100 % of samples non-zero), and the last 70 s (the fight) sit near full scale; two runs give the same SHA-256 (`053dd354...`). The first silence (10-35 s) is still there: it is before the title music starts at blank ~2300 (not investigated; it may be the boot's own silence).
- Not measured separately: the music archive's read cadence (the player state and the wav levels stand in for it). Not run: the regression suite, `fetch_ps2recomp.sh --reset`.

## 2026-10-14, sixteenth piece: the music in every mode (nothing changed in code; measured, not heard)

Binary md5 `ce723b9aed849d8e6f076e876ff220e0` for every run (`md5sum` checked before each). `--sound on --wav`, `DON_TRACE_FRAMES=1`, headless, software renderer, each run under gdb with the `un4` check made whole-run (`work/sound/p11/gz.py`, `parse6.py`, `z/run.sh`): the blocks the sound side returns, the menu player's state byte every 120 blanks. A "restart" is a gap whose next block is the ring's base (`0x125c40`): a new track starting; a "LOSS" would be a skip in the middle of a stream. There was **no LOSS in any run**.

| Route | Blanks | Returned blocks | Restarts / losses | Liveness |
|---|---|---|---|---|
| Survival (`survival_result.txt`) | 6000 | 2721 | 5 / 0 | ok, largest gap 180 |
| Multiplayer (`multiplayer_match.txt`) | 16700 | 13818 | 7 / 0 | ok, largest gap 240 |
| Main mode (`work/soak/r8.txt`, stopped at blank ~31000 by the 800-second wall limit it was given, under gdb) | ~30900 | 28445 | 21 / 0 | ok, largest gap 180 |

**Read cadence.** In the fights the sound side returns about 615-620 blocks per 600 blanks, steady (Multiplayer: 6000-16200 all between 612 and 622; main mode: 4200-23400 and 25200-30000 the same). It dips only in menus and loads (2400-3600: 380-470; main mode's second load 24000-25200: 530 and 395; the results screen in Survival, 272 in the last 600). The player's state byte is 3 through every fight; it falls to 0 at the loading and result screens (Survival 5760 on, main mode 24720 and 24960, Multiplayer 3360, 3720, 4440-4560) and comes back.

**Level per phase (5-second peak; nz = share of non-zero samples).** Boot 0-40 s silent by design (see below). Title and menus: peak 12-27k, nz 0.68-1.0 (some quiet moments between notes). Fights: 30-32k, nz 1.00. Survival result screen (blank 5850 on): music fades (peak 12.6k, nz 0.39 in the last 5 s). Multiplayer: peak 32767 for the whole match from 90 s. No phase that should have music is silent. Clipping is small: 0.09 % of samples at |x| >= 32700 in Multiplayer's fight, 0.02 % over main mode's fights, 0 in Survival's. Nobody has listened.

**The 10-40 s silence is the boot.** Shots of the main route: 10 s the WARNING screen, 20-30 s the Q Entertainment / 8ing logos, 30-38 s the BANDAI logo (the memory-card prompt at 4 s), then white frames (the opening video skipped) until the title at ~45 s; the title music starts at blank ~2700 (45 s). Not checked: whether the console plays any sound over these logos (no evidence either way).

**Regression sound check:** `DON_REGRESSION_SOUND=1 uv run --no-project --with pytest python -I -m pytest tests/regression -m slow -q -k alive_with_sound` -> `4 passed, 4 deselected in 634.59s (0:10:34)`.

Not verified: how the music sounds; that each track is the right one for its phase; the part of main mode after blank ~31000 (not gdb-checked; its plain run, journal 033, was alive to 39240).
