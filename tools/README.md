# Tools

Everything is run from the repository root, with `uv`:

```bash
uv run --no-project [--with package] python -I tools/<script>.py arguments
```

## Already exist

| Script | Dependencies | What it does |
|---|---|---|
| `elf_survey.py <elf>` | `rabbitizer` | Code/data boundary, instruction census, code pointers in data, VU microprograms |
| `layout_survey.py <elf> [config.toml]` | `rabbitizer` | Which library occupies which area of the code |
| `afs_survey.py <file.afs> [--dump N dir]` | - | Lists an AFS container; exports `read_afs()` |
| `fpk.py` | - | Module for FPK packs: `parse`, `decompress`, `read_entry` |
| `fpk_survey.py <afs or fpk> [-v]` | - | Lists and surveys the files in each FPK |
| `gc_rvz.py <disc.rvz> list \| extract <dir> <patterns>` | `zstandard` | Reads the GameCube disc (RVZ with zstd) |

## To be created with the tasks

| Folder | Purpose | Tasks |
|---|---|---|
| `setup/` | Check the environment; download and patch PS2Recomp, Ghidra and PCSX2 | T-001, T-002, T-005, T-103 |
| `disc/` | Verify and extract the disc | T-004 |
| `funcmap/` | Export, validate and complete the function map | T-006, T-009, T-010 |
| `pipeline/` | Build tools, generate, audit, build | T-003, T-011, T-012, T-014 |
| `run/` | Smoke run and log classification | T-015 |
| `verify/` | Checks and image and audio comparison | T-013, T-115, T-203, T-303 |
| `ref/` | Read PCSX2 savestates and dumps | T-018 |
| `re/` | String references, callers, decompile a function, IOP modules | T-019, T-022, T-023, T-101 |
| `container/` | Docker image to build the PCSX2 pieces | T-104 |

## Rules

- No script writes outside `work/`, `generated/`, `build/`, `third_party/` or the files that
  its task indicates.
- Every new script has tests in `tests/` that do not need the game.
- Scripts that import others from this folder start with
  `sys.path.insert(0, str(Path(__file__).resolve().parent))`, because `python -I` does not add
  the script folder to `sys.path`.
