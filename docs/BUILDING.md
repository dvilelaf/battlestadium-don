# Building

This file has the Linux build from a clean clone (next section, checked on 2026-10-15 from a fresh `git clone`: about 40 minutes
in all, from the clone to a headless run) and the Windows build, as far as it exists (T-401). How to play is in [docs/RUNNING.md](RUNNING.md);
the tools are in [docs/knowledge/09-environment-and-tools.md](knowledge/09-environment-and-tools.md).

## Linux, from a clean clone

Needs `git`, `cmake`, `ninja`, `g++` (host tools only), `7z`, `uv` and Docker usable without `sudo` (`bash tools/setup/check_env.sh` lists
what is missing), about 20 GiB of disk, network access, and **your own** disc image (Japanese; the English variants need the English
images too, see "English" below). Everything runs from the repository root. Times are from a 16-thread, 30 GB machine at `DON_JOBS=4`.

```bash
cp local.env.example local.env            # edit DON_ISO (the disc image); DON_PS2_BIOS_DIR is only for the reference emulator
set -a; . ./local.env; set +a
bash tools/setup/check_env.sh                                   # seconds: one OK/MISSING line per check
bash tools/setup/fetch_ps2recomp.sh                             # seconds: third_party/PS2Recomp at the pinned commit, patches applied
bash tools/setup/fetch_pcsx2.sh                                 # seconds: third_party/pcsx2 (needed by the next step)
bash tools/container/build_image.sh                             # about 5 minutes the first time: the Docker image don-build:1
bash tools/pipeline/build_tools.sh                              # about 1.5 minutes: the recompiler, on the host (uses every core; `nice` it on a shared machine)
uv run --no-project python -I tools/disc/verify_disc.py "$DON_ISO"      # must print VARIANT: jp
7z x -y -odisc "$DON_ISO" SLPS_256.75 SYSTEM.CNF DIR.DIR MODULES DATA.AFS SE.AFS BGM.AFS MOVIE.AFS
bash tools/pipeline/generate.sh                                 # about 30 seconds: generated/ (57,724 files, local, never versioned)
bash tools/setup/build_ffmpeg.sh                                # about 15 minutes in the container: the static FFmpeg of the videos (without it the build has no videos)
uv run --no-project python -I tools/pipeline/package_linux.py   # about 10 minutes: builds the game and the GS, VU and SPU2 libraries, makes build/player/don-linux/
tools/launcher/launch.sh                                        # the language window; `launch.sh en-jpvoice -- --headless --sound off --seconds 20` tries it without a screen
```

Notes from the check:

- `tools/disc/extract.sh` is not used: it does not take the `.AFS` files, which the port needs (`MOVIE.AFS` too); use the `7z` line above.
- The container steps (`build_image.sh` apart) hold the one-run-at-a-time lock of `build/.container.lock` and wait if another run has it.
  `build_tools.sh` and `build_image.sh` do not go through it.
- `package_linux.py` is all a player needs: it builds from the committed `HEAD` and does not need `build/linux-dev`. Developers build that
  one with `bash tools/pipeline/build.sh linux-dev` (the "Checking that the game still works" command of RUNNING.md uses it).
- A headless run prints no frame counters unless `DON_TRACE_FRAMES=1 DON_TTY=1` is set. To check one: run
  `DON_TRACE_FRAMES=1 DON_TTY=1 env -u DISPLAY tools/launcher/launch.sh en-jpvoice -- --headless --sound off --seconds 20 > run.txt`, then
  `uv run --no-project python -I tools/run/classify_log.py run.txt`: it must say `OK_VSYNC_LOOP` (the check got `1020` frames and `1140` vsyncs
  in 20 s). Without the variables the log has no `[don] vsync` lines and the classifier says `NO_PROGRESS`, which is not a failure of the port.
- The launcher also reads `~/.config/don/launcher.conf` and `~/.local/share/don/` (the variant folders made earlier, which can link to another
  checkout's `disc/`). To test from nothing, run it with another `HOME`.

### English

The English variants need the fan translation's images (see [knowledge/07-english-patch.md](knowledge/07-english-patch.md); they are not
downloaded by any script). Put them in `en_patch/jpvoice/disc.iso` and `en_patch/envoice/disc.iso`, or point `DON_EN_JPVOICE` / `DON_EN_ENVOICE`
at the folder that holds `disc.iso` (a clone elsewhere can use the folders of another checkout). Then `tools/launcher/launch.sh en-jpvoice`.
Checked the same way: `en-jpvoice` ran 20 s headless, `OK_VSYNC_LOOP`.

## Windows (cross-build)

Everything is cross-built **from Linux** inside the
image `don-build-win:1` (decision D13: clang-cl, lld-link, Microsoft CRT and Windows SDK fetched by xwin into `third_party/xwin/sysroot`).
The game itself (generated code and `don.exe`) is not built yet; see [17-windows-toolchain.md](knowledge/17-windows-toolchain.md), section 10.

Prerequisites: the image (`tools/container/build_image_windows.sh`), the xwin sysroot, and PCSX2 fetched and patched (`bash tools/setup/fetch_pcsx2.sh`; patch 0005 is for this build).

```bash
# 1. dependencies (SDL3, zlib, libpng, ... shaderc), pinned, into third_party/win-deps/clangcl (about 2.5 minutes)
bash tools/setup/build_win_deps.sh
# 2. runtime + host library, and the three bridge DLLs (default steps: runtime spu2 vu gs)
bash tools/pipeline/build_windows.sh
```

Both start the container themselves through `tools/container/run_windows.sh` (16 GiB limit, shared lock). `DON_JOBS` sets
the compile jobs (default 4, at most 6). `--deps DIR` and `--pcsx2 DIR` use other prefixes (relative paths are relative to the repository root).
The step `game` builds everything and links `build/windows-release/don.exe` (about 22 minutes with 4 jobs). It can run in slices so the
container lock is released: `DON_SLICE_SECONDS=230 DON_SKIP_CONFIGURE=1 bash tools/pipeline/build_windows.sh game`, repeated while it exits 124.
Run folder: `don.exe`, `SDL3.dll` from `build/win-deps/bin`, and the bridge DLLs in the folders given with `--gs-dir`, `--vu-dir`, `--spu2-dir`.
**Status:** it builds, links and runs under Wine 10: headless Vulkan, the state lines and a 55 s sound WAV are identical to Linux (section 11.3 of docs/knowledge/17-windows-toolchain.md); a real Windows PC is not tried yet.
[17-windows-toolchain.md](knowledge/17-windows-toolchain.md) section 11). It has not been run on Windows.

The preset is `windows-release` (`cmake --preset windows-release` inside the image; toolchain file `cmake/toolchain-windows-clangcl.cmake`).
It keeps the options that must not change: `-march=x86-64-v3`, `/clang:-ffp-contract=off`, no fast-math. The program is linked with `/STACK:16777216`.
Outputs: `build/windows-release`, `build/windows-spu2/don_spu2.dll`, `build/windows-vu/don_vu.dll`, `build/windows-gs/don-gs-bridge/don_gs.dll`.

### The folder a person copies to a Windows PC

```bash
bash tools/setup/build_ffmpeg_windows.sh    # once, about 15 minutes: the static FFmpeg of the videos (third_party/ffmpeg/windows)
bash tools/pipeline/package_windows.sh        # builds from the committed HEAD in its own folders: build/windows-package/don-windows/ (about 135 MB)
```

It is the Windows twin of `package_linux.py` (see docs/RUNNING.md): it builds the committed `HEAD` itself (no need for the steps above) and writes `BUILT_FROM.txt`. The folder holds `don.exe`, `SDL3.dll` and the dependency DLLs of `third_party/win-deps/clangcl/bin`
beside it (Windows looks for a library's own dependencies in the program's folder), `gs/don_gs.dll` with `gs/resources/`, `vu/don_vu.dll`,
`spu2/don_spu2.dll`, the program renamed `Battle Stadium DON.exe` (a window application: no console; messages in `don.log`), and from
`tools/package_windows/`: `README.txt` (written for a player) and `THIRD-PARTY.txt` (no `start.bat`: the player double-clicks the program, which
finds its game files in `game\` next to it and saves in `saves\`);
plus `LICENSE`. It refuses to finish if game data (`.afs`, `.fpk`, `.irx`, `.iso`, `SLPS_*`) is found in the folder, and none of Microsoft's files
are in it (the player installs the VC++ redistributable).

**One layout, one place in the code.** With no `--gs-dir`/`--vu-dir`/`--spu2-dir` (and no `DON_GS_DIR`...), each of the three libraries is
looked for in `<program folder>`, `<program folder>/<gs|vu|spu2>` and `<program folder>/../<gs|vu|spu2>` (`don_gs_load`, `don_vu_load`,
`don_spu2_load` in `src/host/*_glue.cpp`). The shipped folder uses the second, the Linux build tree the others; the options still win.
`tests/host/test_host_synthetic.py::test_default_search_...` checks the order of the places without the real libraries.
`tools/package_windows/README.txt` has a `SOURCE:` placeholder line the distributor fills in.
