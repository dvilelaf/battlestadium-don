# 06 · The GameCube version as a reference

It is not the basis of the port (see [../DECISIONS.md](../DECISIONS.md)), but it is useful for comparison.
Everything is **measured** on `gc_rvz/Battle Stadium D.O.N (Japan).rvz`.

## Summary

| | |
|---|---|
| ID | `G8SJAF`, internal name `battleGC` |
| Disc size | 1,459,978,240 bytes (RVZ: 286,477,800, zstd compression, 128 KB blocks) |
| Executable | `main.dol`, 2,517,312 bytes, SHA-1 `a5ff312ad398c6f9bd7974092d4dbf4a0b2791d6` |
| Symbols | **None.** There is no `.map` on the disc and no function names in `main.dol` |
| Compiler | Metrowerks CodeWarrior (strings `Metrowerks Target Resident Kernel for PowerPC`) |
| SDK | Dolphin SDK, builds from April and November 2004 |
| Engine | RenderWare as well: `GameCubeAtomicAllInOne.csl`, `GamerCubeWorldSectorAllInOne.csl`, `ImmInstance.csl`, `SubmitNoLight.csl`; assets `_gc.dff`, `_gc.txd`, `_gc.bsp` |
| CRI | `ADXT/GC Ver.9.69`, `CRI SFD/GC Ver.1.976` (November 2005, newer than the PS2 ones) |

## Disc contents (190 files)

| Path | What it is |
|---|---|
| `fpack/**/NNNN.fpk` (173) | The packs, **loose**, with no AFS in between |
| `data.afs`, `bgm.afs`, `se.afs`, `movie.afs` | The same containers as on PS2 |
| `data.h`, `bgm.h`, `se.h`, `movie.h` | Forgotten C headers: one `#define` per file with its index and its original path |
| `*.als`, `*.alg` | Input lists of the tool that built the AFS files |
| `opening.bnr` | Disc icon |

The headers reveal the development paths, for example:

```c
#define P__EV0408_BATTLE_DVD_FPACK_CHR_BUU_1000_FPK  (5)  /* p:\ev0408\battle\dvd\fpack\chr\buu\1000.fpk */
#define P__EV0408_BATTLE_DATA_SND_SE_GC_CHAR_GOKU_DAT (6) /* p:\ev0408\battle\data\snd\se\gc\char_goku.dat */
#define P__EV0408_BATTLE_DATA_SND_MOVIE_GC_OP_SFD    (1)  /* p:\ev0408\battle\data\snd\movie\gc\OP.sfd */
```

These are data names, not code names.

## The executable

| | GameCube | PS2 |
|---|---|---|
| Code | 2.34 MB, in code sections separate from the data | 2.56 MB, mixed with data |
| Direct call targets | 5,089 | 5,011 |
| Code addresses referenced only from data | 3,970 | 3,331 |
| Strings in common | 664 | |

The nearly equal numbers indicate that these are the same sources compiled for two machines.
Practical use: if there is doubt on PS2 about whether something is a function, the equivalent
count on GameCube gives a second opinion.

## Data

| | GameCube | PS2 |
|---|---|---|
| Packs | 173 | 135 |
| Inner files | 1,722 | 1,789 |
| Scripts | `.seg` (218) | `.seq` (221) |
| Particles | `.ptl` (24) | `.par` (28) |
| Other own formats | `.txg`, `.ref`, `.tpl` | `.bone`, `.ico` |

- The `.seg` scripts are **not** the `.seq` files with the bytes swapped: of 95 pairs with the
  same name, only 48 have the same size and, after swapping, on average 36% of the words match.
  They are separately generated binaries.
- Textures: same art and same resolution in 208 of 274 texture dictionaries compared; PS2 has
  more pixels in 59 and GameCube in 7 (47.4 versus 43.5 million pixels). GameCube uses 4-bit
  and 8-bit palettes and, for 3.8 million pixels, lossy compression (`CMPR`).

## What it is for

1. A second opinion on function boundaries (code and data are separate).
2. Names of data files and of the project.
3. If in the future someone wants to name the RenderWare functions: the public RenderWare
   decomps are for GameCube.

## How to read the disc

```bash
uv run --no-project --with zstandard python -I tools/gc_rvz.py "gc_rvz/Battle Stadium D.O.N (Japan).rvz" list
uv run --no-project --with zstandard python -I tools/gc_rvz.py "gc_rvz/Battle Stadium D.O.N (Japan).rvz" \
    extract work/gc/x @dol '*.h' '*.als'
```

`tools/gc_rvz.py` only supports RVZ with zstd and returns zeros in the padding areas.
