# 07 · The English patch

Fan translation by **Lord Izen**, published on 7 September 2023, PS2 only. It is distributed
as an already patched ISO in two variants: Japanese voices and English voices.

Everything is **measured**, comparing file by file against the Japanese disc.

## What changes and what does not

| File | Japanese voices | English voices |
|---|---|---|
| `SLPS_256.75` | Identical | Identical |
| `MOVIE.AFS`, `MODULES/*`, `SYSTEM.CNF`, `DIR.DIR` | Same sizes (content of `MOVIE.AFS` not compared) | Same |
| `DATA.AFS` | 51 inner files changed out of 1,789 | 52 changed |
| `SE.AFS` | Identical | 23 of 38 banks changed |
| `BGM.AFS` | Same 178 entries with the same content; the file differs only in its structure | 99 of 178 tracks changed |

Inside `DATA.AFS`, the 51 changes in the Japanese voices variant are:

- **42 texture dictionaries** (`*_ps2.txd`, 41 distinct names): the in-game text is drawn in
  textures. They are `chr/<character>/4000_ps2.txd` for the 20 characters (move names),
  `charsel/0002..0005`, `game/0002,0004,0005`, `slot/0000..0003`, `viewer/0000..0004`,
  `menu/0001`, `option/0000`, `title/0000`, `font/1000` and `cmn/2000`.
- **9 scripts** (`.seq`) with embedded text: `charsel/0000`, `game/g_2d00`,
  `mcard/0000,1000,2000,3000`, `result/0000`, `slot/0000`, `viewer/0000`. Examples of the text:
  ` YES`, ` NO`, `Gallery mode added`. Four of them change size.

The full list is in [data/english_patch_changes.tsv](data/english_patch_changes.tsv).

## Consequences for the port

1. **Language costs nothing on PS2.** The recompiled code depends only on the executable, which
   is the same. The user chooses the language by choosing which ISO to give the program.
2. **Each variant must be tested separately**, because the `.seq` scripts change size and the
   game interprets them. If the recompiled code is faithful, they will work the same as on the
   console, but this is an assumption that must be verified (task T-320).
3. The port must **recognize the three variants** by the hash of `DATA.AFS` (table in
   [01-game-and-discs.md](01-game-and-discs.md)) and reject anything else with a clear message.
4. Bringing the translation to GameCube would require converting 42 texture dictionaries to
   another native format and redoing the 9 scripts by hand (there they are `.seg`, a different
   binary). This is one of the reasons for not using GameCube as the base.

## How to repeat the comparison

```bash
mkdir -p en_patch/jpvoice
zcat "$HOME/Descargas/battle-stadium-d.-o.-n-english-patched/Battle Stadium D.O.N (English 09-07-23)[Japanese Voices].gz" \
    > en_patch/jpvoice/disc.iso
7z x -y -oen_patch/jpvoice/x en_patch/jpvoice/disc.iso SLPS_256.75 DATA.AFS SE.AFS BGM.AFS
sha256sum disc/SLPS_256.75 en_patch/jpvoice/x/SLPS_256.75      # must match
```

The file-by-file comparison is done by opening both `DATA.AFS` files with `tools/afs_survey.py`
and `tools/fpk.py` and comparing the decompressed content of each entry.

## What running the English variants measured (journal 021, 2026-10-13)

- **Measured** (021): `verify_disc.py` recognises `en-jpvoice`, `en-envoice` and `jp`. 24 files compared between the
  variants and the Japanese disc are SHA-1 identical, so the folders made by `tools/disc/make_variant_dir.py` are
  faithful. `SLPS_256.75` is identical in all three.
- **Measured** (this document's author, `ls` on `en_patch/*/x/DATA.AFS`): `DATA.AFS` is 53,284,864 bytes in both
  English variants against 53,217,280 in the Japanese one.
- **Measured** (021): the port reads every game file from the variant's folder. No `.iso` is opened in the routes; a run
  with no image is byte-identical to one with it. The launcher (`tools/launcher/launch.py`) removes `DON_ISO` before
  starting the port.
- **Measured** (021): the title-to-menu transition comes about 60 blanks later in English than in Japanese.
- **Measured** (021): the English main menu frame has 66 host-to-local uploads (7 strips of 512x64 PSMCT32), the Japanese
  one 0; the vertex count is the same (8,545). The English menu was drawn garbled until patch 0042, which fixed the
  path-3 upload order ([04-graphics.md](04-graphics.md), "The path of a 3D object").
- **Declared** (021): the games run headless in both English variants and classify as `OK_VSYNC_LOOP`.
- The tool that builds each variant's folder is `tools/disc/make_variant_dir.py` (tests: `tests/test_make_variant_dir.py`);
  the launcher is `tools/launcher/` ([09-environment-and-tools.md](09-environment-and-tools.md)).
