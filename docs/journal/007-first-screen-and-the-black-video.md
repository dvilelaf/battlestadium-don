# 007 · The first screen is answered, and the opening video plays black

- **Date:** 2026-10-12
- **Task:** T-111 step 3 / T-112
- **Status:** open

## Symptom

With the input layer wired into the host program (T-112) and the game's multitap wrappers replaced (see
below), the memory-card dialog is answered by a script and the game goes on:

```
build/linux-dev/don --headless --seconds 600 --shots work/shots/memcard \
    --input-script tools/run/scripts/answer_memcard.txt disc/SLPS_256.75      # exit code 0
[don] input script tools/run/scripts/answer_memcard.txt: 11 command(s), last frame 3450
[don] input script quit at frame 3450
[don] GS: 3450 vblanks, 0.45 ms in the GS per vblank (max 852.48), 21.1 MiB of GIF data
```

The shots (640x448, software renderer):

| shot | frame | mean RGB | what it shows |
|---|---|---|---|
| `memcard_dialog` | 240 | (33,33,33) | the memory-card dialog, cursor on 「いいえ」 |
| `memcard_left` | 300 | (33,33,34) | the same dialog, cursor on 「はい」 (2125 pixels changed) |
| `memcard_after_circle` | 390 | (254,254,254) | the health warning (bright text on white) |
| `next_1` | 990 | (255,241,228) | a coloured screen (a logo or the first video frames) |
| `next_2` | 1590 | (233,233,233) | a grey screen |
| `next_3` | 2190 | (0,0,0) | black |
| `next_4` | 2790 | (0,0,0) | black, identical to `next_3` |
| `next_5` | 3390 | (0,0,0) | black, identical to `next_3` |

The log has **no** `missing-target`, `Syscall TODO`, `unhandled` or error line at all. From about frame
2100 on it holds one repeating line:

```
[sceDmaSend] ch=0x1000d400 madr=0x54ca00 qwc=0x400 tadr=0x54ca00 chcr=0x181
[sceDmaSend] ch=0x1000d400 madr=0x550a00 qwc=0x400 tadr=0x550a00 chcr=0x181
```

alternating between two 16 KiB buffers (0x54CA00 and 0x550A00) on DMA channel 9, whose register is at
0x1000D400 - the scratchpad's channel, not SIF2 (corrected on 2026-10-13, docs/knowledge/19 section 3: the
console's map makes 0x1000D000/0x1000D400 the two scratchpad channels; the runtime makes real transfers for
VIF0, VIF1, GIF and those two). It is the stream the CRI player feeds its decoder with.

## Classification

`RESULT: OK_VSYNC_LOOP` throughout (frames and vertical blanks keep advancing; the run ends with the
script's `quit`, exit code 0). This is not a crash and not a stopped run: the game is playing the opening
video, which the health warning precedes on the console too (`tools/ref/route.txt`: "logos and opening
video (about 100 s), then the title").

## Investigation

1. **Where the buttons stopped (fixed, allowed kind).** The first run's three shots were byte-identical.
   The game's per-frame pad poll `0x138FB0` asks its own multitap wrapper `0x255EA0` (GetConnection), a SIF
   RPC to `mtapman`. The real `mtapman` probes the SIO2 hardware at 0x1F808200, which this port does not
   model, so it answered 0, and 0 makes the poll "skip the whole update (no pad input)"
   (`docs/knowledge/14-iop-modules.md` section E, measured by decompiling the callers). The four wrappers
   (0x255CA0, 0x255DC0, 0x255E30, 0x255EA0) are now replaced in `src/game/don_overrides.cpp` with the
   values section E measured (1, 1, 1, and "return 1, `*a` = 0, `*b` = 0"), which keeps the two-port mode
   and lets the replaced `scePadRead` be the source of the pads. After that the cursor moves and the
   dialog is dismissed.
2. **What the black screen is.** The video is the opening movie from `MOVIE.AFS` (searched at boot). The
   stream above alternates two 16 KiB buffers on DMA channel 9 (0x1000D400), the scratchpad's channel - not
   SIF2, see the correction in the Symptom section - which is what the movie player feeds its decoder with. The picture stays black: the port's MPEG/IPU path is a stub (`Kernel/Stubs/MPEG.cpp`,
   `Kernel/Stubs/IPU.cpp`), so nothing is decoded into the frame buffer. The run's script quit at frame
   3450, while the reference spends about 100 s (6000 presented frames) on this video, so the run ended
   before the video's own end.
3. Ruled out: a missing recompiled function, an unimplemented syscall, an unhandled RPC, a crash and a
   stopped game thread (the log has none of those lines, and the frame/vsync counters keep advancing).

## Cause

Not determined, and not a blocker as far as it was measured: the game is in the opening video, whose
picture the port does not decode (no MPEG/IPU implementation), and the run was ended by the script before
the video finished. Whether the video *ends* and the title screen appears after it - as it does on the
console - is **not verified**: that needs the same script with shots and `quit` beyond frame 6000, or the
MPEG/IPU stubs replaced by something that decodes.

## Update (T-114, 2026-10-13 night): the video does NOT end by itself, and Start skips it

* **Without any button the black video never ends.** A run that answers the dialog and then does nothing
  (`tools/run/scripts/to_main_menu.txt`'s first three commands, then a shot every 600 frames, `quit` at
  8460) was taken to frame 8400: from frame 2400 (the video's first black frame) to 8400 - 6000 frames,
  which is the console's whole video (~105 s) - **every** shot is black (mean 0.0), and the log's only
  repeating line is the 16 KiB DMA stream on channel 9. The title never appears. This settles the open
  question of docs/knowledge/19 section 6 item 3: the CRI state machine does not reach its end-of-stream
  state by itself without decoded pictures, so the loop runs until Start.
* **One Start tap inside the video skips it.** With `tap START 6` at frame 2400 (the video is black from
  ~2100) the picture is the title screen at frame 2700 (before: black), i.e. the `0x4D6A40 & 0x800` bit of
  docs/knowledge/19 section 1 is reached and honoured. A second `tap START 6` 60 frames later shows the
  main menu at frame 3000. The console's own timeline (logo 5-25 s, video 25-105 s, title 110 s after the
  answer) maps onto the port frame for frame (frame = 330 + 60 * seconds), so the port's boot is the
  console's to the frame; only the video's picture is missing.

## Fix

For the pad path, applied: the four multitap wrappers are replaced in `src/game/don_overrides.cpp` with
the values of `docs/knowledge/14-iop-modules.md` section E (measured). Nothing was changed for the video:
that is a separate task (the MPEG/IPU path).

## Verification

* `bash tools/pipeline/build_host.sh` and the pytest of `tests/host` and `tests/input` pass (the synthetic
  guest reads the pad through the runtime's handlers and the scripted taps are checked pixel by pixel).
* The run above: exit code 0, the three dialog shots differ as described, the named shots exist.
* T-114: `tools/run/scripts/to_main_menu.txt` reaches the title (frame 2700) and the main menu (frame 3000)
  in about 4 minutes of wall clock; the run without Start reached frame 8400 (all black). Both are in
  `work/orchestrator/t114/`.
* The screenshots and the log stay under `work/` (`work/shots/memcard/`), the evidence for this entry is
  also in `work/orchestrator/t111/`.
