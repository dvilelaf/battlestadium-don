# 01 · The game and its discs

## Identity

| | |
|---|---|
| Title | Battle Stadium D.O.N (バトルスタジアムD.O.N) |
| Year | 2006 (released on 20 July, Japan only) |
| Development | Eighting, with Q Entertainment |
| Publisher | Namco Bandai Games |
| Genre | 4-player arena fighter, with characters from Dragon Ball, One Piece and Naruto |
| Platforms | PlayStation 2 and GameCube |
| Internal project name | `ev0408`, folder `battle` (paths `p:\ev0408\battle\...` on the GameCube disc) |

## Versions and serials

| Version | Serial / ID | Executable |
|---|---|---|
| PS2, Japan | `SLPS-25675` | `SLPS_256.75` |
| GameCube, Japan | `G8SJAF` | `main.dol` |

> Correction: the project's initial prompt wrote the serial as `SLPS-256.75`. The serial
> is `SLPS-25675`; `SLPS_256.75` is the name of the executable file.

## Disc images on this machine

All of them are the user's own copies. **None goes into the repository** (see `.gitignore`).

| What | Path | Bytes | SHA-1 |
|---|---|---|---|
| Japanese PS2 (base of the port) | Outside the repository; its path is in the environment variable `DON_ISO` (file `local.env`, not versioned) | 1,770,487,808 | `d81eff5e209a923f6ee954c2620ccb6738f716e8` |
| Japanese PS2, same image compressed | `~/Descargas/Battle Stadium D.O.N (Japan) [SLPS-25675].7z` | 223,458,996 | (CRC32 of the inner ISO `4879D549`, identical to the one above) |
| English PS2, Japanese voices | `~/Descargas/battle-stadium-d.-o.-n-english-patched/...[Japanese Voices].gz` → `en_patch/jpvoice/disc.iso` | 1,387,528,192 | `033e4af523b5d0eff1416d4301c724d7aeda7c01` |
| English PS2, English voices | `...[English Voices].gz` → `en_patch/envoice/disc.iso` | 1,388,576,768 | `15365d1a8f873e335ce103564f982f72a8fff17b` |
| Japanese GameCube | `~/Descargas/Battle Stadium D.O.N (Japan).zip` → `gc_rvz/*.rvz` | 286,477,800 (RVZ) / 1,459,978,240 (disc) | `36eb38902d8337eab5605dba468128d927dd7ed0` (RVZ) |

## How to tell the three PS2 variants apart

The executable is **identical** in all three (measured): same SHA-256
`994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1`. Only the data changes:

| File | Japanese | English, Japanese voices | English, English voices |
|---|---|---|---|
| `SLPS_256.75` (SHA-256) | `994e8401…80c1` | same | same |
| `DATA.AFS` (SHA-1) | `1c2cdbe5500e5c2328a2ec9f030789156fecc0b5` | `12f174bccbeadc5c89eeb172317d163365904fd2` | `ca29e2ee6d21b9137533d1ddd80547a10133ae0f` |
| `SE.AFS` (SHA-1) | `7639a2ad4a51fad4b8afde41189e7722c9b33adf` | same as Japanese | `cf8e228dc86d190299b5e5dadc7f6b20d9bc1ff1` |
| `BGM.AFS` (SHA-1) | `6b73363ec315b6c507c61479f0574966c5fc31e7` | `f9e4363d9e1acbba78ac198d8755107421496800` | `fbd445d3c4ae61dc2596dc7d13b2830cfdc46e7c` |

Consequence: **a single recompiled build works for all three discs**. The details of the patch
are in [07-english-patch.md](07-english-patch.md).

## Contents of the PS2 disc (Japanese)

Measured with `7z l "<iso>"`. UDF volume `BATTLE`, created on 2006-06-13.

| File | Bytes | What it is |
|---|---|---|
| `SYSTEM.CNF` | 57 | `BOOT2 = cdrom0:\SLPS_256.75;1`, `VER = 1.02`, `VMODE = NTSC` |
| `SLPS_256.75` | 3,973,040 | Main executable (ELF for the main CPU) |
| `DIR.DIR` | 38 | Text list: `data.afs`, `bgm.afs`, `se.afs`, `movie.afs` |
| `DATA.AFS` | 53,217,280 | 135 FPK packs with models, textures, scripts and animations |
| `BGM.AFS` | 98,580,480 | 178 music and voice tracks |
| `SE.AFS` | 6,844,416 | 38 sound effect banks |
| `MOVIE.AFS` | 111,028,224 | 2 videos (opening and ending) |
| `DUMMY.DAT` | 541,665,280 | Padding |
| `MODULES/0000.FPK` | 115,056 | 8 I/O processor modules, compressed |
| `MODULES/1000.FPK` | 262,400 | 9 sound modules, compressed |
| `MODULES/IOPRP300.IMG` | 275,345 | Replacement image of the I/O processor kernel |
| `MODULES/*.IRX` (18 files) | various | The same modules, loose and uncompressed |

The English patched discs add a 1-byte `DUMMY` file. Their `MODULES` folder has the same
21 files with the same sizes; the rest of the structure is the same.

## Extracting what is needed

```bash
# From the repo root, after `set -a; . ./local.env; set +a`. Creates disc/ (ignored by git).
7z x -y -odisc "$DON_ISO" \
    SLPS_256.75 SYSTEM.CNF DIR.DIR MODULES DATA.AFS SE.AFS BGM.AFS
sha256sum disc/SLPS_256.75   # must give 994e8401...80c1
```

`MOVIE.AFS` and `DUMMY.DAT` are not needed for analysis. To run the game, the runtime will read
the whole ISO image (see [05-audio-video-input.md](05-audio-video-input.md)).

## Running from a variant's folder (journal 021, added 2026-10-13)

The port runs from a folder, not from the image: `tools/disc/make_variant_dir.py JP_DIR VARIANT DEST` builds
it (the three data files from the variant, everything else from the Japanese `disc/`), and `tools/launcher/`
(`launch.py`, `launch.sh`, `launch.bat`) picks `jp`, `en-jpvoice` or `en-envoice` and starts the port on it. The three
variants are told apart by the SHA-1 of their `DATA.AFS` (`config/discs.toml`). Details and measurements:
[07-english-patch.md](07-english-patch.md) and [09-environment-and-tools.md](09-environment-and-tools.md).
This replaces the statement above that the runtime reads the whole ISO image: the port needs no image when started this way (**Measured**, journal 021).
