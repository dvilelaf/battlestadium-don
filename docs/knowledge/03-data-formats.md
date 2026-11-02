# 03 · Data formats

Everything **measured** unless noted. The tools are in `tools/` (see
[09-environment-and-tools.md](09-environment-and-tools.md)).

## Container chain

```
Disc ──▶ DATA.AFS ──▶ 135 FPK packs ──▶ 1,789 files (compressed one by one)
         BGM.AFS  ──▶ 178 ADX tracks
         SE.AFS   ──▶ 38 effect banks ("ps2_DTPK")
         MOVIE.AFS──▶ 2 Sofdec videos (.sfd)
         MODULES/0000.FPK, 1000.FPK ──▶ compressed IRX modules
```

The game opens the packs by name (`fpack/chr/gok/1000.fpk`); the name table is in the
executable and each name maps to an index of `DATA.AFS`. The exact name → index mapping on PS2
is **unverified** (task T-022); on GameCube it is known because the disc carries the list
(`data.h`, see [06-gamecube-version.md](06-gamecube-version.md)).

## AFS (CRI container)

Little endian.

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | `"AFS\0"` |
| 4 | `u32` | Number of entries `N` |
| 8 | `N × {u32 offset, u32 size}` | Entry table (the offsets are aligned to 0x800) |
| 8 + 8N | `u32 offset, u32 size` | Name table: `N` records of 0x30 bytes (`char name[0x20]`, 6 × `u16` of date, `u32`) |

In the PS2 `DATA.AFS` the names are truncated and useless (`char`, `titl`, `view`).

## FPK (Eighting container)

Same format on PS2 (little endian) and GameCube (big endian).

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | 0 |
| 4 | `u32` | Number of files `N` |
| 8 | `u32` | Header size: `0x10` |
| 12 | `u32` | Total size of the pack |
| 0x10 | `N × 0x30` | Entries |

Entry (0x30 bytes): `char name[0x20]`, `u32` (0), `u32 offset`, `u32 packed_size`,
`u32 unpacked_size`. If `packed_size == unpacked_size`, the file is stored uncompressed. There
are 34 empty entries (size 0): 10 `.dff`, 8 `.anm`, 8 `.bone`, 6 `.txd` and 2 `.uva`.

### Compression (solved)

A PRS variant (LZ77 with control bits). The control bits are read **from the most
significant to the least significant** bit of each control byte:

| Bits | Meaning |
|---|---|
| `1` | Literal: copy the next byte |
| `0 0 s1 s0` | Short copy: length `s + 2` (2 to 5), followed by one offset byte (`byte − 256`, that is −256 to −1) |
| `0 1` | Long copy: two bytes `hi`, `lo`. Offset `((hi << 8 \| lo) >> 3) − 0x2000`. Length `(lo & 7) + 2`; if `lo & 7 == 0`, the length is the next byte plus 1 |

**There is no end marker**: decoding stops when the uncompressed size is reached. (In standard
PRS, `hi = lo = 0` ends the stream; here it is a valid copy.)

Verification: the 1,755 non-empty files of `DATA.AFS` decompress to the exact size and the
1,396 that are RenderWare have consistent headers. Implementation: `tools/fpk.py`.

## File types inside `DATA.AFS` (PS2)

| Extension | Files | Total size | What it is |
|---|---|---|---|
| `.txd` | 421 | 54.0 MB | RenderWare texture dictionary, PS2 native (`*_ps2.txd`) |
| `.dff` | 434 | 29.9 MB | RenderWare model, PS2 native (`*_ps2.dff`) |
| `.tmo` | 81 | 22.7 MB | Game-specific. By size and name, probably character animations. **Unverified** |
| `.seq` | 221 | 12.5 MB | Game-specific: scripts for characters (`chr/xxx/0000.seq`), for the CPU (`cpu/xxx/0000.seq`) and for menus |
| `.bsp` | 11 | 2.9 MB | RenderWare world (one stage per file) |
| `.anm` | 448 | 0.8 MB | RenderWare animation |
| `.fld` | 28 | 88 KB | Game-specific: stage data. A `_gc.fld` variant exists |
| `.par` | 28 | 63 KB | Game-specific: particles |
| `.uva` | 108 | 41 KB | RenderWare texture coordinate animation |
| `.ico` | 1 | 59 KB | Memory card icon |
| `.bone` | 8 | <1 KB | Game-specific |

Internal folder structure: `charsel`, `chr/<character>` (20 characters plus `cmn` and
`ace`), `cpu/<character>`, `cmn`, `ending`, `font`, `game`, `mcard`, `menu`, `option`,
`result`, `slot`, `stg/001`…`stg/011`, `title`, `viewer`. The list of which folders
are in each pack is in [data/data_afs_ps2.tsv](data/data_afs_ps2.tsv).

Character codes: `ace`, `buu`, `cel`, `cho`, `fri`, `gaa`, `goh`, `gok`, `kak`, `lee`,
`luf`, `nam`, `nar`, `pic`, `sak`, `san`, `sas`, `tru`, `uso`, `veg`, `zor`.

First bytes of the game-specific formats (to recognize them):

| Extension | Example | First bytes |
|---|---|---|
| `.seq` | `chr/ace/2000.seq` | `08 00 00 00 00 01 00 00 10 00 00 00 00 00 00 00` |
| `.tmo` | `chr/buu/0000.tmo` | `00 02 00 00 00 00 00 00 00 00 00 00 00 00 00 00` |
| `.par` | `chr/ace/2500.par` | `03 00 00 00 …` and later the string `.ptl` |
| `.fld` | `stg/001/0000.fld` | `02 00 00 00 00 00 …` |

None of these formats needs to be understood for stage 1: the recompiled game reads them with
its own code.

## RenderWare files

Each file is a sequence of chunks `{u32 type, u32 size, u32 version_stamp}`.

| Type | Meaning |
|---|---|
| `0x10` | Model (`.dff`) |
| `0x16` | Texture dictionary (`.txd`) |
| `0x0B` | World (`.bsp`) |
| `0x1B` | Animation (`.anm`) |
| `0x2B` | Texture coordinate animation (`.uva`) |
| `0x15` | Native texture (inside a `.txd`) |

Version stamps seen (number of files): `0x1C02000A` (257), `0x1C02002D` (562),
`0x1C020037` (247), `0x1C020055` (292): all are RenderWare **3.7.0.2** with different exporter
build numbers. There are also 35 files with `0x1803FFFF` (3.6.0.3) and 3 with `0x1400FFFF` (3.5).

PS2 textures: almost all are paletted, 8 bits per pixel (32.7 million pixels in total) or
4 bits (14.6 million); 0.1 million at 32 bits. Texture names sometimes keep the source
path (`p:\txdmake\font\Font01.png` on GameCube).

## Sound and video

| File | Entries | Format | Note |
|---|---|---|---|
| `SE.AFS` | 38 | Start with `ps2_DTPK` | Effect banks: `system`, `battle`, `menu*`, `slot`, `stage01..11`, `char_*` |
| `BGM.AFS` | 178 | ADX (CRI) | Music and voices. Identical size on PS2 and GameCube |
| `MOVIE.AFS` | 2 | Sofdec (`.sfd`) | Opening and ending (`OP.sfd`, `ED.sfd` according to the GameCube disc) |

## How to repeat the measurements

```bash
uv run --no-project python -I tools/afs_survey.py disc/DATA.AFS        # list of AFS entries
uv run --no-project python -I tools/fpk_survey.py disc/DATA.AFS -v     # files inside each FPK
uv run --no-project python -I tools/fpk_survey.py disc/MODULES/0000.FPK -v
```

To decompress a specific file from Python: `fpk.parse(buf)` and `fpk.read_entry(buf, entry)`.
