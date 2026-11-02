# 10 · PCSX2 savestates (`.p2s`)

What a PCSX2 savestate holds and how `tools/ref/p2s_extract.py` reads it (task T-018).

**Conventions.** **Declared** = read in the pinned PCSX2 sources (v2.8.2, `third_party/pcsx2`,
paths relative to `pcsx2/`, with line numbers). **Measured** = seen in a real savestate on this
machine. Everything under "The tool" and "Container" is **Declared** (read from the sources); the section
"Real savestates" is **Measured** on the six savestates of T-017, and the synthetic-ZIP tests are in
`tests/test_p2s_extract.py`.

**Savestates contain the game's memory.** They live in `work/ref/` only; extracted output goes to
`work/ref/x/`. Nothing extracted is versioned or pasted into reports (counts and addresses are fine).

## Container

| Fact | Value | Source |
|---|---|---|
| Format | ZIP (libzip) | **Declared** `SaveState.cpp:1032-1066` (`SaveState_ZipToDisk`) |
| Default compression of the entries | **Zstandard**, ZIP method 93, level 3 (`Medium`) | **Declared** `Config.h:1336-1337`, `SaveState.cpp:932-944`, `3rdparty/libzip/lib/zip.h:198` |
| Other methods the user can choose | Deflate (levels 1/5/7/9), uncompressed (method 0) | **Declared** `SaveState.cpp:945-961` |
| Entries always stored uncompressed | the version file and `Screenshot.png` | **Declared** `SaveState.cpp:838`, `:997-999` |
| Save version | `g_SaveVersion = 0x9A590000` (major `0x9A59`, minor `0`). A file is loadable if its major equals the build's and its version is not newer | **Declared** `SaveState.h:29`, `SaveState.cpp:1105` |
| Zstandard in Python | `zipfile` cannot read method 93 (it fails on open). The tool parses the ZIP local headers itself | tool |

Order of the entries when writing: version file, `PCSX2 Internal Structures.dat`, then the
components in the order of the table below, then the screenshot (**Declared**
`SaveState.cpp:700-745`, `:963-1030`).

## Entries

All the memory components are raw dumps with the sizes of the build (**Declared**
`MemoryTypes.h`, `VUmicro.h`). Sizes in bytes; "Extracted as" is the fixed name used by the tool.

| ZIP entry | Contents | Size | Extracted as | Source |
|---|---|---|---|---|
| `PCSX2 Savestate Version.id` | `u32` save version + 32-byte NUL-padded PCSX2 tag (e.g. `v2.8.2`); 36 bytes, stored | 36 | (in `manifest.json`) | `SaveState.cpp:339,342,963-1000` |
| `PCSX2 Internal Structures.dat` | BIOS tag, `cpuRegs`, cycles, EE and IOP subsystems (VIF, GIF, DMAC, SIF, timers, CDVD...): PCSX2-dependent structures, not interpreted | variable | `internal.bin` | `SaveState.cpp:140-290`, `:715-725` |
| `eeMemory.bin` | **EE main memory**, indexed by physical address | `0x02000000` (32 MiB; `0x08000000` in devkit mode) | `ee_ram.bin` | `SaveState.cpp:517-530`, `MemoryTypes.h:11`, `Memory.cpp:244` |
| `iopMemory.bin` | **IOP main memory** | `0x00200000` (2 MiB; 8 MiB in devkit mode) | `iop_ram.bin` | `SaveState.cpp:532-541`, `MemoryTypes.h:18,20` |
| `eeHwRegs.bin` | EE hardware registers (`eeHw`) | `0x10000` | `ee_hw.bin` | `SaveState.cpp:543-551`, `MemoryTypes.h:15` |
| `iopHwRegs.bin` | IOP hardware registers (`iopHw`) | `0x10000` | `iop_hw.bin` | `SaveState.cpp:553-561`, `MemoryTypes.h:21` |
| `Scratchpad.bin` | EE scratchpad | `0x4000` (16 KiB) | `scratchpad.bin` | `SaveState.cpp:563-571`, `MemoryTypes.h:16` |
| `vu0Memory.bin` | VU0 data memory | `0x1000` (4 KiB) | `vu0_mem.bin` | `SaveState.cpp:573-581`, `VUmicro.h:10` |
| `vu1Memory.bin` | VU1 data memory | `0x4000` (16 KiB) | `vu1_mem.bin` | `SaveState.cpp:583-591`, `VUmicro.h:12` |
| `vu0MicroMem.bin` | VU0 code memory (microprogram) | `0x1000` (512 instructions) | `vu0_micro.bin` | `SaveState.cpp:593-601`, `VUmicro.h:11` |
| `vu1MicroMem.bin` | VU1 code memory (microprogram) | `0x4000` (2,048 instructions) | `vu1_micro.bin` | `SaveState.cpp:603-611`, `VUmicro.h:13` |
| `SPU2.bin` | SPU2 state (`SPU2freeze`) | variable | `spu2.bin` | `SaveState.cpp:613-621` |
| `USB.bin` | USB state, optional | variable | `usb.bin` | `SaveState.cpp:623-633` |
| `PAD.bin` | Pad state | variable | `pad.bin` | `SaveState.cpp:635-643` |
| `GS.bin` | **GS state** (`GSState::Freeze`): starts with a `u32` state version, then the GS registers and the transfer state (VRAM is expected to follow, **Unverified**: only the first part of `GSState::Freeze` was read); not a plain register dump | variable | `gs.bin` | `SaveState.cpp:645-653`, `GS/GSState.cpp:3557-3620` |
| `Achievements.bin` | RetroAchievements state, optional, usually absent | variable | `achievements.bin` | `SaveState.cpp:655-690` |
| `Screenshot.png` | 640x480 PNG, stored | variable | `screenshot.png` | `SaveState.cpp:339-340,778-843` |

A load requires every entry except `USB.bin` and `Achievements.bin` (**Declared**
`SaveState.cpp:1183-1200`). The tool warns when a required memory entry is missing or has a
different size, and extracts what it knows.

Mapping to the guest (**Measured**: the text range of the executable is byte-identical at offset `0x00100000` in all six): `ee_ram.bin` is indexed by
physical address, so the executable's load segment at `0x00100000` should appear at offset
`0x00100000`; the scratchpad is the guest range `0x70000000-0x70003FFF`; VU1 code memory
holds instructions of 8 bytes, so the instruction index is `offset / 8`.

## The tool

```bash
uv run --no-project python -I tools/ref/p2s_extract.py <state.p2s> work/ref/x/NN
uv run --no-project python -I tools/ref/p2s_extract.py code-pointers work/ref/x/NN
uv run --no-project python -I tools/ref/p2s_extract.py vu1-program work/ref/x/NN
```

| Command | What it does |
|---|---|
| `extract` (default when the first argument is a file) | Writes the components with the fixed names above and `manifest.json` (every ZIP entry with name, size, stored size, ZIP method; every extracted entry is checked against its CRC and size; the save version; the GS state version). Compares the code range of the executable (`[text] start..end` of `config/layout.toml`) with `ee_ram.bin` at the same address and prints the number of differing bytes (a running game does not modify its code, so the expected number is **0**). The comparison is skipped, with a message, if `disc/SLPS_256.75` is not there |
| `code-pointers` | Scans the 4-aligned words of `ee_ram.bin` outside the loaded image of the executable (so low memory, the zero-initialised data, the heap and the stack). A value that is a multiple of 4 inside the text range is a code pointer. Counts them (occurrences and distinct) by class: start of a row of `config/functions.ghidra.csv`, listed in `config/extra_entry_points.txt` or `config/manual_entry_points.txt`, inside a row but not covered, in no row. Of the uncovered ones, those whose previous pair in the executable is `jal`/`jalr`/`bal` and its delay slot are **return addresses** (a real stack holds them) and are counted apart (`--show-returns` lists them); the others are listed with flags (`P` prologue-like, `E` after a `jr $ra`), one address where each was found and its number of occurrences (`--limit`, default 100); `--tsv FILE` writes all of them with the smallest row that contains each |
| `vu1-program` | Parses each of the 141 microprograms of `docs/knowledge/data/vu_programs.tsv` in the executable as a VIF stream of `MPG` blocks (`cmd 0x4A`, `NUM` in bits 16-23 with 0 meaning 256, address in bits 0-15 in **instructions**, followed by `NUM` x 8 bytes) and compares each block with `vu1_micro.bin` at `address x 8`. Reports programs loaded in full, partial ones (at least `--min-fraction` of their instructions equal at their address, default 0.5), the share of the 2,048 instructions equal to some program at the same address and a greedy decomposition (`--min-gain`) |

`vu_addr` in the TSV is an instruction index, not a byte offset (**Measured** with the tool:
the first block of every one of the 141 programs has the MPG address of the TSV and the sum of
the blocks equals `instrs`; there is no warning when the tool runs).

### Packages

| Need | For |
|---|---|
| Python 3.11+ (`tomllib`), standard library | everything except Zstandard |
| Zstandard decoder: the `zstandard` package (`uv run --no-project --with zstandard python -I ...`), or Python 3.14's `compression.zstd`, or the `zstd` command line tool in `PATH` (tried in this order) | real savestates (Zstandard is the PCSX2 default) |
| `rabbitizer` | nothing in this tool (the helper `tools/funcmap/layout.py` only needs it for decoding) |

The tool tells what to install if none of the three decoders exists. The tests need nothing:
the Zstandard cases run when an encoder exists (`zstandard` or the `zstd` tool) and are skipped
otherwise; the "no decoder" message is tested by hiding the decoders.

## Real savestates

**Measured** on the six savestates of T-017 (`01-title`, `02-menu`, `03-charselect`, `04-match-1v1`,
`05-match-4p`, `06-results`; 7.8 to 12.5 MB each), extracted into `work/ref/x/NN` with this tool.

Nothing in the sources turned out to be wrong: entry names, sizes, order, compression and the
version file are as declared above.

| Fact | Measured |
|---|---|
| Entries | 16 in every file: version, internal structures, the 8 memory entries, `SPU2.bin`, `USB.bin`, `PAD.bin`, `GS.bin`, `Screenshot.png`. No `Achievements.bin` |
| Compression | the version file and `Screenshot.png` are stored; the other 14 entries are Zstandard (method 93) |
| Version | `0x9A590000`, tag `v2.8.2` |
| Sizes | `eeMemory.bin` 33,554,432 (32 MiB), `iopMemory.bin` 2,097,152, `eeHwRegs.bin`/`iopHwRegs.bin` 65,536, `Scratchpad.bin` 16,384, `vu0Memory.bin` 4,096, `vu1Memory.bin` 16,384, `vu0MicroMem.bin` 4,096, `vu1MicroMem.bin` 16,384, `SPU2.bin` 2,178,992, `USB.bin` 8,491, `PAD.bin` 228, `GS.bin` 4,194,813 (the same in all six), `Screenshot.png` 415 to 552 KB |
| `GS.bin` | 4,194,813 bytes = 4 MiB (VRAM) + 509 bytes of registers and transfer state; its first `u32` (state version) is 9 |
| `PCSX2 Internal Structures.dat` | varies with the moment: 5,335,274 / 3,909,866 / 8,083,262 / 8,231,786 / 2,657,230 / 351,758 bytes (01 to 06) |

### Executable code against main memory

Range `0x00100000-0x003902D4` (2,687,700 bytes, `config/layout.toml`):

| | 01 | 02 | 03 | 04 | 05 | 06 |
|---|---|---|---|---|---|---|
| Differing bytes in the code range | 0 | 0 | 0 | 0 | 0 | 0 |

The code range is untouched at all six moments. Over the whole file-backed image
(`0x00100000-0x004C9D80`) 6,065 to 6,121 bytes differ (99 to 106 clusters, none below
`0x003BC114`; per 64 KiB: `0x3B` 265, `0x3C` 915, `0x3D` 1,783, `0x3E` 2,384, `0x3F` 618,
`0x4B` 6, `0x4C` 139 in `04`). Those are runtime-modified data and the kernel patch blob area; the
first clusters are shared by all six files. Unverified what each one is.

### Code pointers in memory

`code-pointers` over everything outside the executable's image (7,395,488 words). Values that are
multiples of 4 inside the text range:

| | 01 | 02 | 03 | 04 | 05 | 06 |
|---|---|---|---|---|---|---|
| Occurrences | 18,848 | 18,185 | 18,525 | 27,580 | 37,131 | 21,004 |
| Distinct | 2,354 | 2,326 | 1,864 | 1,933 | 2,012 | 2,059 |
| Start of a row | 668 | 677 | 601 | 629 | 639 | 638 |
| In the entry-point files, not a row start | 12 | 12 | 13 | 15 | 15 | 13 |
| Not covered, inside a row | 1,664 | 1,631 | 1,240 | 1,281 | 1,349 | 1,399 |
| Not covered, in no row | 10 | 6 | 10 | 8 | 9 | 9 |
| of the not covered: return addresses | 108 | 112 | 103 | 76 | 78 | 103 |
| of the not covered: others | 1,566 | 1,525 | 1,147 | 1,213 | 1,280 | 1,305 |
| of the others: entry-like (`P`/`E`) | 11 | 7 | 11 | 12 | 12 | 11 |

Most of the "others" are **not pointers**: the most frequent ones are round integers
(`0x00150000`, `0x00120000`, `0x00110000`... found about 10,000 times each: sizes, strides,
addresses of other things) and 942 of the 2,906 distinct values appear in one savestate only.
The tool adds a heuristic to separate them: flag `P` = `addiu/daddiu $sp,$sp,-N` in the first two
instructions at that address, flag `E` = preceded by `jr $ra` and its delay slot. Union over the six files:
27 values that are not return addresses have `P` or `E`, 22 of them in no row of the function map.
The list is in `work/reports/p2s_code_pointers.tsv` (local). Whether they are real entry points is
**Unverified**: the test is to run the recompiled game and see whether it jumps there.

### VU1 microprograms

`vu1-program`: each of the 141 programs is compared with VU1 code memory at its own address
(the instruction `i` of a block starting at MPG address `a` is at `(a+i)*8`). **Measured**: the
VU1 code memory is full (2,048 non-zero instructions) at every moment, and what is in it is
**a mix**: the last big program uploaded plus overlays of small ones and stale instructions of
earlier larger programs. A program is "loaded in full" only when the memory still holds all of it.

| Moment | Loaded in full | Dominant program (equal instructions / size) | Memory equal to some program at the same address |
|---|---|---|---|
| 01, 02, 03 (same memory in the three) | `0x393D5C` (47 instr, at 0) | `0x47B89C` (1,813 / 1,860, the 47 instructions at 0 are overlaid by `0x393D5C`) | 1,860 (90.8%) |
| 04 match 1v1 | `0x393D5C` | `0x3FFE1C` (1,168 / 1,367); also `0x3C6F7C` 152 / 199, `0x3C7BEC` 126 / 199 | 1,929 (94.2%) |
| 05 match 4p | `0x393D5C` | `0x3FFE1C` (1,320 / 1,367); also `0x3FB66C` 550 / 933 | 1,929 (94.2%) |
| 06 results | `0x47B89C` (1,860 instr, at 0) | same, 1,860 / 1,860 | 1,950 (95.2%) |

Reading: the programs `0x393D5C` (small, at the start of the load segment's microcode),
`0x47B89C` and `0x3FFE1C` are really uploaded by the game. The rest of what the tool lists in its
decomposition (small gains of `0x435FEC`, `0x493C2C`...) is shared code between variants and stale
tails, not proof that those programs ran. That the memory holds a program does not prove it ran
in the last frame. 04 is 85% of `0x3FFE1C`: the rest is not from any of the 141 programs
at those addresses (**Unverified** whether it is a variant that the heuristic detector of
`04-graphics.md` missed, or code patched at run time).

