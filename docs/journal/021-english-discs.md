# 021 · The English discs

- **Date:** 2026-10-13
- **Task:** T-320
- **Status:** open

## Symptom
Run the English variants headless: `build/linux-3d/don --headless --renderer software` with the committed menu and match scripts
(copies under work/english/). All runs exit 0 and classify as OK_VSYNC_LOOP. The main menu is drawn garbled in both English variants.

## Classification
OK_VSYNC_LOOP (3060-3240 menu, 8220-8580 match). The garbled menu is a visual defect, no catalog row.

## Investigation
- verify_disc.py recognises en-jpvoice, en-envoice, jp. 24 compared files are SHA-1 identical to the Japanese ones; the link folders are faithful.
- No .iso is opened in the routes; a run with no image is byte-identical to one with it.
- GIF capture of the menu frame: same vertex count (8,545); English has 66 host-to-local uploads (7 strips of 512x64 PSMCT32), Japanese 0; texture base 0x330000 vs 0x2da000.
- The title-to-menu transition is ~60 blanks later in English.

## Cause
Not determined (hypotheses: per-frame texture re-upload, a menu monitor video, a GS/VRAM layout issue).

## Fix
None for the defect. Added tools/disc/make_variant_dir.py (+ tests) and docs/RUNNING.md.

## Verification
`uv run --no-project --with pytest python -I -m pytest tests/test_make_variant_dir.py -q` and the runs above.

## Temporary diagnostics
None left in the repository; work/english/ holds ignored helper scripts.

## Follow-up: the launcher (T-409)
- `tools/launcher/launch.py` starts `<port> <variant folder>/SLPS_256.75` with no `--iso` and removes `DON_ISO` from the port's
  environment. Reason: `host.cpp` and `don_overrides.cpp` turn `DON_ISO` into the CD image path, and `iop_cdvd.cpp` reads the image
  first at LSNs derived from the folder, so the Japanese image named by `DON_ISO` could poison an English run. With no image the
  runs were byte-identical (this entry). Tested: English starts through the launcher with `DON_ISO` set to the Japanese image open
  on the English dialog (work/launcher/shots-*, headless, `build/linux-3d/don`, `DON_VU1=microvu`; `build/linux-dev` was being rebuilt).
- `make_variant_dir.py` is imported as a module (exceptions come back, one process, replaceable in tests), by path because `-I` keeps
  its folder off `sys.path`. It was not changed.

## Follow-up 2026-10-13: the English main menu on the build with patch 0039 (T-320)

**Still garbled.** `build/linux-dev/don` (16:09, with 0039), `DON_VU1=microvu`, headless, software renderer, no `--iso` and no
`DON_ISO`, the folders the launcher made (`work/launcher/variants/{en-jpvoice,jp}`), `work/english/en_menu_a.txt` (run script
`work/english/run2.sh`). Shots `work/shots/dev-en-menu`, `work/shots/dev-jp-menu`. The English `frame_003120.png` is **byte-identical**
to the earlier garbled one on the side build (`work/shots/en-jp-a/frame_003120.png`, `cmp`): panels and 3D scene are noise and white
blocks, the menu text (OPTIONS, SURVIVAL CHALLENGE, PRACTICE, SCROLL/SELECT/BACK) is legible. The Japanese menu on the same build is right
(Goku/Frieza on the monitor, all panels). So 0039 changed nothing here.

**Step 2, one measurement** (`work/english/gsmap.py`, `blocks.py`, captures `work/english/dev-{en,jp}-*.bin`, maps `map-*.txt`;
registers read from the capture: FRAME, ZBUF, TEX0 with its CBP, BITBLTBUF/TRXPOS/TRXREG/TRXDIR; sizes as linear byte ranges, pages
of 8 KiB used for the page map):

| Item (frame 3120) | English | Japanese |
|---|---|---|
| frame buffer (FBP 0, FBW 10, CT32) | 0 - 1120K | same |
| depth buffer (ZBP 280, Z16S) | 2240K - 2800K | same |
| host->local uploads in the frame | 66, one PATH3 block of 90,230 quadwords (1,443,680 B), before the first textured draw | 0 |
| where the uploads go | 2800K - 4072K (and 2800K again at upload #38, #57: wraps back) | - |
| textures the frame draws with | 37 (UI atlas T8 128x128 with 2344 of 5608 vertices; 7 video strips CT32 512x64 at 128K each) | 23 (strips are T8 512x64, 32K each) |
| texture base of the atlas | 0x3c2000 here, 0x342000 at blank 3060, 0x2cc000 at 3000 | 0x2da000 |

* No upload overlaps a frame buffer or the depth buffer, none runs past 4 MiB (highest end 4072K); `FRAME_1/2` are both at FBP 0.
* The space between the end of the depth buffer (2800K = 2,867,200 B) and 4 MiB is 1,327,104 B; the uploads carry 1,443,680 B, so they
  do not fit: the destination pointer walks up to 4 MiB and wraps to 2800K (uploads #38 and #57 both start at 0x2bc000).
  The first upload's base moves from frame to frame (0x2cc000, 0x342000, 0x3c2000 at blanks 3000, 3060, 3120) while the layout inside
  the set stays the same: a ring that advances by the set's size on every frame.
* Inside that one block, later uploads overwrite the earlier ones **before any draw**: 30 (earlier, later) pairs with overlapping
  ranges (3120). Of the 37 textures drawn, 12 (3120; 15 at 3060, 22 at 3000) sit where a later, different upload landed after their own
  upload (or its CLUT): 3405 of 5608 textured vertices at 3120 (4291/5600 at 3060, 5340/5610 at 3000). The biggest is the UI atlas
  (0x3c2000, T8 128x128, 2344 vertices): upload #53 (CT16 256x256 at 0x3bc000, 128K) covers it. The overwritten ones are the panels,
  the 3D scene's atlas and the first two video strips (#57-#59 overlap strip 0, #60-#65 strip 1). The textures that are intact
  (0x3e8000... up to 0x3fa000, the text textures) are exactly the legible part: the English text.
* The garbled part of the picture is the part drawn with the overwritten textures; the legible text is drawn with the intact ones.
* Japanese: no uploads in the frame; the textures are resident at 0x2da000-0x3e4000+. **Not answered:** whether Japanese uploads
  them at another moment. Captures of the Japanese run at blanks 2790, 2820, 2880, 2940, 2941, 3000, 3001, 3030, 3060, 3061, 3090,
  3121, 3180 are empty (header only: the capture counter, `DON_GS_PACKETS_FRAME`, did not hit a drawn frame); only 3120 gave data.

**Explanations left standing** (not distinguished by this measurement):
1. *Order in the port.* The game uploads set A, draws, uploads set B over the same addresses, draws (the overlap is then harmless),
   and the port hands the GS all the uploads as one PATH3 block ahead of the draws (the capture shows one 1.4 MB block, then the
   PATH1 draws), so set B has already overwritten set A when the draws run. Against it: the block is one DMA's worth.
2. *The game's own allocation.* In the English data the menu's set is bigger (the 7 video strips are CT32, 896K, against T8, 224K,
   in Japanese) than the 1296K the game's texture ring has, so the game re-uploads every frame and overruns its own space; on hardware
   the same data would be drawn wrong too, or the English assets are not what the game expects (a size or format field read from the
   English `DATA.AFS` entry).
A capture of the same frame from the reference emulator (`tools/ref/run_pcsx2.sh`, GS dump) would discriminate: if PCSX2 has the
uploads interleaved with the draws, it is explanation 1. Nothing was fixed.

## Follow-up 2026-10-13 (2): what the console does with the uploads, measured (T-320)

**The reference dump.** `bash tools/ref/pcsx2_auto.sh start` with `DON_ISO=en_patch/jpvoice/disc.iso` and `DON_AUTO_RAW=work/english/ref/raw`, then
`drive.py --raw work/english/ref/raw --out work/english/ref route work/english/ref/route_en_menu.txt` (own route: wait 60 s, LEFT, CIRCLE,
`wait_screen title`, START, `wait_screen menu`, `capture 02-menu nocheck`; the stored Japanese screens matched the English ones, distances 3-10
against the limit 12). It took 188 s; the screenshot `work/english/ref/02-menu.png` shows the English menu **correct** (BATTLE STADIUM MODE!,
SURVIVAL CHALLENGE, PRACTICE, SCROLL/SELECT/BACK, Goku and Frieza on the monitor). The emulator was stopped (`pcsx2_auto.sh stop`); `pgrep`
shows no PCSX2 or Xvfb left. The dump has 4 vsync intervals; the first upload of each is at 0x3c2000, 0x300000, 0x382000, 0x2cc000 (the ring moves).

**Event list** (`work/english/refseq.py`: the GS's registers in arrival order; U = host-to-local transfer, D = run of textured kicks; a dump does
not say the path, `work/english/refcmp.py` compares two lists). Excerpt of the dump's interval 1, with the port's capture of blank 3120 next to it:

```
ref (console order)                                            port (capture order)
 D 46 kicks untextured                                          D 46 untextured
 U00 0x3c2000 CT32 64x64        (atlas)                         U00 0x3c2000 ... U01 ... U02 ... U03 (all 66 uploads
 U01 0x3c2000 CT32 16x16 at (0,64)                                  one after the other)
 U02 0x3c8000 CT16 32x32  U03 0x3c9c00 CT32 8x3 (its CLUT)      D ... (the 5608 textured vertices, afterwards)
 D 1007 kicks tex 0x3c2000 T8 128x128  (atlas)
 D  208 kicks tex 0x3c8000 T4 64x64
 D  144 kicks tex 0x3c2000
 U04 0x3ca000 ... U05 CLUT
 D   31 kicks tex 0x3ca000 ...
 ...
 U39 0x2c3c00  U40 0x2c4000 CT32 512x64   D 48 kicks tex 0x2bc000
 U41 0x2e4000 CT32 512x64                  D  6 kicks tex 0x2c4000 (video strip 0)
```
* Same uploads: both frames have **66** uploads, and with the port's first-upload base equal to the dump's interval 1 (0x3c2000) the two
  lists of uploads are **identical** (same address, format, size and position, same order; `cmp` of the 66 lines). The ring wraps at the same
  upload (#38 goes to 0x2bc000 in the dump too).
* Interleaving on the console: between two groups of draws there are 1 upload (12 times), 2 (17), 3 (4) or 4 (2): a texture's image and its CLUT,
  sometimes a second texture. The big strips come one at a time. The port puts all 66 before the first textured draw.
* On the console, **no draw uses a texture or CLUT after a different upload overlapped it** (0 of 5599 textured vertices); the port's frame has
  3405 of 5608 (the same counter, `refcmp.py`). The atlas (0x3c2000) is last drawn many events before upload #53 lands on it: the game overwrites
  texture memory only when the texture is dead. This confirms explanation 1 of the first follow-up and drops explanation 2: the English set fits
  the ring on the console, the game's allocation is fine.
* One upload as GIF tags (the port's capture, `tagdump.py`): A+D nloop=1 [BITBLTBUF]; A+D nloop=3 [TRXPOS TRXREG TRXDIR]; IMAGE nloop=1024
  (16 KiB); A+D nloop=3 [TRXPOS TRXREG TRXDIR]; IMAGE nloop=64; A+D nloop=1 EOP=1 [register 0x7f, NOP]; sometimes a padding A+D nloop=64 [0x7f..]
  EOP=0. EOP is set only on the closing NOP tag: the block has **37 EOP-terminated GIF packets**, 29 with two uploads (an image and its CLUT, or
  U00+U01) and 8 with one (the large strips). The dump's transfers are cut elsewhere (65 contain uploads, 64 of them one upload) and its
  transfers do not carry EOP at their ends, so EOP counts cannot be compared with it.

**What the game asks for, read from the port's memory** (`DON_TRACE_STATE=60 DON_STATE_DUMP=... DON_STATE_DUMP_AT=3060`; no VIF1 trace exists, none was
added; `work/english/{chain,adscan,vifscan2}.py`; the console's RAM from `02-menu.p2s`, `unp2s.py`):
* The same buffer holds the upload packets in both memories (0x69a480...): a DMA chain of `cnt qwc=2 [NOP, DIRECT 2]` (A+D tag + BITBLTBUF),
  `ref qwc=0x1005 [NOP, DIRECT 0x1005]` (TRXPOS/TRXREG/TRXDIR, IMAGE tag, the pixels), `cnt qwc=2 [NOP, DIRECT 2]`, ..., `call 0x5a9af0`, `next` back to
  the head. Two small shared sub-chains: at 0x5a9900 `MSKPATH3(0x8000), FLUSH, FLUSHA, FLUSH, FLUSHA` and at 0x5a9770 `FLUSH, NOP, NOP, MSKPATH3(0)`.
  Elsewhere: `BASE OFFSET ITOP MSKPATH3(0) NOP NOP NOP` (0x3bc140), `NOP NOP MSKPATH3(0) NOP...` (0xad9cb0).
* **Not resolved:** these packets are laid out as VIF1 chains (DIRECT in the tag's code half), yet the port's capture carries the uploads on path 3
  (the runtime submits path 3 only from the GIF channel's transfers, `ps2_memory.cpp` 1754-1802). Either the game starts the same chain on the
  GIF channel (the code half is then ignored) or only part of the uploads travel there. I could not see which VIF1 codes enclose the release, nor
  where the game starts channel 2, without a VIF1/DMA trace; the order "mask off, what follows, mask on" is therefore **not measured**.

**What PCSX2 does** (`third_party/pcsx2/pcsx2/`):
* `Vif_Codes.cpp:513` MSKPATH3 sets `gifRegs.stat.M3P = (vif1Regs.code >> 15) & 0x1;` and, when it becomes 0, only calls `gifInterrupt()` (line 520): the mask
  gates the **start of a packet**, it does not hold a queue.
* `Gif_Unit.h:881` `Path3Masked() { return ((stat.M3R || stat.M3P) && (gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)); }`
  and `Gif_Unit.h:807` `else if (!gsSIGNAL.queued && !gifPath[2].isDone() && !Path3Masked())` is the only way path 3 is chosen. A packet that has begun
  (state IMAGE/PACKED...) finishes even if the mask goes on; the unit re-arbitrates at every end of a GS packet (`ExecuteGSPacket` returns done at EOP),
  with path 1 before path 2 before path 3 (lines 755-815), and an IMAGE may be sliced to let path 1/2 in when `IMT` is set (line 761).
* `Vif_Codes.cpp:230` FLUSHA: `gifBusy = gifUnit.checkPaths(1, 1, 1) || (gifRegs.stat.APATH != 0)`: the VIF1 stream stalls (`VGW`) until **no path has
  data and no path is active**, i.e. everything already handed to the GS (path 3 included, if it was allowed to run) has left. FLUSH (line 199) does the
  same for paths 1 and 2 only.

**Specification for the runtime** (to implement; not done):
1. The queue of masked path-3 data is a queue of whole GIF packets (cut at EOP=1). Lifting the mask (MSKPATH3 with bit 15 clear) must let the packets run
   **one after the other, in order, interleaved with the VIF1 stream**, not hand the whole queue over at once: after the mask is lifted, the VIF1 stream must
   not execute its next *draw-producing* code (DIRECT, XGKICK/MSCAL start) before the packet(s) the game released have reached the GS; while the mask is on, a
   packet that has started is finished, a packet not started waits.
2. FLUSHA must wait until the path-3 packet in progress and all the path-3 data already allowed (mask off) have been delivered to the GS, and path 1 and 2 are empty. With
   the mask on, FLUSHA does not wait for queued, unstarted path-3 packets (PCSX2: `Path3Masked()` stops them).
3. Data queued when a frame ends with the mask on stays queued across the frame boundary and goes out when the mask is lifted (PCSX2 holds it in the DMA channel
   and the GIF FIFO); the port's frame boundary must not flush it nor drop it. (`FBRST` reset at `ps2_memory.cpp:1295` does flush: that follows `Gif_Unit` reset only if the
   console's reset clears M3P, which I have not checked.)
4. The unit of release is not measured. Candidates, from the capture: one EOP-terminated packet (37 per frame; one texture and its CLUT) or one DMA tag. The
   console's frame lists 1-4 uploads between draw groups, which fits a packet of up to two uploads released per MSKPATH3 off, but I did not see the codes.

**The test.** The port's capture of the English menu at a blank must have, in `refseq.py` terms, the same list of events as the console's dump for the matching
interval: same uploads in the same order (compare the `U` lines: `cmp`), and the count of textured vertices drawn after a *different* upload overlapped their texture
or CLUT (`refcmp.py`, last line) must be **0**. Stronger: the sequence of event kinds (U/D) must have no run of more than 4 uploads between draws. Today: port
3405 of 5608, console 0. (The tools are scratch under `work/english/`; adding them to `tools/verify/` would be about 40 lines plus a test: not done.)

Not verified: the VIF1 codes between uploads (no trace); on which channel the game starts these chains; the console's behaviour for a mask lifted with a packet half
queued; M3R (GIF_MODE) is not touched by the game as far as I looked (not searched). Nothing was changed in the runtime, nothing was built.

## 2026-10-13 follow-up (3): patch 0042, the third path released one packet per lifting of the mask

**Defect.** `flushMaskedPath3Packets` (`ps2_memory.cpp`) handed the GS every entry of `m_path3MaskedFifo` when the VIF1 stream lifted the mask. An entry is a whole
GIF-channel transfer (here the game's single block with all the uploads), so the 66 uploads reached the GS before the first draw and overwrote each other.

**Patch** `patches/ps2recomp/0042-path3-released-by-packet.patch`, only `ps2_vif1_interpreter.cpp` (no header, so a short rebuild):
- MSKPATH3 on to off releases ONE packet from the front of the queue (cut at the tag with EOP set by walking the GIF tags: PACKED, REGLIST, IMAGE) and drains
  the arbiter before the VIF1 stream goes on; the rest of that transfer stays at the front.
- Queue drained completely: FLUSHA with the mask off; the end of a VIF1 transfer (`processVIF1Data`) with the mask off. Data arriving with the mask off passes
  straight through (unchanged); the FBRST reset still flushes everything (unchanged). Data left with the mask on stays queued.
- Decided: a packet with no closing EOP tag, or truncated, is released whole (what the runtime did for everything before).

**Test** `tests/runtime/path3_release_test.cpp` (host GS recording the path of each transfer; GIF channel sends three EOP packets in one transfer, the second
made of two tags; then three times lift/mask/FLUSHA/DIRECT). Patched: `p3:1 p2:11 p3:2 p2:12 p3:3 p2:13`. Unpatched: `p3:3 p2:11 p2:12 p2:13` (the recorder keeps the
last register value of each transfer, so the unpatched block of three shows as one transfer, `p3:3`). Also checked: the mask left off drains the rest, a short queue,
mask off passes through. 4 tests, 3 fail unpatched, all pass patched.

**The game**, English variant (`en-jpvoice`), headless software renderer, capture at blank 3120:
- The menu (`work/shots/p42-en/menu_3125.png`) is drawn right: "BATTLE STADIUM MODE!", "SURVIVAL CHALLENGE", "PRACTICE", the lettering and the background intact.
- `refcmp.py` against the console's interval 1: 66 uploads, same relative addresses/formats/sizes/positions in the same order (first upload at 0x3c2000, as on the console);
  textured vertices drawn after a different upload overlapped their texture or CLUT: **0 of 5608** (before: 3405 of 5608; console: 0 of 5599). The per-texture vertex totals are
  not identical (5608 against 5599, nine more vertices; not investigated). Runs of uploads between draws: console {1: 12, 2: 17, 3: 4, 4: 2}; port {1: 8, 2: 19, 4: 5} (its
  event list cuts draw groups differently, not compared further).
- Japanese variant, `to_match_1v1.txt`, patched against the same run on the unpatched tree (same binary otherwise): the 12 named shots (memcard prompt, main menu, character
  select, practice select, menu again, stage select, match load 1 and 2, intro, 1v1, 2, 3) are byte-identical; of the 130 periodic shots one differs, `frame_007080.png`
  (in the match: 17,089 pixels, in the bottom 184 rows, max channel sum difference 286; the fight itself looks the same). Not investigated whether it is the patch or a
  one-frame timing difference of the run.

Not verified: the VIF1 codes themselves (still no trace); Windows; routes other than these.
