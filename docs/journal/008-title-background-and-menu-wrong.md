# 008 · The title screen's background and the whole main menu are drawn wrong

- **Date:** 2026-10-13
- **Task:** T-114
- **Status:** open

## Symptom

With the route of `tools/run/scripts/to_main_menu.txt` the port reaches the title screen and the main
menu (journal 007: the dialog is answered, one Start tap skips the video, the title is up at frame 2700,
the menu at frame 3000). The two clean shots of that route were compared with the console's own pictures
(the reference GS dumps replayed with `build/gs/gsreplay --renderer software`, software renderer on the
port's side, `tools/verify/image_parity.py`):

```
port title (frame 2700) vs 01-title replay : SSIM=0.5344 MAD=45.55
port title (frame 2700) vs 01-title.png    : SSIM=0.5395 MAD=46.53
port menu  (frame 3000) vs 02-menu replay  : SSIM=0.0804 MAD=95.24
port menu  (frame 3000) vs 02-menu.png     : SSIM=0.0897 MAD=94.80
(reference sanity: 01-title.png vs its replay SSIM=0.9811, 02-menu.png vs its replay SSIM=0.9456)
```

* **Title**: the *foreground* is right — the BATTLE STADIUM D.O.N logo, the three characters, the
  "DRAGON BALL Z ✕ ONE PIECE ✕ NARUTO" banner, the red 「バトルスタジアム ドン‼」, "PRESS START button" and the
  four copyright lines are all there and legible. What is wrong is the **background**: the console shows
  the stadium (dark blue, spotlights, a crowd), the port shows a flat purple/magenta geometry of large
  triangles (mean RGB (105,75,163) against the console's (97,90,113)).
* **Main menu**: not a menu at all. The port's shot is a chaotic magenta/blue mosaic of large triangles with
  no text, no panels and no cursor (mean (147,9,162)); the console's menu (ひとりでバスタ / サバイバルチャレンジ /
  プラクティス over the stadium, with the かんたん操作 / デザートもゲット!! bar) is (100,108,124) and SSIM 0.08
  against the port's shot. The mosaic is not a transition: shots 300, 360, 420, 480, 540, 600 frames after
  the menu's first frame are all the same kind of picture (means (147,9,162), (148,7,157), (146,9,158),
  (144,8,153), (134,60,146), (148,8,157)).

The route itself is not blocked: the game keeps running (`OK_VSYNC_LOOP`), the frames advance and the run
ends with the script's `quit` (exit code 0). Both screens simply draw the wrong picture.

## Classification

Not a crash, not an unimplemented syscall, no `missing-target`. The run is `OK_VSYNC_LOOP` throughout and
the log has no error line.

## Investigation

1. **The display registers are the console's.** `DON_TRACE_GS_REGS=1` during the route (3300 lines) against
   `tools/verify/dump_regs.py work/gsdump/02-menu.gs.zst --decode`: PMODE 0x8067, SMODE1 0x740814504,
   SMODE2 0x1, SRFSH 0x8, SYNCH1 0x7F5B61F06F040, SYNCH2 0x33A4D8, SYNCV 0xC7800601A01801, DISPLAY1
   0x1BF9FF0183227C, DISPLAY2 0x1BE9FF0183227C and EXTBUF/EXTDATA/EXTWRITE/BGCOLOR/BUSDIR/SIGLBLID 0 are
   **equal**; DISPFB1/DISPFB2 are 0x148C/0x8000000148C against the dump's 0x1400/0x80000001400, i.e. the
   same alternating buffers in the other field, as at the memory-card dialog. The only differences are the
   two already known from the dialog: CSR (port 0x2200 = FIELD+RESET, console 0x551B600C with the revision
   byte) and IMR (port 0xFF00, console 0x7F00). The title's frame (2700) has the same values with DISPFB
   0x1400. So the set-up and the display are not the cause.
2. **The VU1 interpreter stops in the microprogram these two screens use.** The same runs log

       [VU1 reserved lower] cycle=<n> pc=0x168 instruction=0xfffffff8

   **74854 times** in the menu run and 16854 times in the route run that reaches it (per frame ~17), and
   **0 times** in every run that ends at the memory-card dialog. All the lines are the same pc and the same
   instruction word, which is not a VU1 instruction on either half: the interpreter has run into data.
   `VU1Interpreter::reportReservedInstruction` (`vu/ps2_vu1_core.cpp`) also sets `m_stopRequested`, so each
   run of the microprogram **ends there**, leaving whatever the frame was drawn with so far: that is the
   mosaic. Which earlier instruction sends the microprogram to 0x168 (a branch, a flag, the `i` field) is
   **not determined**; the microprogram's own source (where the game uploads it from) would settle it.
3. **The RAM comparison.** `DON_DUMP_RAM` at the menu (frame 4440) against the console's own images:
   42777 differing words of 1,219,072 in `work/ref/x/02` (the menu) and 45952 in `work/ref/x/01` (the
   title): both about the same, so the EE-side state does not single out either screen; one detail stands
   out and is reported as an observation, not a cause: at 0x001717BC (inside the region the runtime
   registers as code, 0x100000-0x4C9D80) the port has a heap pointer (0x0140EF58) where the console has an
   instruction (0x46011041) and the words around it are equal, so one word of that area differs.
4. **One word of the game's own code is overwritten in the port.** At 0x001717BC (inside the first ELF
   LOAD segment, 0x100000-0x4C9D80, the region the runtime registers as code) the ELF on the disc, the
   console's image and the port's image are:

       ELF disc/SLPS_256.75     0x46011041   (an MIPS COP1 instruction; the words around it are 0xE7A10048,
                                             0xC6020000, 0xC6212110, 0x46012040, 0x46000834 - a float routine)
       console work/ref/x/02    0x46011041   (unchanged)
       port (menu dump)         0x0140EF58   (a pointer into the game's heap)  <- different

   The same value is there in the title dump. A scan of the whole code region for words where the port has
   a value in the heap's range and the console has something else finds 14; thirteen of them are inside
   tables that legitimately hold pointers or that the game builds as the screen runs (the console's capture
   is from a different moment), so they are not conclusive. 0x1717BC is: the ELF proves the word is an
   instruction, so **something in the port writes a heap pointer over one instruction of the game's float
   code**. Who writes it is not determined (a hardware watchpoint on the host copy of guest RAM fired in the
   host's own sleep loop, i.e. the watch unit was hit by a neighbouring write, and the run had to be given
   up). It is reported as a second finding, not as the cause of the mosaic: the corrupted word is in a float
   routine, not in the VU1 path.
5. Ruled out: a missing recompiled function, an unimplemented syscall, a crash (none in the logs), and the
   display set-up (item 1).

## Cause

**Not determined**, but localised: the two screens are drawn by a VU1 microprogram the runtime's
interpreter abandons at pc=0x168 (`[VU1 reserved lower] ... instruction=0xfffffff8`), so the display lists
of those screens are incomplete and the picture is the palette of whatever was drawn before. The
microprogram is fine on the console (the reference replays of the same screens are clean), so either the
interpreter mishandles an instruction before 0x168 or the microcode the game generated differs; the next
step is a trace of the microprogram from its entry (the runtime's VU1 state is not reachable from the game,
so it needs gdb on the interpreter or a `DON_`-guarded dump inside it).

## Fix

None applied, and none of the allowed kinds for this task: the drawing path is `src/gsbridge/` and
`src/vubridge/`, not a file this task may touch, and forcing a picture would be rule 4. This entry is the
finding.

## Verification

* `tools/run/scripts/to_main_menu.txt` with `--shots --renderer software`: exit 0, `title.png` at frame
  2700, `main_menu.png` at frame 3000, `[don] GS: 3300 vblanks`.
* The comparison figures above (`tools/verify/image_parity.py`); the pictures and the run's logs stay under
  `work/` (`work/shots/t114route1`, `work/orchestrator/t114/`).
* The route was run **three times** (`work/shots/t114route1..3`, `--renderer software`, `--seconds 900`):
  each exit code 0, `title.png` at frame 2700 and `main_menu.png` at frame 3000, and the two shots are
  **byte-identical** across the three runs (SSIM=1.0000 MAD=0.00, the same md5: 197454465ce98026a8c59f4e3e0108d0
  for the title, ce8c17fa21df4e9b28ad8b7f940cc4a9 for the menu). So the wrong picture is reproduced exactly,
  which makes the comparison above meaningful.

## Follow-up (T-202, 2026-10-13): the menu was the VU1 interpreter, the background is not

With `DON_VU1=microvu` (the library's microVU through `src/host/vu_glue.*`) the same route and the route
of `tools/run/scripts/to_match_1v1.txt` were run again (`build/linux-wire/don`, headless, software
renderer, `--shots`). Two separate results:

* **The menu is fixed by the engine.** Port menu at frame 3060 vs `02-menu-replay.png`:
  `SSIM=0.5162 MAD=52.23` (was 0.0804 / 95.24 with the runtime's own VU1 interpreter in the Symptom
  above), and the screen is a menu again: the three panels ひとりでバスタ!! / サバイバルチャレンジ /
  プラクティス with their plates, the かんかん戦え! チケットもゲット!! bar, the ↑↓ 選択 ○ 決定 ✕
  タイトルへ戻る legend and the side banners, the first panel highlighted with its BATTLE badge. The
  runtime's `[VU1 reserved lower] pc=0x168 instruction=0xfffffff8` lines (about 17 per frame on the title
  and the menu, 10347 in a 3300-frame run) do not appear at all with either library engine.
* **The background is not.** The port's title still gives `SSIM=0.5326 MAD=40.92` against
  `01-title-replay.png` with microVU (0.5344 with the runtime's interpreter) and it is still the flat
  magenta/purple geometry of large triangles. The same background is wrong on every screen the title's
  scene is behind: the character select (frame 3600, `SSIM=0.4038 MAD=60.65` against
  `03-charselect-replay.png`; the Naruto portrait, the name plate, the icon grid, the four fighter
  portraits and the footer are all right), its practice variant (frame 4980, 0.4011 / 58.16, Naruto 1P
  and Sasuke Com selected as the reference route intends), the stage list (frame 5880: the STAGE SELECT
  banners and the names 精神と時の部屋 / ゴーイングメリー号 / 海上レストラン are right and legible, the
  preview behind them is the same geometry) and the match (journal 010).

What the console has behind those screens is a **3D stadium scene**: dark blue stands with a crowd,
white/blue spotlights with light beams, a large screen showing characters, and (on the menu and the
character select) a wooden platform. It is essentially static at these moments (the console's own
screenshot and its dump replay agree to SSIM 0.98 on the title, 0.95 on the menu). The port draws, in
the same place, large flat magenta/violet triangles with sharp edges and thin diagonal streaks, and that
pattern *does* animate between frames. Two leads, neither checked: the port's 2D art (logos, portraits,
icons, text) is drawn correctly everywhere while the 3D scene is not, so the broken part is whatever
draws the world; and a texture whose base pointer (TEX0 TBP) is wrong makes PCSX2's GS show exactly this
kind of unstyled coloured triangle. The third possibility is the one this entry already names: part of
the scene is a video/texture the port's MPEG path never decodes (journals 007 and 009).

## Follow-up 2 (T-202, 2026-10-13): the background is the game's own vector data, not the VU1

Journal 013 goes down the 3D path stage by stage and finds the first differing stage **before** VIF1: at
the title the display list in guest RAM is 99.5 % identical to the console's and the VU1's matrix is
byte-identical, but 1080 sixteen-byte vectors the console writes are wrong in the port (1 ULP higher in
the lanes it writes and only half the lanes written), and both VU1 engines then carry that data through
byte-identically (their data memories agree to 99.6 %), the microprogram saturates on it and the GS
clamps the result to its guard band — the flat magenta/violet triangles described above. The background
of the title, the menu, the character select and the stage list, and the missing stage and fighters in
the match (journal 010), are all this one defect. See journal 013 for the tools, the figures and what the
next person should look for in the recompiled code.
