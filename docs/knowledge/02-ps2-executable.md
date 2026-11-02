# 02 · The PS2 executable (`SLPS_256.75`)

Everything in this document is **measured** on the Japanese ELF, unless stated otherwise.

## Summary

| | |
|---|---|
| Size | 3,973,040 bytes |
| SHA-256 | `994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1` |
| File CRC32 | `0x7E09D26D` (the runtime uses it to identify the game) |
| Format | ELF32, little endian, MIPS-III, R5900 variant, `eabi64` ABI |
| Entry point | `0x00100008` |
| Symbols | None: the `.symtab` table exists but is empty |
| Compiler | `MW MIPS C Compiler (2.4.1.01)` (Metrowerks CodeWarrior for PS2), according to `.comment` |
| Language | Looks like C: there is no trace of a C++ runtime (`pure virtual`, `type_info`...) in the strings |
| Link date | 2006-06-13 |

## Memory map

There is **a single loadable section**, with read, write and execute permissions, that mixes
code and data. This is the main trap for any tool: if it walks the whole section as code, it
translates data as if it were instructions.

Measured by `tools/funcmap/layout.py` (task T-008) from `config/functions.ghidra.csv`, the ELF
and `docs/knowledge/data/vu_programs.tsv`. The result is in `config/layout.toml`.

| Range | Size | Contents |
|---|---|---|
| `0x00100000`–`0x003902D4` | 2,687,700 B (2.69 MB) | Main text: the code of the game, the SDK and the libraries |
| `0x003902D4`–`0x0039030C` | 56 B | Padding: 54 zero bytes and two words `0x60000042` (at `0x390300` and `0x390308`); not instructions |
| `0x0039030C`–`0x004B2878` | 1,189,228 B (1.19 MB) | The 141 VU microprograms (911 KB of instructions) and the data between them |
| `0x003BC428`–`0x003BD8C8` | 5,280 B | **Island** inside the VU gap `0x3BC0C0`–`0x3C695C`: MIPS code, `kernel_patch_blob` |
| `0x003C0DB0`–`0x003C12A4` | 1,268 B | **Island** in the same gap: `false_positive` (stray labels over data) |
| `0x004BB9A0`–`0x004C9D80` | 58,336 B | **Island** at the end of the data: `false_positive` (stray labels over data) |
| `0x003902D4`–`0x004C9D80` | 1,284,780 B (1.28 MB) | Initialized data as a whole (contains everything listed above from the padding on) |
| `0x004C9D80`–`0x005A6800` | 903,808 B (0.90 MB) | Uninitialized data (`bss`) |

Islands are the CSV rows at or beyond the main text end, merged when they are less than `0x400`
bytes apart. A row longer than `0x10000` bytes inside an island is a stray label that Ghidra gave
a huge span (for example `entry_003c12a0` claims `0x3C12A0`–`0x4BB9A0`); it is counted as one
word, otherwise it would swallow the islands after it.

| Island | Function rows / labels | Kind | Evidence |
|---|---|---|---|
| `0x3BC428`–`0x3BD8C8` | 9 / 55 | `kernel_patch_blob` | The words of the 9 function rows all decode as R5900 and use `lui` `0x8007`. The 62 words of the label-only holes (first at `0x3BC728`) do not decode: data between the blobs (a table at `0x3BC760` read by the code at `0x115B68`, and words at `0x3BCA68` that hold addresses inside the island). |
| `0x3C0DB0`–`0x3C12A4` | 0 / 2 | `false_positive` | Only `entry_*` labels; 216 words do not decode as R5900. It sits in a gap between VU microprograms. |
| `0x4BB9A0`–`0x4C9D80` | 0 / 3 | `false_positive` | Only `entry_*` labels; 4,881 words do not decode as R5900. |

Who uses the kernel patch blob (**Measured**, `lui`/`addiu` pairs in the main text that build an
address inside the island):

| Code address | CSV function | Builds | Call that follows |
|---|---|---|---|
| `0x115C60` | `FUN_00115c30` | `0x3BC428` | `FUN_00115BC8(0x80075000, 0x3BC428, 0x330)` |
| `0x116778` | `FUN_00116740` | `0x3BCA98` | `FUN_00116680(0x80074000, 0x3BCA98, 0x7A8)` |
| `0x116B40` | `FUN_00116af8` | `0x3BD260` | `FUN_00116AA0(0x80076000, 0x3BD260, 0x740)` |
| `0x115B68` | none (`0x115B40` has no CSV row) | `0x3BC760` | Indexed table access (`lw`/`sw` at `base + 4 * $a2`), not a copy. It reads a table of words, not code |

The three callees are identical stubs: `syscall` with `$v1` = `0x5A`, a word-copy loop
(`$a0` destination, `$a1` source, `$a2` bytes) and a second stub with `$v1` = `0x5B`. The
destinations `0x80074000`, `0x80075000` and `0x80076000` are kernel memory and each span ends at
or before the next blob (`0x3BC428`+`0x330` = `0x3BC758`, `0x3BCA98`+`0x7A8` = `0x3BD240`,
`0x3BD260`+`0x740` = `0x3BD9A0`). So the blobs are code that the SDK copies into kernel memory;
the island is not game code that runs in place. **Unverified**: the SDK name of syscall `0x5A`
(the same CSV names `0x5B` `RFU091` and `0x74` `RFU116_SetSyscall`), when the three callers run
(boot or on demand) and what each blob patches. Pointer words to the island outside it: none.

**History.** The first estimate of this document put the end of the code at `0x381000`. It was a
page-density heuristic (pages with more than 97 % valid instructions and at least one
`jr $ra`), and it was wrong: the code goes on for another 62,164 bytes, up to `0x3902D4`. The
Ghidra export of T-006 showed it. The rule "rows less than `0x400` bytes apart are one run" in the
card of T-008 was also wrong for the main text: real code has holes that Ghidra did not turn
into functions (`0x488` bytes at `0x2B8DB8` and `0x15A0` bytes at `0x3362F0`, both fully valid R5900
code), so the main text uses a limit of `0x10000` bytes (the next gap after `0x3902D4` is
`0x2C154` bytes). The main text end is the `End` of `FUN_00390260`, whose last instruction
(`nop` in the delay slot of `jr $ra`) is at `0x3902D0`.

## Code split by origin

Estimated by locating which areas reference strings or constants of each library
(`tools/layout_survey.py`). The boundary between game and RenderWare may move by ±100 KB.

| Approximate range | Size | What it is |
|---|---|---|
| `0x100000`–`0x130000` | ~190 KB | Sony SDK (libgraph, libdma, libpad, libcdvd, libmc, kernel...) |
| `0x130000`–`0x262000` | ~1.2 MB | **The game's own code** |
| `0x285000`–`0x310000` | ~550 KB | RenderWare 3.7 |
| `0x30C000`–`0x350000` | ~270 KB | CRI (ADX, Sofdec) and Sony's libmpeg |
| `0x350000`–`0x3902D4` | ~260 KB | C runtime and math (the first estimate ended at `0x381000`; the end of the text is `0x3902D4`, see "Memory map") |

## Libraries identified by strings

- Sony SDK 3.0: `PsIIlibgraph3000`, `PsIIlibdma  3000`, `PsIIlibipu  3000`, `PsIIlibkernl3000`,
  `PsIIlibsdr  3000`, `PsIIlibcdvd 3000`, `PsIIlibscf  3000`; `PsIIlibpad  3020`; `PsIIlibmc   3030`.
  Also libmtap (multitap) and libmpeg.
- CRI Middleware, compiled on 28-02-2005 and 01-03-2005: `ADXT/PS2EE Ver.9.44`, `ADXF/PS2EE Ver.7.30`,
  `ADXPS2 Ver.2.60`, `CRI SFD/PS2EE Ver.1.958`, `CRI MPV/PS2EE Ver.1.958`, `CVFS/PS2EE Ver.2.39`, `SJ/PS2EE Ver.6.31`.
- RenderWare Graphics 3.7.0.2 (see [04-graphics.md](04-graphics.md)).

## Instruction census (code area only)

Counted over the main text, `0x100000`–`0x3902D4`, word by word (the few words of the text that
no Ghidra row covers are included). Recounted on 2026-10-11 after T-008; the first version of
this table stopped at `0x381000`.

| Class | Count | Note |
|---|---|---|
| `jal` (direct calls) | 25,404 | 5,065 distinct targets inside the code |
| `jr $ra` | 7,916 | |
| Floating point (COP1) | 46,308 | The game is very floating-point intensive |
| `sqrt.s` | 96 | In 56 functions. In 47 of them the operand is not `$f0` and PS2Recomp without the PR #277 patch reads the wrong register; in the other 49 the result matches by chance |
| `rsqrt.s` | 0 | |
| VU0 in macro mode (COP2) | 6,784 | Supported by PS2Recomp |
| `vcallms` / `vcallmsr` | 0 / 1 | The only one is at `0x102B14`, inside the SDK (`sceDevVu0*` area). VU0 in micro mode is not used in practice |
| MMI (128 bits) | 1,580 | |
| `lq` / `sq` (128-bit loads and stores) | 11,841 / 12,399 | |
| `lqc2` / `sqc2` | 1,059 / 1,418 | |
| `syscall` | 168 | 145 distinct identifiers |
| COP0 | 944 | |

Hardware accesses, counted as `lui` with the high part of the address:

| High part | Uses | What is there |
|---|---|---|
| `0x1000` | 1,359 | CPU registers: timers, IPU, GIF, VIF, DMA, interrupts |
| `0x1001` | 47 | |
| `0x1100` | 12 | Not accesses to the memory of the vector units, as first assumed: the 12 build constants (the VIF command `FLUSH`, `0x11000000`, and two engine ids). **Measured** in T-121, see [04-graphics.md](04-graphics.md) |
| `0x1200` | 30 | Privileged GS registers |
| `0x7000` | 401 | Scratchpad: used heavily |
| `0xB000` | 3 | |

## Functions

| Source | Result |
|---|---|
| Ghidra export (`config/functions.ghidra.csv`) | 57,789 rows: 8,050 functions and 49,739 labels (names starting with `entry_`, `caseD_`, `default`); after T-025 it had 56,246 rows (7,111 functions, 49,135 labels), and T-026 added the code that no function owned (see below). 0 nested functions. Beyond the main text end the island rows are unchanged: 9 function rows (all in the island at `0x3BC428`) and 60 label rows (55 in that island, 5 stray ones in the other two) |
| PS2Recomp analyzer, heuristic mode | 5,817 functions; 575 SDK names recognized (406 renamed, 169 added) |
| `jal` targets inside the code | 5,065 distinct, counting every word of the main text; 5,048 counting only the `jal` inside CSV rows, of which 13 are not the start of a row |
| Code addresses referenced only from data tables | 3,331 distinct (out of 7,278 words) |

A function in Ghidra can have a body made of several separate ranges. **Measured** (T-025): the
original export script wrote `End` = highest address of the body, so 12 functions of 24 to 92
bytes (`FUN_003163c8`, `FUN_00349b38`, `FUN_00350370` and nine between `FUN_0034b400` and
`FUN_0034c3f0`) had spans of 19 to 118 KB, 1,775 functions lay inside the span of another, 1,114
rows had `Size` different from `End - Start`, and the spans of the main text added up to 3.71 MB
for a 2.69 MB text. Patch `patches/ps2recomp/0003-ghidra-export-contiguous-function-ranges.patch`
changes the rule: a function row goes from the entry point to the end of the last range of the
body that starts before `N` (the lowest entry point of another function above it), so small
holes stay inside the function; ranges that start at or after `N`, or lie completely before the
entry point, become label rows. 32 functions have such far ranges (`split_function_count`) and
all of them already had a row (`body_range_label_count = 0`), so the row count did not change;
531 label rows and 14 function rows changed `End`. Result: 0 nested functions, the 7,102
function rows of the main text add up to 2,475,036 bytes (largest 22,352, `FUN_00190580`), validator `E1=0, E2=0, E4=0, E5=0`. Main text coverage went from 97.89 % to
97.36 %: 463 gaps with non-zero words (50,136 bytes) remained, all undefined bytes in the Ghidra
project (**Measured**; closed by T-026, below). A first version of the rule (the row
covers only the range that holds the entry point) was rejected: it cut 1,067 functions at their
first small hole and pushed the labels outside every function from 2,826 to 15,478 (now 2,945).

**Orphan code** (**Measured**, T-026). After T-025, 463 stretches of the main text (50,136
bytes, every word a valid R5900 instruction) were in no row, because Ghidra's analysis never
reached them (461 undefined bytes, 2 partly disassembled), yet 39 of them are pointer targets
and they call other code (13 call targets were found only by accident). The recompiled game can
only jump to addresses of the map, so `tools/funcmap/ghidra/ClaimOrphanCode.java`, run by
`tools/funcmap/ghidra_export.sh` once after the analysis (marker `work/ghidra/.orphans-claimed`),
makes Ghidra claim it: it disassembles from the 39 `new_code` seeds of
`config/extra_entry_points.txt`, then sweeps `[0x100000, 0x3902D4)` and at each non-zero word that
has no instruction disassembles and creates a function (zero words are padding), restricted to
the text range, and runs `analyzeChanges` between rounds. Result: 39 + 831 functions created, 12,341
instructions disassembled, 2 rounds (the second creates nothing); functions in the Ghidra project
7,152 -> 8,098; CSV 56,246 -> 57,789 rows (8,050 functions, 49,739 labels); no function row
lost its `Start`; validator `E1=0, E2=0, E4=0, E5=0` (`E3` only for `entry_003c12a0`), coverage
97.36 % -> 99.13 %, 0 gaps with non-zero words (the 4,383 left are zero padding);
`find_code_pointers.py`: `new_code` 39 -> 0, `inside_row` 203 -> 194. A sweep may make a
function of what is really the unreachable tail of the previous one; that costs only a row.
Running the script again creates nothing.

Three conclusions:

1. **Heuristic mode is not good enough for this game.** It invents 15 functions that start
   outside the code it can tell from data, and generates 35,236 "unhandled instruction" errors,
   all of them beyond `0x381000` (the first estimate of the code end). In the real main text
   there are 0. This conclusion first said that all 15 were false functions inside the data;
   with the layout of T-008 (main text to `0x3902D4`, see "Memory map") that was wrong for 3 of them. The verdict
   for each start address (**Measured**, `tools/funcmap/layout.py` and the words at those
   addresses):

   | Analyzer function | Verdict | Evidence |
   |---|---|---|
   | `sub_00390260` | **Real code**, last function of the main text | CSV `FUN_00390260`, `0x390260`–`0x3902D4`, ends with `jr $ra` |
   | `sub_003BC428` | **Real code** (island, `kernel_patch_blob`) | Start of the blob copied to `0x80075000`; first words `lui $v0, 0x8007` + `addiu` |
   | `sub_003BD260` | **Real code** (island, `kernel_patch_blob`) | Start of the blob copied to `0x80076000`; same `lui` `0x8007` |
   | `sub_003C9890` | False | Data between VU microprograms (gap `0x3C9398`–`0x3CAA6C`); the word does not decode |
   | `sub_00400004` | False | Inside the VU microprogram `0x3FFE1C`–`0x402900` |
   | `sub_004B3640`, `sub_004B4910`, `sub_004B51A8`, `sub_004B51C8`, `sub_004B5DB8`, `sub_004B5DD8`, `sub_004B64C0`, `sub_004B65D8`, `sub_004B67A0` | False | Data after the last VU microprogram (`0x4B2878`); no CSV row starts there |
   | `sub_004C0DE8` | False | Inside the `false_positive` island `0x4BB9A0`–`0x4C9D80`; no CSV row starts there |

   So 3 of the 15 start at real code and 12 are false. Which of the 35,236 errors belong to which
   function was not measured. **Unverified** whether the three real ones produce any of them
   (the blobs of the island contain label-only holes with data, which a heuristic that walks
   the whole span would decode as code).
2. **There are thousands of functions that are only reached through a pointer** (task, state
   and callback tables). Some of the 3,331 will be `switch` labels and not functions; they have
   to be classified (task T-010). If one is missing, the game crashes at run time with "function not found".
3. The recompiler warns about some 1,600 unresolved indirect jumps (`unresolved JR/JALR`),
   consistent with the above.

The analyzer marks 42 instructions as "potential self-modifying code" and proposes to nop them
out (the first version of this document counted only the 7 in the SDK's libcdvd and libmc area).
**Measured** (task T-021): all 42 are false positives, ordinary stores to global variables, and
none is applied. See "Instruction patches proposed by the analyzer" at the end of this document.

## Useful strings as anchors

- Boot: `rom0:ROMVER`, `rom0:UDNL `, `cdrom0:\MODULES\IOPRP300.IMG;1`, `modules/0000.fpk`, `modules/1000.fpk`.
- Data loading: `data.afs`, `bgm.afs`, 150 `fpack/.../NNNN.fpk` paths (3 of them templates
  such as `fpack/story/00%d/%d%d00.fpk`), `host0:data/`, `atfile:%s!8420,/battle/data/`.
- Suffixes the loader appends: `_ps2.bsp`, `_ps2.dff`, `_ps2.txd`, `_cmp.anm`.
- Engine debugging: `CPU%05d GPU%05d TOTAL%05d`, `TASK   : %d`, `MEMORY : %d %d %dk %dk`,
  `ResArena %08x/%08x`, `ClumpsInFrustum`, `SectorsInFrustum`, `EFT_TskParticle`. They point to
  a task system of the game's own on top of RenderWare.
- Sound: `snd/se/ps2/system.dat`, `battle.dat`, `menu*.dat`, `stage01..11.dat`, `char_*.dat` (20 characters).

## How to repeat the measurements

```bash
# Census, code/data boundary and VU microprograms (takes ~1 min)
uv run --no-project --with rabbitizer python -I tools/elf_survey.py disc/SLPS_256.75

# Main text end, islands of code inside the data, and who references them (writes config/layout.toml)
uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv \
    --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml

# Code split by library
uv run --no-project --with rabbitizer python -I tools/layout_survey.py disc/SLPS_256.75 work/analyzer/config.toml

# PS2Recomp analyzer and recompiler in heuristic mode (baseline, do not use for the port)
third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer disc/SLPS_256.75 work/analyzer/config.toml
third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp work/analyzer/config.toml

# Strings
strings -n 6 disc/SLPS_256.75 > work/strings.txt
```

## Instruction patches proposed by the analyzer

Task T-021. Verified against `third_party/PS2Recomp/ps2xAnalyzer/src/elf_analyzer.cpp` and
`work/analyzer/analyzer.log`.

### The rule the analyzer applies

`ElfAnalyzer::tryPatchSelfModifyingStore` (`elf_analyzer.cpp:540-582`) flags an instruction when:

1. it is an `sw` (line 546) and the next instruction is `j` or `jal` (lines 552-555);
2. its address, resolved as `lui` base plus offset, satisfies `isCodeAddress` (lines 563-566);
3. the jump target also satisfies `isCodeAddress` (lines 568-572).

It then sets the patch to `0x00000000` with the reason "Potential self-modifying code"
(lines 579-580). `isCodeAddress` (`elf_analyzer.cpp:649`) returns true for any address inside a
section with `isCode`. This ELF has a single RWX section that mixes code and data (see "Memory
map" above), so every address up to `0x5A6800` counts as code. The rule therefore matches any
`sw` to a global followed by a call. It does not recognise code being written.

### The 7 addresses

All 7 write to a data address (`0x3BDAD4`..`0x3BF690`, above the `0x3902D4` end of the main text)
and none is a code write. "In stubs" means the function is in the `stubs` array of
`work/analyzer/config.toml`.

| Address | Function | Instruction | In stubs? | Decision | Reason |
|---|---|---|---|---|---|
| `0x11a31c` | `sub_0011A2A8` (in libcdvd, unnamed) | `sw $zero, -0x252C($v0)` (`$v0`=`0x3C0000`, writes `0x3BDAD4`), then `jal 0x10C0E0` | no | drop | Clears a global variable of the CD library before a call. Nopping it would leave the variable stale. |
| `0x11a808` | `sceCdLayerSearchFile` | `sw $v0, -0x2524($v1)` (`$v0`=1, writes `0x3BDADC`), then `jal 0x11ACB8` | no | drop | Sets a "busy" style flag to 1 before a call. Dropping it changes the state machine. |
| `0x11b264` | `sceCdInit` | `sw $zero, -0x24FC($v1)` (writes `0x3BDB04`), in the delay slot of `b`, next `jal 0x11A4D0` | yes | drop (moot) | Variable store, and the function is replaced by a runtime handler, so the instruction is never executed. Not applied in the recompiler log. |
| `0x11b310` | `sceCdDiskReady_old` | `sw $v1, -0x2500($a0)` (`$v1`=1, writes `0x3BDB00`), then `jal 0x1163B0` | no | drop | Flag write to a global, applied only when the preceding call returns zero. |
| `0x11b608` | `sceCdDiskReady` | `sw $zero, -0x2500($v1)` (writes `0x3BDB00`), then `jal 0x1163B0` | yes | drop (moot) | Same flag as the previous row; the function is stubbed, so the instruction is not executed. Not applied in the recompiler log. |
| `0x11bde0` | `sceMcInit` | `sw $s0, -0x970($v0)` (writes `0x3BF690`), then `jal 0x10C2B0` | yes | drop (moot) | Stores a semaphore ID returned by the preceding call. Function is stubbed. Not applied in the recompiler log. |
| `0x11dc34` | `sceMcReadFast` (also seen as `mceSifEntry`, the entry sharing the same code) | `sw $v0, -0x978($v1)` (`$v0`=0x16, writes `0x3BF688`), then `jal 0x10C2C0` | no | drop | Stores an error code into the MC library result global before it signals a semaphore. Dropping it loses the result. |

No entry is "doubt". For none of the 7 is there any evidence of code being modified: no target is
in the code area, and every jump target is a normal function entry. The decision is the same
for all: the patch would only remove real stores of game or SDK state. Because the code is
recompiled ahead of time, even a true write to code could never take effect at run time.

`config/instruction_patches.toml` therefore contains an empty `instructions` array (0 patches
kept).

### Inventory of the other analyzer patches

The premise that the array also contains COP0 and CACHE patches is not supported by the
files. `[patches] instructions` in `work/analyzer/config.toml` has **42 entries and all 42 have
the same reason**, "Potential self-modifying code" (the 7 above plus 35 more). There is no COP0
or CACHE entry. `patch_cop0 = true` and `patch_cache = true` (`config.toml:19-20`) are
recompiler options, not entries in that array. `analyzer.log` has 44 "Potential self-modifying"
lines because `0x11dc34` and `0x11dd34` are reported once for each of two overlapping function
names.

The recompiler log applies 39 of the 42. The 3 not applied are exactly the ones inside stubbed
functions: `0x11b264`, `0x11b608`, `0x11bde0`.

Of the other 35, the first 2 are in the SDK (`0x11dd34` in `sceMcWriteFast`/`mceSifEntry`, the same
pattern as `0x11dc34`), the rest are in the game (`0x138e10`, `0x13ca0c`), RenderWare (`0x293750`) and CRI/other
libraries (`0x302c20`..`0x36cf1c`). They all write to addresses `0x3C02B8`..`0x3F2408`, which is
data. Opinion: the same decision (drop) very likely applies to all of them, because they match
the same rule and the same layout pattern. I did not disassemble each one; only the first few
sit in functions already read. The orchestrator can use `config/instruction_patches.toml` as
empty and add an entry only if a specific function is found to be writing real code.
