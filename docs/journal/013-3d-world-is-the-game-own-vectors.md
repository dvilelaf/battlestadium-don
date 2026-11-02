# 013 · The 3D world is not drawn: the port's own vector data is wrong before the VU1

- **Date:** 2026-10-13
- **Task:** T-202 (step 5a of the card: the main processor's mathematics)
- **Status:** open

## Symptom

Every 2D element of every screen is right; the 3D scene (the stadium behind the title, the menu and the
character select; the stage and the fighters in the match) comes out as flat magenta/violet triangles,
with the same result on the runtime's VU1 interpreter, PCSX2's interpreter and microVU (journal 008 and
its T-202 follow-up). This entry goes down the path a 3D object takes and names the **first stage where
the port differs from the console**: the game's own vector data, written by the recompiled code *before*
VIF1, at the title screen.

Tools and artifacts (all under `work/orchestrator/t3d/`, the console and capture files under `work/`):

| what | where |
|---|---|
| the GIF-packet analyser (step 1) and its 10 synthetic tests | `tools/verify/gif_stats.py`, `tests/test_gif_stats.py` |
| one frame of the port's own GIF transfers (step 4a) | `src/host/gs_trace.{h,cpp}` + the hook in `gs_glue.cpp`, `DON_GS_PACKETS=... DON_GS_PACKETS_FRAME=2700` |
| the port's VU1 memories at the title (step 4b) | `work/orchestrator/t3d/dump_vu1.sh` (gdb, `don_vu1_data/micro`), `port-vu1-*.bin`, `port-interp-vu1-*.bin` |
| matrices in both RAM images (step 2) | `matrix_scan.py` |
| per-texture comparison of both frames | `compare_frames.py` |
| the console's frames as text/JSON | `console-title.txt`, `console-match.txt`, `console-memcard.txt` |

## 1. What the console asks the GS to draw (tools/verify/gif_stats.py)

Careful: **a PCSX2 dump labels every transfer with index 3** (Path1New), so it cannot say which path a
packet came through (`docs/knowledge/15-gs-dumps.md`); only the port's own capture has the real path.

| frame | tags | vertices | primitives (vertices) | textures (vertices) | positions |
|---|---|---|---|---|---|
| 00-memcard | 507 | 934 | fan 894, point 40 | 0x2c4000 576, 0x2be000 240, … | x=[0,640] y=[0,448] px (299/299 tags inside) |
| 01-title | 648 | 8437 | strip 8199, fan 198, point 40 | 0x2bc000 3301, 0x2c2000 1374, 0x2ea000 736, … | x=[-454,1095] y=[-289,740] px |
| 04-match-1v1 | 1054 | 14925 | strip 14453, triangle 192, fan 240, point 40 | 0x2d6000 4845, 0x2d0000 3134, 0x2f0000 2459, … | x=[-2043,2047] y=[-2037,1968] px |

So the console's 3D scene is the **triangle strips with the textures at the top of VRAM** (TBP0
0x2bc000-0x314000), the 2D art is the triangle fans (the memcard dialog is *all* fans, inside the
screen), and the 40 points are a fixed piece of every frame's setup. The XYOFFSET the dumps write
before the frame is (27648, 29184) (0x6c00, 0x7200) and the vertex field layout is X at bytes 0-1, Y at
bytes 4-5, Z = (u32 at byte 8) >> 4 & 0xffffff for XYZF2 (PCSX2's `GIFPackedXYZF2`); with it the dialog's
vertices fall exactly inside 640x448, which is how the layout was calibrated.

## 2. What the port asks the GS to draw (one frame, path by path)

`DON_GS_PACKETS=port-title.bin DON_GS_PACKETS_FRAME=2700 DON_VU1=microvu build/linux-dev/don …
--input-script tools/run/scripts/to_main_menu.txt` (2m11s, exit 0) gives 568 transfers / 510992 bytes:

```
tags=634 vertices=10064
  path 1: tags=369 vertices=10024      <- the runtime's gif_transfer path 1 = the VU1's XGKICK
  path 2: tags=265 vertices=40         <- VIF1 direct
  vertices per primitive: triangle_strip=9826, triangle_fan=198, point=40
  textured vertices per TBP0: 0x2bc000=3435, 0x2c2000=2213, 0x2e4000=954, ...
  positions of every vertex: x=[-2046.9,2042.9] y=[-2033.4,2000.6] px
```

**The 3D geometry is there** — the same textures, the same primitive classes, the same tag structure
(REGS 02,01,04), the same nloop per tag as the console — and it arrives **through path 1, produced by the
VU1**, exactly as it should. What is wrong is where it lands: the console's stadium tag covers a 22x17
pixel patch, the port's the whole ±2048 pixel area, and its Z is *saturated* to 0xffffdf where the
console's varies (0x1eb…0x1f9). ±2048 px and a saturated Z is what the GS shows for vertices clamped to
its guard band, i.e. **the VU1's output is saturated**.

## 3. The game's data, before the VU1 (step 2)

* The display list lives in guest RAM; the console's own VIF1 DMA registers (`ee_hw.bin`, D1_TADR =
  0x5c5fe0, D1_MADR = 0x5c5ff0, QWC = 0, so the last transfer of the frame is there) point at
  0x5c5fe0-0x5c5ff0. The whole 64 KiB window around it is **99.5 % byte-identical** between the
  console's savestate (`work/ref/x/01/ee_ram.bin`) and the port's RAM dump
  (`work/ramdump/t114-title.bin`); the heap overall is 92.3 % identical, the whole 32 MiB 83.5 %.
* The matrix the VU1 uses (at VU1 offset 0x3d00 in the console's savestate) is **byte-identical** in both
  images and found at the same guest address in both: `0x0053fd50`, `1 0 0 1 / 0 1 0 1 / 0 0 1 1 /
  -0.0068466 2.791659 14.958234 1`.
* **But**: 1080 sixteen-byte blocks that the console fills with the 4-lane vector 0.502353
  (`0x3f009a34`) are, in the port, only *half* written:

  | | console | port |
  |---|---|---|
  | the float 0x3f009a34 | 1087 times (270 as a full 16-byte block) | **0 times** |
  | the float 0x3f009a35 (1 ULP higher) | **0 times** | 544 times (never as a full block) |
  | a full 16-byte block of it | 270 | 0 |

  One example (console | port), from the display-list region: `34 9a 00 3f 34 9a 00 3f 34 9a 00 3f 34 9a
  00 3f` | `35 9a 00 3f 00 00 00 00 35 9a 00 3f fb 03 01 6c` — two lanes written with a 1-ULP-higher
  value, one lane 0, one lane holding stale bytes of another structure.

  The 1-ULP difference cannot be an animation phase: the same computation gives a *different* result in
  the port, systematically, in >1000 places. The half-filled blocks may also be a phase difference; the
  1 ULP is not.

## 4. The VU1's side (step 4b), and why the VU1 engine is not the cause

The port's VU1 memories were dumped at frame 2700 with gdb (`dump_vu1.sh`; the binary has no debug info,
so the breakpoint is a non-debugging symbol and the 2700th call of `gs_trace::vsync` is frame 2700;
`DON_VU1=microvu`, and the same with `DON_VU1=pcsx2interp`).

| comparison | identical bytes |
|---|---|
| console VU1 **code** vs port, at the title | 15256 / 16384 (93.1 %); the *only* difference is that the console has a block of NOP filler at 0x3a40-0x4000 where the port has zeros (a region no program runs in) |
| console VU1 **data** vs port (microVU) | 6935 / 16384 (42.3 %) |
| console VU1 **data** vs port (PCSX2 interpreter) | 6933 / 16384 (42.3 %) |
| port microVU **vs** port interpreter | **16319 / 16384 (99.6 %), and identical at the VIF1 write area (0xf00)** |

At the VIF1's own write area (`VIF1_OFST = VIF1_TOPS = 0x1e1`, i.e. byte 0xf08, from the console's
`ee_hw.bin`):

```
console   -4663.4  4437.4  13.925  13.925 | 0.0298 0.0032 0.0298 0        (real coordinates)
microVU    3.4e+38 -3.4e+38 3.09e+38 3.09e+38 | 0.0298 0.0032 0 0        (saturated: ±FLT_MAX)
interp     3.4e+38 -3.4e+38 3.09e+38 3.09e+38 | 0.0298 0.0032 0 0
```

Two things follow:

1. **The difference is not in the VU1's execution**: two independent engines (microVU and PCSX2's
   interpreter) given the same input produce byte-identical data memories, including the saturated
   words. So the *input* to the VU1 is already wrong.
2. **The VU1 chain is faithful to the port's own RAM**: the port's VU1 data memory contains the garbage
   that the port's *guest RAM* contains (e.g. the 4-byte value `fc 03 01 6c` = 6.2388e+26 that sits in
   the port's display-list region is in the port's VU1 memory and in no console memory), and the
   saturated words appear in the VU1's output because the microprogram read those inputs and the GS then
   clamped the result. The unpack is therefore *not* inventing data: it carries the port's own (wrong)
   game data through.

## 5. VU0 (step 3)

* **The console's savestates have empty VU0 micro memory and data in all six screens**: `vu0_micro.bin`
  and `vu0_mem.bin` are 0 of 4096 non-zero bytes at 01-title, 02-menu, 03-charselect, 04-match-1v1,
  05-match-4p and 06-results, while the VU1's are 87 % and 54 % non-zero. So **the game runs no VU0
  microprograms (VCALLMS/VCALLMSR) on any of these screens**; the VU0 microprogram path cannot be the
  cause of the wrong 3D here.
* The game *does* use the VU0 in macro mode (COP2 instructions inline in the EE code) and the 128-bit
  VU0 memory instructions: a scan of the executable segment
  (`work/orchestrator/t3d/cop2_scan.py`, rabbitizer not needed: the fields are decoded by hand) counts
  LQC2 1060 and SQC2 1427 (the quad loads/stores of a VU0 register) and thousands of COP2 CO=1
  instructions. **Caveat: that scan reads the whole 3.9 MiB flat image, data included, so its counts are
  an upper bound and the macro funct names are ambiguous (two encodings map the low 6 bits); the reliable
  part is that the quad moves are frequent enough to matter and that the macro ops are used heavily.**
* The runtime *does* implement the microprogram path
  (`PS2Runtime::vu0StartMicroProgram` → `executeVU0Microprogram`, VU0 interpreter, 4096-cycle budget,
  `third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp:1510`). It is not what these screens use.
* A **real defect found on the way, not implicated by this evidence**:
  `generated/FUN_00102b08_0x102b08.cpp:37` reads the VCALLMSR start address as
  `ctx->vi[27] & 0x1FF`, but `R5900Context::vi` is `uint16_t vi[16]`
  (`third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime.h:79`) — an out-of-bounds read of the context,
  so that call starts the VU0 microprogram at a garbage address whenever it is executed (the same
  function also stores the correct value in `ctx->vu0_cmsar0`). It is a recompiler defect; it needs the
  code to be regenerated, so it is reported, not patched tonight. Which game code calls it was not
  determined (`grep -rl` over `generated/` with 57726 files hits the shell's argument limit; it needs
  `rg -l` or a `find`-based scan).

## Cause (first differing stage, with the evidence)

**The game's own vector data, written by the recompiled main-processor code before VIF1** (step 5a of
the card; the VU0 macro mode is the natural place, since that is where the game's 3D vector maths lives).

Evidence: the display list in guest RAM is 99.5 % identical to the console's and the VU1's matrix is
byte-identical, **but** 1080 16-byte vectors that the console writes are wrong in the port (1 ULP higher
in the lanes it writes, and only half of the lanes written at all); those wrong values are then carried
through VIF1 into the VU1 exactly (the port's VU1 memory contains them; the two VU1 engines agree to
99.6 %), and the microprogram that reads them overflows, so its output saturates (±FLT_MAX, Z
0xffffdf) and the GS clamps it to the guard band — which is the magenta/violet triangle soup.

Ruled out, with how:

| not the cause | how it was ruled out |
|---|---|
| the GS side, or the 2D art path | every 2D element is right; the memcard dialog's 919 fan vertices are inside 640x448 exactly as on the console |
| the runtime's VIF1 unpack *as such* | it carries the port's own RAM data faithfully (the port's VU1 memory contains the port's RAM garbage; no value appears in the VU1 memory that is not in the port's RAM or computed there) |
| the VU1 engine (microVU vs interpreter) | the two engines' data memories are identical to 99.6 % and identical at the write area |
| the VU1 microcode | identical to the console's where programs live (only a NOP filler block differs) |
| FP contraction / build flags | `build/linux-dev/compile_commands.json`: `-ffp-contract=off -march=x86-64-v3` on the generated code, the runtime, `src/game/` and `src/host/` |
| the VU0 *microprogram* path | the console's VU0 code memory is empty on all six screens |

## Fix

None applied: the defect is in the *generated* code's arithmetic (a recompiler or runtime-emulation
defect), and the fix needs `tools/pipeline/generate.sh` and a full rebuild, which is the orchestrator's
to schedule.

## What the next person needs (the exact defect to find)

1. Take the guest address of one of the wrong 16-byte vectors (e.g. 0x005aacf0, or 0x005c0030) and find
   the *store* that writes it: the value 0x3f009a35 in the port is 1 ULP above the console's 0x3f009a34
   and is written to lanes 0 and 2 only, with lane 1 zero and lane 3 stale.
2. That store is in a recompiled function: disassemble it (`bash tools/re/decompile.sh 0xADDR`) and
   compare the MIPS (a COP2 macro op such as VMULx/VMULA/VDIV with a destination mask, or an LQC2/SQC2
   quad move) instruction by instruction with the generated C++
   (`third_party/PS2Recomp/ps2xRecomp/src/lib/vu_translator.cpp` emits `_mm_blendv_ps` with a mask built
   from `inst.vectorInfo.vectorField`; `ps2_runtime_macros.h` has the `PS2_VMUL*` SSE macros). The two
   suspects are the *destination mask* (which lanes are written) and a 1-ULP arithmetic difference
   (a division/reciprocal or a rounding mode).
3. Compare the *match* screen's data the same way (only the title's RAM image was compared here).

## Verification

* `uv run --no-project --with pytest python -I -m pytest tests/test_gif_stats.py -q` → 10 passed.
* The console frames: `tools/verify/gif_stats.py` on 00-memcard, 01-title, 04-match-1v1
  (`console-*.txt`/`.json`); the port's frame: `port-title.bin` (`port-title.txt`).
* The RAM comparisons of step 2 (`matrix_scan.py`, the block searches above; all figures in this entry).
* The VU1 dumps: `dump_vu1.sh` with `DON_VU1=microvu` and with `DON_VU1=pcsx2interp`
  (`gdb-vu1.log`, `gdb-vu1-interp.log`, `port-vu1-*.bin`, `port-interp-vu1-*.bin`).
* The VU0 check: the six savestates' `vu0_micro.bin`/`vu0_mem.bin` byte counts; `cop2_scan.py`.

## Temporary diagnostics

`src/host/gs_trace.{h,cpp}` and its two calls in `gs_glue.cpp` (off unless `DON_GS_PACKETS`), and the gdb
script `dump_vu1.sh`. Both are diagnostics for this task and are meant to stay only as long as they are
useful; nothing else in the tree was changed.

## Part 1 (2026-10-13, later): the writer, the instruction, and the fix

The orchestrator's reading of the `fb 03 01 6c` bytes in the fourth lane was right, and it opened the case:
those bytes are the word `0x6C0103FB`, a VIF code — `UNPACK V4-32`, address `0x3FB` — i.e. the 16-byte block
is the *data* of that unpack, one per-object constant the game uploads into the VU1's constants area at the
top of its data memory (the same area as the matrix at 0x3D00). **The verdict above stands and is now
narrowed to the exact defect.**

**The function.** Scanning the executable segment for code that builds that code word
(`lui $r, 0x6C01` followed by `ori $r, $r, 0x3FB`) gives 15 sites; the first (0x25BC30 with the correct
ELF mapping, see the note below) is inside **FUN_0025b8f0 (0x25b8f0-0x25be44)**, and its sibling constants
(0x3FC, 0x3FD, ...) are built in the same function, one display-list record apart. The generated file is
`generated/FUN_0025b8f0_0x25b8f0.cpp`; the relevant part (its comments give the address and the MIPS):

    0x25bc30  lui   $v1, 0x6C01 ; ori $v1,$v1,0x3FB ; dsll 16 ; ori 0x100 ; dsll 16 ; ori 0x404
    0x25bc48  pcpyld $v0, $v1, $v0          -> SET_GPR_VEC(ctx, 2, PS2_PCPYLD(GPR_VEC(ctx, 3), GPR_VEC(ctx, 2)));
    0x25bc4c  sq     $v0, 0x0($a2)          -> WRITE128(...);   // the VIF code + the next word
    0x25bc9c..0x25bcc4  the data:
        f6 = b47 * f1 ; f7 = b45 * f5                  (mul.s, the object's bytes 0x44..0x47)
        v0 = mfc1(f6) ; v1 = mfc1(f7)                  (the float into lane 0, sign extension lane 1)
        v0 = pexew(v0) ; v1 = pexew(v1)                -> PS2_PEXEW(...)
        f6 = b46 * f5 ; f7 = b44 * f5 ; v0 = mfc1(f6) ; v1 = mfc1(f7)
        v0 = ppacw(v0, v1)                             -> PS2_PPACW(...)
    0x25bcc8  sq     $v0, 0x10($a2)         -> WRITE128(...);   // the 16 bytes of the constant

**The defect.** `pexew` is the idiom that moves a value from lane 0 to lane 2, and `ppacw` gathers
`x, y, z, w`, so with the console's semantics the four lanes come out equal (`a a a a` for an object whose
four bytes are equal). The runtime's macro, however, was

    #define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(2, 3, 0, 1))

which swaps the lane *pairs* (0,1) and (2,3); the console's `PEXEW` (PCSX2 2.8.2,
`third_party/pcsx2/pcsx2/MMI.cpp`, "UL[0]=Rt[2]; UL[1]=Rt[1]; UL[2]=Rt[0]; UL[3]=Rt[3]") swaps lanes 0 and
2 and keeps 1 and 3. With the wrong macro the constant comes out `a 0 a <stale>` — the port's RAM has
exactly that (`3f009a34 00000000 3f009a34 6c0103fb` at 0x5aacf0), the microprogram then multiplies or
divides by a garbage `w`, saturates (±FLT_MAX), and the GS clamps the result to its guard band: the flat
magenta triangles. **116 sites in 32 files of `generated/` call `PS2_PEXEW`.** The same one-line defect was
in `PS2_PEXEH` (it swapped halfword pairs the same way; the console swaps halfwords 0 and 2 and 4 and 6 —
0 sites today), fixed in the same patch; `PS2_PEXCH` (the byte variant) has no macro in that header at all,
so a game that emitted it would not compile (noted, not added).

**The fix and its proof.** `patches/ps2recomp/0037-pexew-lanes.patch` (one line each for PEXEW and PEXEH,
**not** in `series`; a change of the runtime's headers means every generated file has to be recompiled, so
the orchestrator schedules it). New test `tests/runtime/mmi_lanes_test.cpp` (+ one line in that
CMakeLists), which checks every MMI macro the game emits against a reference written from PCSX2's `MMI.cpp`
and then the exact `FUN_0025b8f0` sequence:

    cmake -S tests/runtime -B build/rt-tests-3d     -G Ninja -DPS2X_WITH_RAYLIB=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    cmake --build build/rt-tests-3d -j2 --target mmi_lanes_test && build/rt-tests-3d/mmi_lanes_test
      FAIL PS2_PEXEW                      got 40050607 80010203 ffff0b0c 0008090a  want 0008090a 40050607 80010203 ffff0b0c
      FAIL the port's constant block is `a a a a`   got 3f009a34 00000000 3f009a34 00000000
      mmi_lanes_test: 26 checks, 6 failed                     (exit 1)

    same test against a scratch worktree with 0037 applied (build/rt-tests-3d-fix, -DPS2RECOMP_DIR=work/t3d/PS2Recomp-0037):
      mmi_lanes_test: 26 checks, 0 failed                     (exit 0)

The sibling sweep (every `PS2_P*` macro the generated code emits: 1538 PCPYLD, 202 PPACW, 116 PEXEW, 88
PADDH, 67 POR, 48 PAND, 39 PNOR, 36 PEXTLB, 32 PMINH, 32 PMAXH, 24 PSUBB, 18 PEXTUB, 16 PPACB, 8 PEXTUW,
8 PEXTLW, 7 PSUBW, 6 PXOR, 6 PCGTH) is in the test's table: **PEXEW and PEXEH were the only two wrong**;
the rest match PCSX2's definitions, including the PEXT* family's argument order (the macro's `(rs, rt)`
with `_mm_unpacklo(b, a)` interleaving `rt` first, as PCSX2 does).

**A correction to my own work, worth keeping.** The address my `work/orchestrator/t202/disasm.py` printed
for that code (0x25BCB0) was **0x80 too low**: the ELF's first LOAD segment has `p_offset = 0x80` and
`p_vaddr = 0x100000`, so `vaddr = file_offset + 0xFFF80`, not `file_offset + 0x100000`. The generated
files' own comments are right (0x25BC30) and were used above; the RAM addresses quoted in this entry are
absolute guest addresses and are unaffected. Fix the helper before using it again.

**One consequence to keep in mind.** `tests/runtime/mmi_lanes_test.cpp` fails by design against
`third_party/PS2Recomp` as it is, so a `pytest tests/runtime` that is pointed at a build folder containing
it (e.g. `DON_RT_TESTS_DIR=build/rt-tests-3d`) reports a failure until patch 0037 is in the tree **and** the
generated code has been rebuilt. That is the intent (a test that shows the defect), not a regression of
something that used to pass.

## With patch 0037: what the game looks like (2026-10-13, T-202 verification)

**Binary.** A side build, not the shared tree: `build/linux-3d/don`, built (5m24s, 1,967 units, `-j 8`, peak
memory 7,121,575,936 bytes) against the runtime tree `work/3d/PS2Recomp` = `third_party/PS2Recomp` at its HEAD
(2c5fbb9) + the patches of the COMMITTED series (0001..0034) + `0037-pexew-lanes.patch`, and nothing else
(`grep define PS2_PEXEW` shows `_MM_SHUFFLE(3, 0, 1, 2)`; `compile_commands.json`: 0 include paths into
`third_party/PS2Recomp`, 2,032 into `work/3d/PS2Recomp`). Same `generated/` as the shared build, recompiled
because the runtime header changed. All runs below: `DON_VU1=microvu`, `--headless --renderer software`, no
sound, the machine loaded by other agents (load average 6.8 to 8.6 on 16 CPUs).

**The unit test against this tree** (`build/rt-tests-3dv`, host build, 2 jobs): `mmi_lanes_test: 26 checks, 5
failed`. The 5 are `PS2_PADDH`, `PSUBH`, `PMAXH`, `PMINH` and `PCGTH`, and in every one the *reference* column
is all zeros (`want 00000000 ...`): the test's reference function for those reads the lanes through
`reinterpret_cast<uint16_t*>` of a `uint32_t` array (`ref_halfword_op`, `ref_pcgth`), which this host compiler
at no optimisation level silently folds to zero. They do not involve PEXEW/PEXEH: the checks for those two
and for the `FUN_0025b8f0` sequence pass with 0037 (journal 017 records `11 failed` for the same test in both
its trees, so this count depends on the host compiler too). Reported, not touched (the test is not mine).

### Figures (SSIM / mean absolute difference, `tools/verify/image_parity.py`)

| screen (frame) | against | before 0037 | with 0037 |
|---|---|---|---|
| title (2700) | the console's screenshot | 0.53 | **0.965 / 3.6** |
| title (2700) | the replay of its dump | 0.53 | **0.959 / 5.3** |
| menu (3000) | screenshot / replay | 0.52 | 0.857 / 15.9, 0.854 / 16.3 |
| menu (3060) | screenshot | | 0.902 / 11.0 |
| menu (3300, the static end) | screenshot | | 0.882 / 12.3 |
| character select, practice (4980) | screenshot / replay | 0.40 | 0.755 / 20.2, 0.688 / 25.4 |
| character select, first (3600) | screenshot / replay | | 0.896 / 9.5, 0.833 / 14.3 |
| match, GET READY (6540) | screenshot / replay of 04 | 0.19 | 0.236, 0.214 |
| match (7140, 7440, 7740) | screenshot | | 0.32, 0.32, 0.30 |

The match and the practice character select are not comparable by this figure: the console's reference is
another moment of an animated scene (a different phase of the pictures, the 1P/Com cursor on the right
fighter, a fighter standing in a different place). The title figure, where the scene hardly moves, is the
honest one: 0.965 against the console's own screenshot-against-replay 0.98.

### What the pictures look like

* **Title (frame 2700; also 2640, 2670, 2730).** The stadium is there: the floodlight towers with their
  bright lamps, the blue beams, the steel scaffolding at both sides, the crowd, the runway going away in
  the middle, the 3D depth. The logo, the three characters, the PRESS START text and the copyright lines (2D)
  are as before. Differences from the console's frame, from an amplified difference image (the console's
  screenshot is 640x480 and the port's 640x448: the screenshot was resized to 448 rows first, so every edge
  shows in the difference and the figure has a floor from that): the largest area is the crowd at bottom left
  (mean absolute difference 9.8 against 4.3 to 5.2 elsewhere; yellow in the difference: the red and green of
  that patch are off, a light or colour difference, the picture is there), then thin coloured spots on the
  beams at the left and right edges and the lamps' brightness. No object is missing. The
  scene moves between the shots (SSIM 0.930, 0.947, 0.959, 0.926 at frames 2640, 2670, 2700, 2730), as the
  console's does.
* **Main menu (3000).** The stadium behind the menu with the big screen showing the two fighters and the
  wooden stage is there and recognisable, the lights are right. What differs from the console's frame is the
  *phase* of the animation: at 3000 the menu panel is still sliding in (the ticker text, the position of the
  buttons, the position of the right and left blue columns), by 3060 it is nearer (0.902) and at 3120 settled.
* **Character select (3600, 4980).** The 3D stadium and the big screen behind are right; the 2D portraits,
  name plates, grid and footer are right. The big screen shows a different stage at the port's frame than the
  console's one (the screen's content is a texture that changes over time).
* **Stage list (5880, `stage_select.png`).** The deck of the ship from a low 3D view, the framework of
  lights, the big screen with the ship's picture, the 2D list: all right to the eye.
* **Match (6540 onward).** The stage (Going Merry's deck, the sea, the island, the sails with the skull
  flag), the two fighters (Naruto and Sasuke, textured, with their animations: running, kicking, a special
  move with a full-screen cut-in, throws, knock-down), the shadows/effects (smoke puffs, a hit flash, a
  rainbow effect on the health gauge), the water, the HUD (names, gauges, the "GET READY" and "FIGHT" art) are
  all drawn and look like the game. Things that look odd and could not be judged without a console frame of
  the same instant (**not verified**): (a) in two stretches (about frames 6720-6780 and 7260) the whole 3D
  view, fighters included, is turned 90 degrees while the HUD stays upright; (b) after the first round is lost
  (Naruto does nothing: 1P is not pressed) the HUD shows both portraits overlapped at the left and the two
  names run together ("NARUTESASUKE") for about 300 frames, then a new round begins; I do not know if that
  is the console's layout in that state.

### Does the match stay on "GET READY"? (journal 010)

**No.** `build/linux-3d/don`, `work/3d/match_long.txt` (the shipped route, quitting at 8640), exit 0, 3m49s of
wall time, 8,640 vblanks: frame 6540 is the "GET READY" art, **6600 is "FIGHT"**, 6660 onward is the fight:
the fighters move, attack, a special move (frame 7020, a cut-in picture) and a KO; the stage camera moves;
the pictures are all different from 6540 to 8520 (only 8580 and 8640 are byte-identical, the start of a
new round). In the following 20 seconds of game time (frames 6540-7740) the run did not stop and nothing in
the log is anomalous (`classify_log.py`: no modules, no warnings). One run only: whether the other of the
four runs of journal 010 would have frozen is not known.

### Speed

* The software renderer at the title and the menu: `[don] GS: 3300 vblanks, 0.58 ms in the GS per vblank (max
  7.25)` (before the fix: 36 ms). Whole match route, 8,640 blanks: 0.97 ms per blank (max 7.42); 0.54 ms at
  the first 1200 blanks, 0.58 at 6000, 0.81 at 7200, 0.96 at 8400 (the match is more expensive: 5,777 MiB of
  GIF data in the run).
* Vblanks per wall second (shots' file times): title 2640-2760: 120 blanks in 3.6 s = 33; menu 2940-3060:
  120 in 3.1 s = 39; whole run 3300 blanks in 81 s = **40**; match 6540-8640 (2,100 blanks, 59 s) = **36**.
  So the game runs at about 0.6 of the console's speed on this machine, loaded; the host is still "late" for
  every blank.

### Packets and memory (the mechanism, measured)

* `DON_GS_PACKETS=work/3d/port-title.bin DON_GS_PACKETS_FRAME=2700` + `gif_stats.py`: 611 tags, 9,142 vertices
  (strip 8,904, fan 198, point 40); **positions x=[-454.5, 1095.4] y=[-289.0, 739.6] px** (before: about
  +-2047; the console's title frame: x=[-454, 1095], y=[-289, 740]: **equal**). The textures named at the
  title are the console's: 0x2bc000 (port 3405, console 3301), 0x2c2000 (1723, 1374), 0x2ea000 (736, 736).
* A match frame (6540, `work/3d/port-match.bin`): 2,270 tags, 66,165 vertices (strip 65,705, fan 228, triangle
  192, point 40). Console's frame 04: 14,925 vertices. Textures in common with the same counts: 0x2d0000 3134,
  0x2f0000 2459, 0x2c4000 840, 0x30a000 420 (the console: the same figures); 0x2d6000 4465 (4845),
  0x2ca000 737 (731), 0x2d2000 730 (730). Textures only in the port's frame: 0x37a000 8620, 0x3a6000 2832,
  0x368000 2330, 0x37e000 1046, 0x3b8000 709 (the fighters or the effects of that instant in the port's
  frame; the console's frame is another moment, so this is not a difference by itself).
* `DON_DUMP_RAM=work/ramdump/3d-title.bin`, route `work/orchestrator/t3d/to_title.txt` (exit 0): the aligned
  word 0x3F009A34 appears **1,086 times** (the console's image: **1,087**; the port before: 544) and 0x3F009A35,
  the old wrong value, **0 times**. The one word of difference is not explained here.

### What is still different, and which stage it belongs to

Nothing below is a fix; each item says what the tools show (measured) and what is a reading (supposition).

1. **Colour/lighting of parts of the 3D scene** (the bottom-left crowd of the title: 9.8 mean difference
   against about 5 elsewhere, yellow in an amplified difference; the brightness of the lamps and spots on the
   beams at the edges; the menu's and character select's scene against the console's, after allowing for the
   animation phase). *Measured:* the geometry is where the console's is (title vertex range equal to a
   tenth of a pixel; 34 of the console's 38 (texture, size, format, blend, fog) states are in the port's
   frame too, with the same counts for the big ones); no object missing. *Supposition:* the vertex colours
   the microprogram outputs (they depend on per-object colour and light data the game uploads, the same kind
   of data that PEXEW broke) or the texture palettes; it is the vector unit's inputs or the GS colour path.
   The next measurement is to dump a single object's colour vector in both RAM images, like journal 013 did for
   the matrix.
2. **One word in 1,087 of the constant block** (1,086 in the port): not explained; it can be the animation phase.
3. **Four texture states only in the console's title frame** (0x2d0000 256x256 PSMT8, 17 vertices; 0x2f6000
   16x32, 119; 0x2f8000 64x1024, 17) and one only in the port's (0x2c2000 64x64, 5): *measured* difference of
   the two frames' contents; *supposition*: the animation phase (the console's frame is dump frame 4 of 4 of
   another instant), not a defect. The port's depth values are no longer saturated (z 61..65535, none at
   0xffffdf).
4. **The two 90-degree camera stretches** in the match and the overlapped HUD names after Naruto's KO (see
   above). Not measured at all; to be compared with a console recording of the same fight, which this task
   did not have. If it is a defect it is upstream of the GS (the camera matrix comes from the game's own
   vector data, the same kind of data as 0037's) and would also show in the first-round packets.
5. **The animation phase** of the menu (the panels are sliding at frame 3000) is the cause of the menu's
   0.86 and of the lower figure of the character select: these are not defects.
6. **Speed:** the game runs at 36 to 40 vertical blanks per wall second (0.6 of real time) on this loaded
   machine, still late for every blank. *Supposition:* now the generated code and the VU1 emulation are the
   time, not the GS (0.6 to 1.1 ms per blank).
7. **The menu is a static picture from 3180 on** in the title/menu route (frames 3180, 3240 and 3300 are
   byte-identical, 60 blanks apart, with the menu idle). The console's menu has moving parts (the ticker, the
   lights); whether it should still move after it has slid in was not checked against the console. *Not
   understood;* not followed.

Not verified: the console's frames at the same instants as the port's (no recording of the same fights);
every judgement of a picture is mine, by eye, next to the console's screenshot; one run per route.

### With patch 0038 as well (second round, same day)

`build/linux-3d/don` relinked with `0038-pacing-does-not-change-guest-state.patch` applied on top of the same
tree (it applied cleanly to the series through 0034 + 0037; only `EeScheduler.cpp` is recompiled: 3 units,
11 s). The shots of every route are the same as with 0037 alone: **title/menu route, 3,300 blanks: the 7 named
shots (`title_m60`, `title_m30`, `title`, `title_p30`, `menu_m60`, `main_menu`, `menu_p60`) and all 55 automatic
shots byte-identical to 0037's; match route, 8,640 blanks: all 144 automatic shots byte-identical** (including
"GET READY" at 6540, "FIGHT" at 6600 and the whole fight). So 0038 changes the guest's pictures in neither
route (journal 017, "With the game: first runs", has the state check). Per-blank cost of the GS: 0.55 ms (title
and menu route), 1.07 ms (match route; 0.97 with 0037).

## 2026-10-13 · The picture against the console after patches 0039 and 0042

Task T-202. Two runs of `build/linux-dev/don` (`--headless --renderer software`, `DON_PLAY_MOVIES=1`, Japanese
disc), scripts `to_main_menu.txt` (`work/shots/look-menu/`) and `to_match_1v1.txt` (`work/shots/look-match/`);
both ran to the end (exit 0). Tool: `tools/verify/image_parity.py`; regions with `work/look/region.py`
(SSIM and mean absolute difference over a crop, plus the crop's mean colour). The console's frames are
the replays in `work/gsdump/` (480-line screenshots are scaled to 448 by the tool).

| screen | shot | against the replay (SSIM / MAD) | against the console's screenshot | earlier figure |
|---|---|---|---|---|
| title | `title` | 0.959 / 5.3 | 0.965 / 3.6 | 0.96 |
| main menu | `main_menu` (frame 3060, panels still sliding) | 0.854 / 16.3 | 0.857 / 15.9 | 0.85-0.90 |
| main menu, settled | frame 3300 (static from 3180 on) | 0.882 / 12.6 | | |
| character select | `charselect` | 0.833 / 14.3 | 0.896 / 9.5 | 0.90 |
| practice select | `charselect_practice` (Com chosen as well) | 0.688 / 25.4 | 0.755 / 20.2 | |
| stage list | `stage_select` | no console frame | | |
| match | `match_intro`, `match_1v1`, `match_2`, `match_3` | 0.27-0.32 / 53-65 | 0.30 / 61 | (another fight) |

**Title.** Same picture to the eye: logo, text, ring, the light beams, the lamps. Regions (replay vs port):
lamps (y 0-70) 0.986, logo 0.981, ring 0.991, text 0.959. The two earlier differences are **mostly gone**:
the lamps' mean colour is equal (135,151,192 vs 134,150,192). The crowd is the only thing left: bottom left
(x 0-200, y 300-448) SSIM 0.891, mean colour 56,61,79 (console) against 71,74,83 (port), so the port is
**lighter**, by 13 to 15 levels of red and green, not darker; bottom right (x 460-640) SSIM 0.868, mean
52,67,90 against 46,52,72, the port **darker** by 6 to 18. The crowd is animated (heads), so some of this
is phase; the opposite signs on the two sides are not explained by phase and are the one thing that
remains at the title, small to the eye.

**Main menu.** The same layout, texts, big board with the fight in it, the cursor on the first entry. The
difference of the score is the panels' slide phase (at frame 3060 the port's ticker and bottom bar are
still sliding; at frame 3300, with the menu settled, they sit where the console's do and the score goes
to 0.882). Regions of the settled menu: sky and lamps 0.937 (mean colour 109,114,136 vs 110,117,140),
board 0.942, left crowd 0.911 (mean 84,95,115 console, 91,105,122 port: the port's crowd is again a
little lighter on the left). Nothing else that animation does not explain.

**Character select.** Same background, portraits grid, HUD strip, board. The shot `charselect` is a
different moment of the slide (portrait's and board's phase); `charselect_practice` has two characters
chosen (Naruto and Sasuke with the "Com" tag), the console's frame only one, so its figure measures a
different state, not a defect. The coloured name plates, portraits, icons and the bottom button bar look
the same. No missing or extra object that the state does not explain.

**Stage list.** No console frame exists, so nothing is measured. By eye: the stage board with the ship
picture, the three entries with the middle one highlighted, the side banners ("STAGE SELECT"), the lamps
and the button bar are all drawn and legible, no wrong colours.

**Match.** The fight is not the same moment, so the figures mean nothing (regions compared anyway:
HUD 0.51, sky 0.36, sea 0.17, deck 0.20). By kind: the fighters' colours and shading match
(Naruto's orange 205,106,58 in the port, 204,106,58 in the replay), the black-blue of Sasuke, the sea and the sky (blue
pixels' mean 86,131,173 against 84,125,163), the water splash, the sails and the hull textures, the dust
puffs, the red chakra discs on the bodies (they are in the console's frame too, 734 and 362 red
pixels in the screenshot and the replay), the 1P marker, the HUD (health bars with their gradients, portraits,
name plates, the small gauge under each name). Only the small gauge under Naruto is green in the port at that
instant and yellow in the console's: a different moment of the fight (in the port's `match_intro` it is
green as well, in the console's yellow), not measured further. The earlier suspicions (camera stretch, overlapped HUD names) were
not looked for: only the four named match shots were viewed.

**Order of what a player would notice:** (1) nothing on a match, the title or the menu that I could
tell from the console's; (2) the crowd at the title is lighter on the left and darker on the right; (3)
nothing else.

Not verified: the stage list against the console; the match at the same instant; crowd differences
are not separated from the animation phase; one run per route; all the visual judgement is by eye from
single frames, and I did not look at every one of the 130 automatic match shots.
