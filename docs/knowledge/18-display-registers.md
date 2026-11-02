# 18. Display registers with a real GS

Written 2026-10-11 (T-108 follow-up). Markers: **Measured** (seen in a dump or in the
decompiled game), **Declared** (read in the runtime source), **Unverified** (inferred, not
checked). Addresses are guest addresses of `SLPS-25675`. Dumps are in `work/gsdump/`
(game data, never versioned); only register values appear here.

## 1. Reference values (Measured)

`tools/verify/dump_regs.py` prints the 19 registers at the initial moment and at each vsync
of a GS dump (`--decode` names the fields). All six dumps (title, menu and the four others)
have 5 moments with identical values, except DISPFB1/2 and CSR.

| Register | Value | Meaning |
|---|---|---|
| PMODE | 0x8067 | EN1=1 EN2=1 CRTMD=1 MMOD=1 AMOD=1 ALP=128 |
| SMODE1 | 0x740814504 | CMOD=2 (NTSC), PRST=1, RC=4, LC=32, SPML=4, CLKSEL=1, NVCK=1, SLCK2=1, VCKSEL=1 |
| SMODE2 | 0x1 | INT=1, FFMD=0 (frame) |
| SRFSH | 0x8 | |
| SYNCH1 | 0x7F5B61F06F040 | |
| SYNCH2 | 0x33A4D8 | |
| SYNCV | 0xC7800601A01801 | |
| DISPLAY1 | 0x1BF9FF0183227C | DX=636 DY=50 MAGH=3 DW=2559 DH=447 |
| DISPLAY2 | 0x1BE9FF0183227C | same, DH=446 |
| DISPFB1 | 0x1400 / 0x148C | FBW 10, FBP 0 / 140, alternates each vsync |
| DISPFB2 | 0x80000001400 / 0x8000000148C | same, DBY=1 |
| EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, BUSDIR, SIGLBLID | 0 | |
| IMR | 0x7F00 | |
| CSR | 0x551B400C / 0x551B600C | revision byte 0x1B; FIELD (bit 13) toggles |

So the real picture is 640x448 interlaced frame mode, magnified x4 (MAGH=3) by the CRTC.

## 2. The game's own SDK code (Measured, decompiled)

The game statically links the SDK `sceGs*` code at 0x100350-0x102100. Shared state is the
"gparam" block at guest 0x3BC110 (accessor `FUN_001004E0`): u16 interlace, u16 omode
(2 NTSC, 3 PAL, 0x50 VESA), u16 ffmode (1 = FIELD, 0 = FRAME), u16 GS revision
`(CSR>>16)&0xFF`, u32 vsync callback at +8, u32 INTC handler id at +0xC.

| Function | Address | What it does to the hardware |
|---|---|---|
| sceGsResetGraph | 0x100350 | mode 0: CSR=0x200, fills gparam, reads revision, GsPutIMR(0xFF00), removes the vsync INTC handler, SetGsCrt(inter&1, omode, ffmode&1). **No GIF packet. Writes no DISPLAY/DISPFB/PMODE itself.** Mode 1: CSR=0x100. Mode 5: gparam + SetGsCrt |
| sceGsResetPath | 0x1004F0 | VIF1_FBRST=1, VIF1_ERR=2, VU0 control bit 0x200, two quadwords to the VIF1 FIFO, GIF_CTRL=1 |
| sceGsSetDefDispEnv | 0x100608 | builds 5 dwords: PMODE=0x66, SMODE2, DISPFB, DISPLAY, BGCOLOR=0 |
| sceGsPutDispEnv | 0x100980 | revision 1: PMODE, DISPFB1, DISPLAY1, EXTDATA. Otherwise (ours, 0x1B): PMODE, SMODE2, DISPFB2, DISPLAY2, BGCOLOR (circuit 2 only) |
| sceGszbufaddr | 0x100A40 | page count of the frame, `<<16` for interlace-frame else `<<17`, then `>>16` |
| sceGsSetDefDrawEnv | 0x100B08 | 8 A+D pairs: FRAME_1, ZBUF_1, XYOFFSET_1, SCISSOR_1, PRMODECONT(\|=1), COLCLAMP(\|=1), DTHE (1 only if psm&2), TEST_1. **Does not touch ALPHA_1 or SCISSOR_2** |
| sceGsSetDefClear | 0x100CF0 | clear packet: TEST, PRIM=6, RGBAQ, XYZ2 x2, TEST |
| sceGsPutDrawEnv | 0x100DF8 | polls DMAC2 CHCR bit 8 (timeout 0x1000000), then QWC, MADR, CHCR=0x101 |
| sceGsSetDefDBuff | 0x100EE0 | 2 disp envs (+0, +0x28), 2 draw envs (+0x60, +0x150), optional clear packets, giftags at +0x50 and +0x140 (NLOOP 8, or 0xE with clear) |
| sceGsSwapDBuff | 0x101178 | PutDispEnv(env + (which&1)*0x28), then PutDrawEnv |
| sceGsSyncV | 0x1011D8 | clears INTC_STAT bit 2 then spins on it **with no timeout**; with a callback it also uses SetVSyncFlag. Interlaced: returns CSR bit 13 |
| sceGsSyncPath | 0x101270 | polls DMAC1/DMAC2 CHCR, VIF1_STAT, VU0 status, GIF_STAT, each with timeout 0x1000000 (returns -1) |
| sceGsSyncVCallback | 0x101DB8 | AddIntcHandler(2, cb) / RemoveIntcHandler, gparam +8/+0xC |
| sceGsExecStoreImage | 0x101728 | CSR=2, BUSDIR=1, VIF1 DMA and FIFO readback |

SetDefDispEnv formulas (Measured): `smode2 = interlace ? (ffmode ? 3 : 1) : 2`;
`dispfb = (psm&0xF)<<15 | (((w+63)>>6)&0x3F)<<9`; `magh = (w+0x9FF)/w`;
`DISPLAY = (magh-1)<<23 | (magh*w-1)<<32 | ((dx*magh+x0)&0xFFF) | DH<<44 | (DY&0xFFF)<<12`.

| Case | x0 | DY | DH |
|---|---|---|---|
| NTSC (omode 2), progressive | 0x27C | dy+0x19 | h-1 |
| NTSC, interlace | 0x27C | dy+0x32 | frame h-1, field 2h-1 |
| PAL (omode 3), progressive | 0x290 | dy+0x24 | h-1 |
| PAL, interlace | 0x290 | dy+0x48 | as NTSC |

For omode 2 and 3 the user offsets are 0. Other modes read a file (`FUN_00100558`) and call
syscall 0x80; not relevant to NTSC. With w=640, h=448, interlace, frame, omode 2 this gives
DX=636, DY=50, MAGH=3, DW=2559, DH=447, which matches DISPLAY1 in section 1 exactly.

Syscalls used (Measured): SetGsCrt = 2, GsPutIMR = 0x71 (dispatcher also maps 0x70 to
GsGetIMR), SetVSyncFlag, AddIntcHandler/RemoveIntcHandler, DIntr/EIntr wrappers
(0x116360, 0x1163B0).

## 3. What the runtime does today (Declared)

Paths relative to `PS2Recomp/ps2xRuntime/src/lib/`.

- `sceGsResetGraph` (`Kernel/Stubs/GS.cpp:786-855`, mode 0): fills `g_gparam`, writes it to
  scratchpad, then **sends a stand-in A+D packet through the GIF** (PMODE, SMODE2, DISPFB,
  DISPLAY, BGCOLOR and more). It writes no SMODE1 and does not call SetGsCrt.
- `GsSetCrt` (`Kernel/Syscalls/System.cpp:6-35`): writes only SMODE2 = interlace | ffmode<<1
  and forces PMODE EN1. Never SMODE1, SRFSH, SYNCH1/2 or SYNCV. Its comment on a1 ("0=NTSC")
  is wrong: the game passes 2 for NTSC.
- `sceGsSetDefDispEnv` (GS.cpp:999-1021), `SetDefDBuff` (940-998), `SetDefDBuffDc`
  (869-939), `PutDispEnv/PutDrawEnv` (760-785), `SwapDBuff*` (1131-1213), all via helpers in
  `Kernel/Stubs/Helpers/Support.h` (`makePmode` 1597, `makeDisplay` 1617, `applyGsDispEnv`
  1877, `seedGsDrawEnv1/2` 1901).
- `GsGetIMR/GsPutIMR` (System.cpp:40-80) read and write `memory().gs().imr`.
- Privileged GS registers (`ps2_memory.cpp` ~960-1035) are plain stores except CSR
  (write-one-to-clear; bit 0x200 calls `notifyGsReset`, host GS only).
- INTC_STAT 0x1000F000: no handling; a plain stored register (only 0x1000F200-0x1000F260 is
  special-cased, ~2387). DMA CHCR reads return the value with bit 8 cleared (~2372), so DMA
  poll loops end at once.
- `sceGsSyncV` is replaced and waits through `WaitVSyncTick`; `sceGsSyncPath` is replaced and
  reads the same registers with the same timeouts.

## 4. Differences handlers vs. the game's SDK code

| Item | Game SDK (Measured) | Runtime handler (Declared) |
|---|---|---|
| ResetGraph display writes | none; only CSR, IMR, SetGsCrt | stand-in packet through the GIF; its entries 0x41/0x42 land on SCISSOR_2 and ALPHA_1 |
| SMODE1, SRFSH, SYNCH1/2, SYNCV | written by the SetGsCrt syscall (BIOS) | never written |
| PMODE | 0x66 | `makePmode(1,1,0,0,0,0x80)` = 0x8007 style, no MMOD/AMOD |
| SMODE2 | `interlace ? (ffmode ? 3 : 1) : 2` | `interlace \| ffmode<<1` (progressive differs) |
| DISPLAY | MAGH from width, DX/DY from mode tables | `makeDisplay(636, 32, 0, 0, w-1, h-1)`: MAGH 0, DY 32 |
| Display circuits | circuit 2 only (revision != 1) | both circuits always |
| DTHE | 1 for 16-bit psm | always 0 |
| DBuff giftag NLOOP | 8, or 0xE with clear | always 14; the `clear` argument is ignored |
| DBuffDc second buffer | interlace-frame keeps both at FBP 0 | buffer 1 at FBP = zbufaddr |
| gparam | in guest memory at 0x3BC110 | in a runtime struct plus a scratchpad copy |

The memory layout of the DBuff structures matches the SDK offsets. Combined with the T-110
finding (docs/knowledge/13), the practical effect: with a real GS the picture is 4x too
narrow because SMODE1 is never set and DISPLAY has MAGH 0, and the stand-in packet corrupts
SCISSOR_2 and ALPHA_1.

Unverified: PMODE in the dumps is 0x8067, not the SDK's 0x66, and both DISPFB circuits
alternate in lockstep. Either the game writes PMODE/DISPFB itself outside the SDK calls, or
PCSX2 or system software sets them. The same applies to the FBP 0/140 alternation, which the
SDK rule for interlace-frame would not produce.

## 5. Routes

- **(a) Fix the handlers.** Keep `sceGs*` replaced and make each one write what the game's
  code writes (section 2). Small, local, testable against the reference table.
- **(b) Recompile the SDK functions** (remove them from `config/stub_policy.toml`). The game
  then runs its own code, with the true gparam block. Risks: `sceGsSyncV` spins on INTC_STAT
  bit 2, which the runtime never sets (a write of 4 reads back as 4, so the loop would not
  block and would lose frame pacing); ResetGraph/ResetPath touch VIF1, VU0 and GIF control
  registers and need interrupt syscalls; ExecStoreImage needs BUSDIR and VIF1 readback.

## 6. Recommendation (a proposal; the orchestrator decides)

A mixed route, staged:

1. **Stage 1, small runtime patch.**
   - In host-GS mode (`runtime->hostGs()`), `sceGsResetGraph` sends no stand-in packet. In the
     built-in GPU mode the packet stays, because that renderer relies on it.
   - `GsSetCrt` with mode 2 writes SMODE1, SRFSH, SYNCH1, SYNCH2 and SYNCV with the NTSC
     values of section 1 (PAL values are not measured).
   - `SetDefDispEnv` and the display part of `SetDefDBuff/Dc` use one builder that follows the
     SDK formulas of section 2.
2. **Stage 2, optional, after the real game runs.** Recompile the pure builders and
   Put/Swap functions (SetDef*, Put*Env, zbufaddr, SwapDBuff*). Keep replaced: ResetGraph (it
   would also have to write the SDK gparam block at 0x3BC110), ResetPath, SyncV,
   SyncVCallback, SyncPath, ExecStoreImage.

Whole-set route (b) is not recommended: it depends on INTC and VIF1/VU0 behaviour the runtime
does not model. The host-side `filter_reset_graph` workaround in `src/host/gs_glue.cpp`
assumes a transfer starts at a GIF tag, which is not guaranteed, so it should be dropped once
stage 1 exists.

**Decision (orchestrator, 2026-10-11).** Stage 1 is accepted and is being written as patch
`0021` with the synthetic guest of `tests/host` as its test; `filter_reset_graph` goes away with
it. Stage 2 is decided when the real game draws through the host program, by comparing its
registers with section 1. The whole-set route (b) is discarded for the reasons above.

**Done on 2026-10-12 (patch `0021-host-gs-display-registers`).** Measured with the real game at
its first screen (the memory-card dialog), port against the reference dump of the same screen
(`work/ref/pcsx2/00-memcard.gs.zst`, registers with `tools/verify/dump_regs.py`, the port's with
`DON_TRACE_GS_REGS=1`):

| Register | Port | Console |
|---|---|---|
| PMODE, SMODE1, SMODE2, SRFSH, SYNCH1, SYNCH2, SYNCV, DISPLAY2, BGCOLOR, EXTBUF, EXTDATA, EXTWRITE, BUSDIR, SIGLBLID | equal | `0x8067`, `0x740814504`, `0x1`, `0x8`, `0x7F5B61F06F040`, `0x33A4D8`, `0xC7800601A01801`, `0x1BE9FF0183227C`, 0... |
| DISPFB1 / DISPFB2 | `0x1400` / `0x80000001400`, fixed | alternate `0x1400`/`0x148C` every vertical blank |
| DISPLAY1 | `0x1BF27F00000000` (an early value of the runtime, never rewritten) | `0x1BF9FF0183227C` |
| CSR | `0x200` / `0x2200` | `0x551B400C` / `0x551B600C` (revision `0x1B`, no RESET bit left set) |
| IMR | `0xFF00` (what the SDK writes) | `0x7F00` |

PMODE `0x8067`, DISPFB2 with DBY = 1 and DISPLAY2 with DH 446 are written by the engine itself
(the handlers write `0x66`, `0x1400`, DH 447), which closes that item of section 8. The picture:
640 by 448, and identical to the replay of the reference dump in 354 of 448 rows, with both
renderers; the rows that differ are four lines of the dialog's text, which says "no data for
this game" in the port and "not formatted" in the reference, that is, the state of the memory
card, not graphics. The differences left in the table (the frame-buffer alternation, DISPLAY1,
the revision byte of CSR, IMR) do not change this picture; they are open and may matter later
(interlaced field rendering, code that reads the GS revision). Still the runtime's own: the mode
block (`gparam`) is kept in a runtime structure, not in the game's block at `0x3BC110`.

## 7. Verification plan

1. `tools/verify/dump_regs.py` on the reference dumps (done, section 1).
2. Synthetic guest that sets the display only through the handlers; after stage 1 its
   register state must equal the section 1 table (NTSC, 640x448, interlace, frame) and the
   scissor- and alpha-sensitive scene must stay correct without the filter.
3. On the real game: dump registers through the host GS at the title screen and compare with
   section 1.

## 8. Not verified

- Who sets PMODE 0x8067 and the FBP 0/140 alternation (section 4).
- PAL register values; any mode other than NTSC omode 2.
- That a recompiled `sceGsSyncV` would return at once on our INTC_STAT (inferred from the
  plain-store behaviour, not run).
- Behaviour of `sceGsExecStoreImage` and `sceGsSyncVCallback` under the runtime.

## 9. The comparison at the title and the menu (journal 008, T-114; added 2026-10-13)

**Measured** with the route `tools/run/scripts/to_main_menu.txt` (title at blank 2700, menu at 3000),
`DON_TRACE_GS_REGS=1`, against `tools/verify/dump_regs.py work/gsdump/02-menu.gs.zst --decode`:

| Register | Console (reference dump) | Port at the title and the menu |
|---|---|---|
| PMODE | `0x8067` | equal |
| SMODE1 | `0x740814504` | equal |
| SMODE2 | `0x1` | equal |
| SRFSH | `0x8` | equal |
| SYNCH1 / SYNCH2 / SYNCV | `0x7F5B61F06F040` / `0x33A4D8` / `0xC7800601A01801` | equal |
| DISPLAY1 / DISPLAY2 | `0x1BF9FF0183227C` / `0x1BE9FF0183227C` | equal |
| EXTBUF, EXTDATA, EXTWRITE, BGCOLOR, BUSDIR, SIGLBLID | 0 | equal |
| DISPFB1 / DISPFB2 | `0x1400` / `0x80000001400` | `0x148C` / `0x8000000148C`: the same alternating buffers in the other field, as at the memory-card dialog; the title's blank 2700 has `0x1400` |
| CSR | `0x551B600C` (with the revision byte) | `0x2200` (FIELD + RESET) |
| IMR | `0x7F00` | `0xFF00` |

So the display set-up the section 6 route produces is the console's at the title and the menu; the two
differences (CSR, IMR) were already known from the memory-card dialog. The wrong pictures of those
screens (title SSIM 0.534, menu SSIM 0.080 against the console's replays) are **not** a display-register
defect: journal 008 traced the menu to the runtime's VU1 interpreter stopping at `pc=0x168` and the
background to the game's own vector data; both are in [04](04-graphics.md), "The path of a 3D object and
the three defects that broke it".
