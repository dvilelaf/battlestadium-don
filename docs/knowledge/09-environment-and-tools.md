# 09 · Environment and tools

## This machine (measured on 2026-10-11)

| | |
|---|---|
| System | Ubuntu 22.04, Linux 6.8, x86-64 with AVX2 |
| CPU / RAM | 16 threads, 30 GB |
| GPU | AMD Radeon RX 6800 XT, Vulkan 1.4 (`radv` driver) |
| Disk | ~1.2 TB free on the volume that holds the repository |

| Tool | Status |
|---|---|
| `git`, `cmake` 3.22.1, `ninja`, `make` | Installed |
| `gcc`/`g++` 11.4.0 | Installed. It is the only compiler on the host (**Measured**: `command -v clang` prints nothing; there is **no** `g++-12` or `g++-13`). (changed on 2026-10-11: the build container has clang 17.0.6, see the pipeline; the host is unchanged) |
| Java | OpenJDK 21 (enough for Ghidra 12) |
| `7z`, `zstd`, `xvfb-run`, `vulkaninfo`, `pkg-config`, `wine` | Installed |
| `uv` 0.5.29 | Installed. System Python: **do not use directly** (see below) |
| `gh` | Installed and authenticated with the owner's GitHub account |
| `cargo`/`rustc` | Installed |
| Docker 29.8 | Installed and usable without `sudo` (the user is in the `docker` group). (changed on 2026-10-11: the image `don-build:1` is built on it, 1.01 GB; **Measured**: `docker images don-build`) |
| Wine 6.0.3 | Installed. Used to smoke-test the Windows build (decision D13) |
| Ghidra 12.1.4 | (changed on 2026-10-11: this row said it was absent) **Installed** under `third_party/ghidra/`, with the PS2 extension. **Measured**: `ls third_party/ghidra` shows `ghidra_12.1.4_PUBLIC/` and both zip archives; the extension is in `ghidra_12.1.4_PUBLIC/Ghidra/Extensions/ghidra-emotionengine-reloaded`; `cat config/ghidra_language.txt` prints `r5900:LE:32:default` |
| SDL2/SDL3 development files, `glslc`, FFmpeg development files | Absent from the host (no package installed). **Measured**: `pkg-config --modversion sdl3` and `libavcodec` find nothing; `command -v glslc` prints nothing. (changed on 2026-10-11: inside the image `don-build:1` there are SDL3 3.4.12 and clang 17.0.6, see the pipeline) |
| PCSX2, Dolphin | Absent from the system (no program installed). **Measured**: `command -v pcsx2` and `command -v dolphin-emu` print nothing. (changed on 2026-10-11: a portable PCSX2 v2.8.2 for reference captures is in `third_party/pcsx2-ref/`, and the PCSX2 sources at the pinned commit are in `third_party/pcsx2/`; both are inside the repository and not installed. **Measured**: `git -C third_party/pcsx2 rev-parse HEAD` gives `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`; `ls third_party/pcsx2-ref` shows the AppImage and a `PCSX2/` folder) |
| PS2 BIOS | Two Japanese BIOS files (`ps2-0210j-20040917.bin`, `ps2-0200j-20040614.bin`, 4,194,304 bytes each) in a folder outside the repository (`DON_PS2_BIOS_DIR` in `local.env`). Extracted on 2026-10-11 from a zip supplied by the owner. Only used to run PCSX2 as a reference (T-024); the port itself needs no BIOS. **Measured** (T-017, 2026-10-11): with `ps2-0210j-20040917.bin` the reference PCSX2 2.8.2 shows only a black picture (emulator running at full speed, with or without fast boot, with a fresh NVRAM file, input ignored); with `ps2-0200j-20040614.bin` it boots the game. Both scripts under `tools/ref/` default to the 0200j file. The hashes of the two files were not checked against known dumps (**Unverified**), so a bad copy of the 0210j is possible |

`sudo` has not been used for anything. Everything installed by this project lives inside the repo
(`third_party/`, `build/`, ignored by git). (changed on 2026-10-11: the one exception is the Docker
image `don-build:1`, which is in Docker's own storage, outside the repository.)

## Execution rules on this machine

1. **Python always with `uv`.** A hook rejects bare `python`/`python3`. Standard form:

   ```bash
   uv run --no-project python -I script.py args                       # no dependencies
   uv run --no-project --with rabbitizer python -I script.py args     # with dependencies
   ```

   The `-I` option isolates the interpreter (it does not load modules from the current
   directory). Side effect: it also does not add the script folder to `sys.path`; the `tools/`
   scripts that import others solve this with `sys.path.insert(0, script_folder)`.
2. **The game files are untrusted data**: they are extracted into their own folders (`disc/`,
   `en_patch/`, `gc_rvz/`) and the scripts live apart, in `tools/`.
3. **Nothing derived from the game is versioned**: see `.gitignore` and `AGENTS.md`.
4. **Builds that need the PCSX2 dependencies run inside the image**: `bash tools/container/run.sh <command>`
   (the repository is mounted at the same path). The host is not changed for them. (added on
   2026-10-11, T-104)
5. **Memory and parallel builds** (added on 2026-10-13, after a freeze). At 01:40 on 2026-10-13 this
   machine (30 GiB, 32 GB) froze and had to be reset: three agents were building at once and the build
   container had no memory limit, so the machine died instead of one compiler. Two rules now:
   * `tools/container/run.sh` runs the container with `--memory` and `--memory-swap` set to the same
     value (no extra swap): `DON_CONTAINER_MEMORY` (for example `16g`), default **55 % of MemTotal
     rounded down to whole GiB** (16 GiB on this machine), `DON_CONTAINER_MEMORY=none` turns it off. A
     build that needs more dies inside the container with a killed compiler, visibly, and the host stays
     alive. The same script holds **one run at a time**: an exclusive `flock` on `build/.container.lock`
     held *inside the container* (so a killed docker client cannot release it while the build continues),
     with a host-side probe that prints `waiting for another container run to finish` once.
   * `tools/pipeline/build.sh` prints, as the last command of the same container run, the container's
     peak memory (`/sys/fs/cgroup/memory.peak`).

   **Measured on 2026-10-13** (T-207): a full `linux-dev` build from nothing, `DON_JOBS=8`, peaked at
   **6,938,214,400 bytes (6.46 GiB)** and took 8m19s. Rule for `DON_JOBS`: the largest of 6, 8, 10 or 12
   whose estimated peak (peak at 8 jobs × jobs/8) stays under 80 % of the default limit (80 % of 16 GiB =
   12.8 GiB): 6 → 4.9, 8 → 6.5, 10 → 8.1, **12 → 9.7 GiB**, so the default stays 12; on a shared machine 8
   is the measured-safe value. **Never two full builds at once on a 32 GB machine.** The lock above makes
   a second *container* run wait; these do not go through it and are listed here so they are not forgotten:
   `tools/pipeline/build_tools.sh` (cmake on the host, `-j $(nproc)`), `tools/container/build-deps.sh`
   (builds the image's dependencies, `--parallel $(nproc)`) and `tools/container/run_windows.sh`
   (`docker run` directly, no limit, no lock).

## PS2Recomp: verified commands

Clone in `third_party/PS2Recomp`, commit `2c5fbb9389e11dd95693385969490c9e8e6f57b4`, with the
patches from `patches/ps2recomp/series` applied. (changed on 2026-10-11: the series lists three
patches, `0001` GCC 11, `0002` `sqrt.s` operands and `0003` Ghidra export ranges. **Measured**:
`cat patches/ps2recomp/series`. This note only records the list of patches; the commands below are unchanged.)

Tools (analyzer and recompiler), ~2 minutes. (changed on 2026-10-11: `bash tools/pipeline/build_tools.sh`
runs these commands.)

```bash
cmake -S third_party/PS2Recomp -B third_party/PS2Recomp/out/build-tools -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build third_party/PS2Recomp/out/build-tools -j 14
# Result: out/build-tools/ps2xAnalyzer/ps2_analyzer and out/build-tools/ps2xRecomp/ps2_recomp
```

Runtime without game code (checks that it builds), ~5 minutes:

```bash
cmake -S third_party/PS2Recomp -B third_party/PS2Recomp/out/build-rt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
  -DPS2X_ENABLE_FFMPEG=OFF \
  -DCMAKE_CXX_FLAGS=-march=x86-64-v3 -DCMAKE_C_FLAGS=-march=x86-64-v3
cmake --build third_party/PS2Recomp/out/build-rt -j 12
# Result: out/build-rt/ps2xRuntime/ps2EntryRunner (38 MB)
```

Three stumbling blocks already solved:

| Error | Cause | Fix |
|---|---|---|
| `'std::pair<_T1, _T2>::second' has incomplete type` when building `ps2_recomp` | The GCC 11 libstdc++ requires the complete `Symbol` type in an `unordered_map` | Patch `patches/ps2recomp/0001-recomp-gcc11-symbol-complete-type.patch` (one `#include`) |
| `No package 'libavcodec' found` when configuring the runtime | FFmpeg development files not installed | `-DPS2X_ENABLE_FFMPEG=OFF` |
| `inlining failed in call to 'always_inline' '_mm_extract_epi64': target specific option mismatch` | The runtime uses SSE4.1 and does not pass the option to the compiler | `-DCMAKE_CXX_FLAGS=-march=x86-64-v3` (also for C) |

Runtime messages worth recognizing in the logs:

| Message | Meaning |
|---|---|
| `Error: No exact recompiled function for guest PC 0x...` | Execution jumped to an address that is not in the function map |
| `Unimplemented PS2 syscall` / `Warning: Unimplemented PS2 syscall called. PC=0x...` | Unimplemented system call |
| `[Syscall TODO]` | System call recognized but not implemented |
| `[IOP/RPC trace:unhandled]` | Remote call to the IOP that nobody handles |
| `TODO_NAMED("name", ...)` in the runtime code | SDK function bound to an empty handler (for example `sceSdTransToIOP`) |

The upstream runtime has **no** headless mode, automatic screenshots or scripted input.
`xvfb-run` is available to run it without a real display.

## Repo scripts (`tools/`)

Every script that exists in `tools/` on 2026-10-11 (**38 files**: 6 at the root of `tools/`, 3 in
`container/`, 2 in `disc/`, 4 in `funcmap/` plus 1 in its `ghidra/` folder, 6 in `pipeline/`, 5 in `re/`
plus 1 in its `ghidra/` folder, 3 in `ref/`, 5 in `setup/`, 2 in `verify/`). Each row takes "what it
does" from the script's own header comment. Run them from the repository root.

"Created by" is the task card whose "Output" names the script (`docs/tasks/F0-foundations.md`,
`docs/tasks/M1-menu.md`). "evaluation" means it was written before the task cards, during the
evaluation listed in `docs/STATUS.md`.

Scripts under `tools/container/` marked **(in the image)** only run inside the build image: `build-deps.sh`
is called by the `Dockerfile` while the image is built. The others run on the host.

### Root of `tools/` (analysis of the original executable and disc)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/afs_survey.py` | evaluation (tests in T-007) | Lists an AFS archive (CRI) and fingerprints each entry by its leading bytes; exports `read_afs()` | `uv run --no-project python -I tools/afs_survey.py <file.afs> [--dump N outdir]` |
| `tools/elf_survey.py` | evaluation | Surveys a stripped PS2 ELF: code/data boundary, instruction classes, VIF/VU microcode blobs. Needs rabbitizer | `uv run --no-project --with rabbitizer python -I tools/elf_survey.py <elf>` |
| `tools/fpk.py` | evaluation (tests in T-007, `tests/test_fpk.py`) | Eighting FPK packs: parses (PS2 little-endian or GameCube big-endian) and decompresses entries. It is a module | none: imported; `parse(buf)`, `decompress(src, size)`, `read_entry(buf, entry)` |
| `tools/fpk_survey.py` | evaluation | Lists the Eighting FPK packs inside an AFS (or a standalone FPK): inner names, sizes, extension census | `uv run --no-project python -I tools/fpk_survey.py <file.afs or file.fpk> [-v]` |
| `tools/gc_rvz.py` | evaluation (used in `06-gamecube-version.md`) | Reads a GameCube disc stored as RVZ (zstd only): lists the file system and extracts files. Needs zstandard | `uv run --no-project --with zstandard python -I tools/gc_rvz.py <disc.rvz> list` (or `extract <outdir> <pattern>...`) |
| `tools/layout_survey.py` | evaluation (its algorithm is generalised by `tools/re/strxref.py`, T-019) | Rough map of which middleware owns which part of the text, from string and constant anchors. Needs rabbitizer (through `elf_survey.py`) | `uv run --no-project --with rabbitizer python -I tools/layout_survey.py <elf> [analyzer_config.toml]` |

### `tools/container/` (build image `don-build:1`, T-104)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/container/build-deps.sh` **(in the image)** | T-104 (no card names it; the `Dockerfile` calls it) | Builds one PCSX2 dependency from source into `/opt/deps`. Runs inside the image build only | `build-deps.sh <name>` (called from the `Dockerfile`, not by hand) |
| `tools/container/build_image.sh` | T-104 | Builds the build image `don-build:1` from `tools/container/Dockerfile`, with the PCSX2 dependency patches copied from `third_party/pcsx2` | `bash tools/container/build_image.sh` (needs `tools/setup/fetch_pcsx2.sh` first) |
| `tools/container/run.sh` | T-104 | Runs a command inside the image; the repository is mounted at the same absolute path and the command runs as the invoking user | `bash tools/container/run.sh <command...>` |

### `tools/disc/` (the disc, T-004)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/disc/verify_disc.py` | T-004 | Verifies that a disc image is one of the known variants, by streaming its files out with `7z x -so`. Exit 0 known, 1 error, 2 not a known variant | `uv run --no-project python -I tools/disc/verify_disc.py <iso>` |
| `tools/disc/extract.sh` | T-004 | Verifies a disc image and, if it is a known variant, extracts the files the port needs | `bash tools/disc/extract.sh <iso> [destination=disc]` |

### `tools/funcmap/` (function map, T-006 to T-010, T-025, T-026)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/funcmap/ghidra_export.sh` | T-006 (claim step T-026) | Runs Ghidra headless on the executable: import and analysis, the claim of the code that no function owns, and the export of the function map. Without `--force` it reuses an analyzed project | `bash tools/funcmap/ghidra_export.sh [--force]` |
| `tools/funcmap/ghidra/ClaimOrphanCode.java` | T-026 | Makes Ghidra claim the code of the main text that no function owns. It is a Ghidra script; it changes the program | run by `ghidra_export.sh`: `-postScript ClaimOrphanCode.java <text start> <text end> [seeds file]` |
| `tools/funcmap/layout.py` | T-008 | Pins the code layout: the end of the main text and the islands of code inside the data; classifies each island. Needs rabbitizer | `uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml` |
| `tools/funcmap/find_code_pointers.py` | T-010 | Finds every address of the main text that the game can hold as a code pointer and classifies it (row start, inside a row, new code, rejected). Needs rabbitizer | `uv run --no-project --with rabbitizer python -I tools/funcmap/find_code_pointers.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv` |
| `tools/funcmap/validate.py` | T-009 | Validates the function map against the layout and the executable (checks E1 to E5); errors are accumulated, nothing is modified. Needs rabbitizer | `uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 [--extra config/extra_entry_points.txt]` |

### `tools/pipeline/` (translation and build)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/pipeline/build_tools.sh` | T-003 | Builds the PS2Recomp host tools (`ps2xAnalyzer` and `ps2xRecomp`) from the patched checkout; the runtime and the studio are off | `bash tools/pipeline/build_tools.sh` |
| `tools/pipeline/make_config.py` | T-011 | Assembles `work/don.toml` (the recompiler configuration) and the filtered function map, from the Ghidra map, the layout, the stub policy and the analyzer output. Writes nothing in `generated/` | `uv run --no-project python -I tools/pipeline/make_config.py [--csv ...] [--layout ...] [--out work/don.toml]` (defaults in the header) |
| `tools/pipeline/generate.sh` | T-012 | Runs the translation chain: the analyzer (only if its config is missing), `make_config.py`, the recompiler and the audit of its log | `bash tools/pipeline/generate.sh` |
| `tools/pipeline/audit.py` | T-012 | Summarizes the recompiler log and decides whether the translation is clean; writes the summary as JSON; exits 1 on unhandled instructions in the text or a failed run. Run by `generate.sh` | `uv run --no-project python -I tools/pipeline/audit.py [--log work/logs/recomp.log] [--layout config/layout.toml] [--out generated] [--json work/reports/translation_audit.json]` |
| `tools/pipeline/build.sh` | T-014, T-110 | Configures (only when the build folder does not exist) and builds the `don` executable inside the build container with clang 17 and lld, then copies `libSDL3.so.0` next to it and sets its run path so that the GS library in `build/gs` is found. `don` is the generated code, `src/game/don_overrides.cpp`, the runtime without raylib and the host program of `src/host`. Never run `cmake` for these folders on the host | `bash tools/pipeline/build.sh [linux-dev\|linux-release]` (default `linux-release`; `linux-dev` for the edit-and-run loop) |
| `tools/pipeline/build_gs.sh` | T-105 | Builds `libdon_gs.so` (the PCSX2 GS behind a C API) inside the build container; the result goes to `build/gs/` | `bash tools/pipeline/build_gs.sh` |

Added 2026-10-13 (from each script's header comment; **Declared**, not run for this document):

| Script | What it does (its header) |
|---|---|
| `tools/pipeline/build_vu.sh` | Builds `libdon_vu.so` (the PCSX2 microVU behind a C API) and `vu_selftest` inside the build container |
| `tools/pipeline/build_spu2.sh` | Builds `libdon_spu2.so` (the PCSX2 SPU2 behind a C API) and `spu2_selftest` inside the build container |
| `tools/pipeline/build_host.sh` | Builds the host program's synthetic test (`src/host` plus a synthetic guest) inside the build container |
| `tools/pipeline/copy_safe.sh` | A file to be sourced, not run: `copy_safe <src> <dst>` and `patchelf_safe <file> <rpath>` copy beside the target and rename over it, and do nothing when the files are already identical (journal 018, see "Memory and parallel builds") |

**Rule from journal 018 (T-304):** a build never writes in place into a file a running program may have
mapped. `cp -f` onto a library or executable that a process has mapped truncates the file its pages come
from, and the next access faults in the new bytes; that was the crash at `0x66F80` in a run that was taking a
screenshot while a build copied `libSDL3.so.0` over itself. `build.sh` and the other build scripts use
`copy_safe`/`patchelf_safe` for that reason.

### `tools/re/` (reverse engineering, T-019 to T-023, T-101, T-022)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/re/strxref.py` | T-019 | Finds the code that builds the address of a string: `lui` plus `addiu`/`ori` pairs that point into data. Needs no rabbitizer | `uv run --no-project python -I tools/re/strxref.py --elf <elf> --text-end 0x3902D4 [--csv config/functions.ghidra.csv] <regular expression>` |
| `tools/re/callers.py` | T-023 | Lists the direct callers (`jal`) of a function, with the function that contains each call | `uv run --no-project python -I tools/re/callers.py --elf <elf> --target 0xADDR [--layout config/layout.toml] [--csv config/functions.ghidra.csv]` |
| `tools/re/decompile.sh` | T-023 | Prints the decompiled C and the disassembly of the functions that contain the given addresses. Opens `work/ghidra` read-only | `bash tools/re/decompile.sh 0xADDR [0xADDR...]` |
| `tools/re/ghidra/DecompileAt.java` | T-023 | Ghidra script: prints the decompiled C and the disassembly of the function that contains each address argument. Run by `decompile.sh` | `-postScript DecompileAt.java 0x001011D8 [0x...]` (headless) |
| `tools/re/irx_imports.py` | T-101 | Inventory of the imports and exports of the IOP modules (IRX) inside FPK packages; nothing is written to disk | `uv run --no-project python -I tools/re/irx_imports.py <package.FPK> [...] [--tsv]` |
| `tools/re/pack_names.py` | T-022 | Proposes the mapping from the FPK pack names of the executable to the indices of `DATA.AFS` (two hypotheses are tested) | `uv run --no-project python -I tools/re/pack_names.py` |

### `tools/ref/` (reference captures with PCSX2, T-017 and T-024)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/ref/run_pcsx2.sh` | T-024 | Starts the reference PCSX2 with the game, in portable mode and with the configuration for reference captures; screenshots, savestates and GS dumps go to `work/ref/pcsx2/raw/` | `DON_PS2_BIOS_DIR=<folder with the PS2 BIOS> bash tools/ref/run_pcsx2.sh [iso]` |
| `tools/ref/pcsx2_auto.sh` | T-017 | Starts and stops the reference PCSX2 on its own virtual display (Xvfb, Mesa's software Vulkan), for captures without a person; never uses the user's display | `bash tools/ref/pcsx2_auto.sh start\|stop\|status` |
| `tools/ref/drive.py` | T-017 | Sends PS2 buttons to that emulator, waits for stored screens, takes the captures and checks each dump by replaying it; `route` plays a route file (`tools/ref/route.txt`: from power-on to the results screen, about 10 to 16 minutes) | `uv run --no-project --with pillow --with numpy python -I tools/ref/drive.py route tools/ref/route.txt` |
| `tools/ref/name_capture.sh` | T-024 | Moves the newest screenshot, savestate and GS dump from `raw/` to a name; fails and moves nothing if one is missing | `bash tools/ref/name_capture.sh <name>` |
| `tools/ref/check_captures.sh` | T-024 | Lists the 18 expected reference files (6 moments by 3 kinds) as present or missing; exit 0 only if all are present | `bash tools/ref/check_captures.sh` |

### `tools/setup/` (fetching and checking, T-001, T-002, T-005, T-024, T-103)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/setup/check_env.sh` | T-001 | Checks that the tools and hardware are available; one `OK` or `MISSING` line per check; installs nothing | `bash tools/setup/check_env.sh` (extra tools: `CHECK_ENV_EXTRA="tool1 tool2"`) |
| `tools/setup/fetch_ps2recomp.sh` | T-002 | Clones PS2Recomp at the pinned commit and applies the patches in `patches/ps2recomp/series`; safe to run again | `bash tools/setup/fetch_ps2recomp.sh [--reset]` |
| `tools/setup/fetch_ghidra.sh` | T-005 | Downloads Ghidra and the PS2 extension into `third_party/ghidra/`, checks both archives against `tools/setup/ghidra.sha256`, and writes the EE language id into `config/ghidra_language.txt` | `bash tools/setup/fetch_ghidra.sh` |
| `tools/setup/fetch_pcsx2.sh` | T-103 | Fetches PCSX2 at the pinned commit and applies the patches in `patches/pcsx2/series` (empty series is valid); safe to run again | `bash tools/setup/fetch_pcsx2.sh [--reset]` |
| `tools/setup/build_win_deps.sh` | T-401 | Builds the Windows dependencies (SDL3, zlib, libpng, ... shaderc) from pinned sources with clang-cl in the Windows image; pins at the top of the file | `bash tools/setup/build_win_deps.sh [--prefix DIR]` |
| `tools/pipeline/build_windows.sh` | T-401 | Configures the preset `windows-release` and builds the runtime, host library and the SPU2, VU and GS DLLs for Windows | `bash tools/pipeline/build_windows.sh [steps]` |
| `tools/setup/fetch_pcsx2_ref.sh` | T-024 | Downloads the PCSX2 v2.8.2 AppImage into `third_party/pcsx2-ref/` (reference emulator only); records its SHA-256 in `tools/setup/pcsx2_ref.sha256` on the first download and checks it on every run | `bash tools/setup/fetch_pcsx2_ref.sh` |

### `tools/verify/` (checks of the output)

| Script | Created by | What it does (its header) | Command (its usage line) |
|---|---|---|---|
| `tools/verify/check_sqrt.py` | T-013 | Checks that the generated code computes each `sqrt.s` of the ELF from the register it names (fixed by PS2Recomp patch `0002`) | `uv run --no-project python -I tools/verify/check_sqrt.py --elf disc/SLPS_256.75 --layout config/layout.toml --generated generated` |
| `tools/verify/image_parity.py` | T-115 | Compares two images with SSIM and mean absolute difference; exit 0 when SSIM is at least `--min-ssim` | `uv run --no-project --with pillow --with numpy --with scikit-image python -I tools/verify/image_parity.py a.png b.png [--min-ssim 0.90] [--crop x,y,w,h] [--diff out.png]` |

Added 2026-10-13 (from each script's header comment; **Declared**):

| Script | What it does (its header) |
|---|---|
| `tools/verify/dump_regs.py` | Prints the privileged GS registers of a PCSX2 GS dump at every vertical blank (`--decode` names the fields); standard library only |
| `tools/verify/gif_stats.py` | Describes the GIF packets of one frame (T-202): from a GS dump or from the port's own `DON_GS_PACKETS` capture; per tag: primitive, vertex count, texture, bounding box. `--tags`, `--frame N`, `--json FILE` |
| `tools/verify/profile_summary.py` | Turns a profile written with `DON_PROFILE=<file>` (T-203) into subsystem shares, top symbols and top recompiled game functions; uses `nm` (or `$DON_NM`) |
| `tools/verify/ram_diff.py` | Compares two guest memory images word by word (T-016); `--kind code` keeps code-pointer differences; ours is written with `DON_DUMP_RAM` |
| `tools/verify/wav_stats.py` | Duration, RMS, silent fraction and saturated fraction of a WAV (T-303); exits 1 over `--max-silence` / `--max-clip` |
| `tools/verify/replay_all.sh` | Replays every reference GS dump through our bridge (`gsreplay`) and through PCSX2's `pcsx2-gsrunner` with the same renderer, and compares the two images (`--renderers`, `--min-ssim 0.98`) |

### `tools/run/` (running the port; journals 004, 010, 022)

| Script | What it does (its header; **Declared**) |
|---|---|
| `tools/run/smoke.sh` | Starts `$DON_BUILD_DIR/don` (default `build/linux-dev`) headless for N seconds (default 60), keeps the output in `work/logs/` and classifies it; exit code is that of the run |
| `tools/run/classify_log.py` | Classifies a smoke log into `OK_VSYNC_LOOP`, `MISSING_FUNCTION`, `CRASH`, `NO_PROGRESS` and others; writes `<log>.json`; exit 0 only for `OK_VSYNC_LOOP` |
| `tools/run/gdb_trace.sh` | Runs the port under headless gdb with a breakpoint on every runtime function matching each pattern, prints short backtraces, and at the time limit the last events and every thread's backtrace; output in `work/logs/gdbtrace-<date>.log` |
| `tools/run/headless_desktop.sh [--size WxH] [--rate HZ] [--x11] -- CMD...` | T-305: a private headless desktop (GNOME Shell 42 as a Wayland compositor with one virtual monitor, on the real graphics card) and CMD run in it, then everything is shut down. DISPLAY and the caller's session variables are removed; own D-Bus session (`dbus-run-session`), own `XDG_RUNTIME_DIR` under /tmp (mode 700), GSettings in memory, so nothing touches the user's desktop (and the game finds no sound device inside: the audio path of the SPU2 is off there). The monitor's rate is honoured up to 60 Hz; above that the compositor repaints on each commit without quantizing to the refresh, so 75 or 144 Hz displays are not simulated. `tools/verify/wayland_present.py LOG` reads a `WAYLAND_DEBUG=1` log (commit, buffer-release and frame-callback gaps) |
| `tools/run/scripts/to_main_menu.txt` | Input script: power-on to the main menu (format in `docs/INPUT_SCRIPTS.md`; frames are presented frames) |
| `tools/run/scripts/to_match_1v1.txt` | Input script: power-on to the first single-player match (Naruto vs Com); starts with the route above |
| `tools/run/scripts/answer_memcard.txt` | Input script (T-112): answers the memory-card dialog and watches what the game does next |

The timing of these routes assumes the videos play; journals 023 and 024 skip them, and the save and load routes of journal 022 depend on fixed frame counts, so they need retiming (**Unverified**: not re-run here).

### `tools/launcher/` and `tools/disc/make_variant_dir.py` (disc variants; journal 021)

| Script | What it does (its header; **Declared**) |
|---|---|
| `tools/disc/make_variant_dir.py` | `JP_DIR VARIANT DEST [--copy]`: builds the folder the port runs from for one disc variant. The three data files (`DATA.AFS`, `BGM.AFS`, `SE.AFS`) come from VARIANT, everything else from the Japanese folder; symbolic links where possible; the variant is recognised by the SHA-1 of `DATA.AFS` against `config/discs.toml`; refuses a DEST it did not make. Tests: `tests/test_make_variant_dir.py` |
| `tools/launcher/launch.py` | Launcher and language selector (Tk picker with a terminal fallback; adapted from the owner's `f7gs` project, commit `539b594`); variants `jp`, `en-jpvoice`, `en-envoice`; `--print`, `--list`, arguments after `--` go to the port unchanged |
| `tools/launcher/launch.sh`, `launch.bat` | Wrappers for Linux/macOS (uses `uv`, or `$DON_PYTHON`) and Windows |

### `tests/conformance/` (journal 025)

Instruction conformance tests of the recompiled code against a reference, patches 0043 and 0044: named here by number and title only (journal 025 was still being written when this was updated).

Outputs of the measurements already made (in `work/`, ignored by git, regenerable):

| File | Content |
|---|---|
| `work/strings.txt` | Strings of the ELF |
| `work/elf_survey.txt` | Output of `elf_survey.py` |
| `work/analyzer/config.toml`, `analyzer.log` | Analyzer in heuristic mode: 575 SDK functions with name and address |
| `work/analyzer/recomp.log`, `work/analyzer/output/` | Baseline recompilation (296 MB of C++, 5,838 files). (changed on 2026-10-11: the pipeline now writes `generated/` and `work/logs/recomp.log` instead, see below) |
| `work/data_afs_fpk.txt` | Full listing of the 1,789 files of `DATA.AFS` |
| `work/gc/` | Listing of the GameCube disc, `main.dol`, headers, strings |

## The pipeline, in order

From a clean checkout to a built `don` executable and a built `libdon_gs.so`. Each line is one
command, from the task that defined it (the cards T-001 to T-014, T-025, T-026 and T-103 to T-105),
and what it leaves. Step 9 is in progress: the script exists, the executable does not yet.
Nothing here was run to write this section; the outputs marked **Measured** were seen on disk.

Before step 1: the host tools in "This machine" (`uv`, `cmake`, `ninja`, Docker, `gh`), and the ISO of
the Japanese disc (its path is in `docs/knowledge/01-game-and-discs.md`).

1. `bash tools/setup/check_env.sh` (T-001). Leaves nothing; prints one `OK` or `MISSING` line per check and exits 0 if all pass.
2. `bash tools/setup/fetch_ps2recomp.sh` (T-002). Leaves `third_party/PS2Recomp` at commit `2c5fbb9`, with the three patches of `patches/ps2recomp/series` applied. `--reset` restores it from clean.
3. `bash tools/pipeline/build_tools.sh` (T-003). Leaves `third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer` and `third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp`. **Measured**: both files exist (dated 2026-10-11).
4. `uv run --no-project python -I tools/disc/verify_disc.py <iso>` (T-004), then `bash tools/disc/extract.sh <iso> disc` (T-004; it runs `verify_disc.py` again). Leaves the disc contents in `disc/`: `SLPS_256.75`, `DATA.AFS`, `BGM.AFS`, `SE.AFS`, `DIR.DIR`, `SYSTEM.CNF` and `MODULES/`. **Measured**: `ls disc` shows these entries.
5. `bash tools/setup/fetch_ghidra.sh` (T-005). Leaves Ghidra 12.1.4 and the PS2 extension in `third_party/ghidra/`, and `config/ghidra_language.txt` (`r5900:LE:32:default`). **Measured**: see "This machine".
6. Function map. Only needed to regenerate it: `config/functions.ghidra.csv` is versioned, and running step 6a overwrites it.
   - 6a. `bash tools/funcmap/ghidra_export.sh` (T-006; the claim step is T-026). Leaves `config/functions.ghidra.csv`, `work/ghidra/don.ghidra.toml` and the logs in `work/ghidra/`. About 1 minute of analysis from scratch (STATUS); a few seconds when it reuses the analyzed project.
   - 6b. `uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml` (T-008). Leaves `config/layout.toml`.
   - 6c. `uv run --no-project --with rabbitizer python -I tools/funcmap/find_code_pointers.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv` (T-010). Leaves its report in `work/reports/code_pointers.json` (the default of `--report`).
   - 6d. `uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 --extra config/extra_entry_points.txt` (T-009; the check is exit 0). Leaves nothing but its printed summary; `--gaps-out` writes `work/reports/text_gaps.tsv` if given.
7. `bash tools/pipeline/generate.sh` (T-011 and T-012). Runs the analyzer only if `work/analyzer/config.toml` is missing, then `make_config.py`, the recompiler and `audit.py`. Leaves `work/don.toml`, `work/functions.filtered.csv`, `generated/` (the C++ of the game; local, never versioned), `work/logs/recomp.log` and `work/reports/translation_audit.json`. Exit 0 when the audit is clean. **Measured**: `work/don.toml` and `work/functions.filtered.csv` exist; `generated/` holds 57,724 entries.
8. Check of the output (T-013): `uv run --no-project python -I tools/verify/check_sqrt.py --elf disc/SLPS_256.75 --layout config/layout.toml --generated generated`. Reads `generated/` and the ELF; writes nothing; exit 0 without errors.
9. `bash tools/pipeline/build.sh linux-dev`. Leaves `build/linux-dev/don` (the release preset leaves `build/linux-release/don`; not built since the move to the container). **Measured** on 2026-10-12, in the container with clang 17: a full dev build takes 5 min 9 s at 12 jobs (1,957 steps: 1,802 unity batches of generated code, 72 large files alone, the runtime and the host program), a build with nothing to do 4 s, the executable is 115 MB. With GCC 11 on the host the same dev build took 23 minutes and the release one 41, 36 of them in `generated/register_functions.cpp` at `-O3`; that file now gets a fixed `-O1` in every preset. Every compile command carries `-ffp-contract=off` and `-march=x86-64-v3` and none a fast-math flag (checked on `compile_commands.json`). After changing the runtime's patches, `bash tools/setup/fetch_ps2recomp.sh --reset` rewrites its files and the next build recompiles everything.
10. `bash tools/run/smoke.sh [seconds]` runs `build/linux-dev/don --headless --seconds N` (folder from `DON_BUILD_DIR`), writes the log under `work/logs/` and classifies it (`tools/run/classify_log.py`); `tools/run/gdb_trace.sh` runs it under gdb with breakpoints. The game's files are looked up in `disc/`, so `MOVIE.AFS` must be there too (**Measured**, journal 004): the runtime serves the CD file search from that folder, not from the disc image.
10. GS library, needs the container image:
    - 10a. `bash tools/setup/fetch_pcsx2.sh` (T-103). Leaves `third_party/pcsx2` at commit `fd9d310` with the patches of `patches/pcsx2/series` (three). **Measured**: the commit is `fd9d310`.
    - 10b. `bash tools/container/build_image.sh` (T-104). Leaves the Docker image `don-build:1` (clang 17.0.6, cmake 3.31.9, SDL3 3.4.12 according to the T-104 card). **Measured**: `docker images don-build` shows it (1.01 GB).
    - 10c. `bash tools/pipeline/build_gs.sh` (T-105). Builds inside the image; leaves `build/gs/libdon_gs.so` and its runtime libraries in `build/gs/`. **Measured**: `build/gs/libdon_gs.so` exists.

## Pinned versions

Each version is fetched by one script, which checks it. The location is relative to the repository root.

| What | Version | Fetched by | Where it ends up | Check |
|---|---|---|---|---|
| Ghidra | 12.1.4 | `tools/setup/fetch_ghidra.sh` (T-005) | `third_party/ghidra/` (the archive and `ghidra_12.1.4_PUBLIC/`) | SHA-256 against `tools/setup/ghidra.sha256` |
| PS2 extension for Ghidra | v2.1.38 | `tools/setup/fetch_ghidra.sh` (T-005) | `third_party/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Extensions/ghidra-emotionengine-reloaded` | SHA-256 against `tools/setup/ghidra.sha256` |
| PCSX2 (GS, SPU2, VU sources only) | v2.8.2, `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3` | `tools/setup/fetch_pcsx2.sh` (T-103) | `third_party/pcsx2/`, patches from `patches/pcsx2/series` | the commit (`git -C third_party/pcsx2 rev-parse HEAD`) |
| PCSX2 as a program, for reference captures | v2.8.2 | `tools/setup/fetch_pcsx2_ref.sh` (T-024) | `third_party/pcsx2-ref/` (the AppImage and a `PCSX2/` folder used in portable mode) | SHA-256 recorded in `tools/setup/pcsx2_ref.sha256` |
| PS2Recomp | commit `2c5fbb9389e11dd95693385969490c9e8e6f57b4` | `tools/setup/fetch_ps2recomp.sh` (T-002) | `third_party/PS2Recomp/` with the patches of `patches/ps2recomp/series` | the commit, and `--reset` for a clean state |
| Build image | `don-build:1` (clang 17.0.6, SDL3 3.4.12, ccache from Debian; T-706) | `tools/container/build_image.sh` (T-104) | Docker's storage (not in the repository) | `docker images don-build` |
| LLVM clang and lld, Linux x86_64 (for the module build) | 17.0.6, `clang+llvm-17.0.6-x86_64-linux-gnu-ubuntu-22.04.tar.xz` from llvmorg-17.0.6 | `tools/pipeline/fetch_toolchain.py` | `build/toolchain/linux/` (the cache in `build/toolchain/cache/`) | SHA-256 against `tools/pipeline/toolchain.sha256`, which is **trust-on-first-use**: **Measured** 2026-10-25, LLVM publishes no `.sig` for this binary archive (the source tarballs only) and GitHub's release API gives `digest: null` for it and for all 55 assets of llvmorg-17.0.6. There is no second source to cross-check the hash. |

Changed on 2026-10-11: this section was "Pinned versions for what is still to be installed"; all
items above are now fetched or built. (The PCSX2 portable mode is the `portable.ini` file next to the
program, or the `-portable` argument, as in `tools/ref/run_pcsx2.sh`.)

## What may require `sudo` or user intervention

- A newer compiler and development packages (X11, Wayland, Vulkan, SDL3) to build
  the PCSX2 GS. Whether GCC 11 is enough is unverified. **Plan: do not touch the system and build those
  pieces inside a Docker container** based on Ubuntu 22.04 (same libc as this machine)
  with whatever compiler is needed (decision D11). (changed on 2026-10-11: done. The image
  `don-build:1` has clang 17.0.6, and the GS builds in it without `sudo` (T-104, T-105). The host was not changed.)
- Installing PCSX2 to generate reference screenshots and savestates (AppImage, does not need `sudo`;
  it does need the BIOS from the user's console). (changed on 2026-10-11: the AppImage is in
  `third_party/pcsx2-ref/` (T-024) and is used by `tools/ref/run_pcsx2.sh`; the BIOS is read from
  where it is and is not copied into the repository.)
- A Windows machine or virtual machine to validate that build.
