# 056 · Windows static-link A/B build of today's main

- **Date:** 2026-10-28
- **Task:** T-755
- **Status:** resolved (PC measured: the DLL boundary does not explain the ~4 fps gap; the static build still shows the ~1.6 fps residual of journal 059)

## Symptom
On the owner's PC (i5-9600K) the ougi fight script gives a worst 10 s window of 54.1-54.4 fps with the module build
38ecd3f (game code in `don_game.dll`) against 58.4-58.5 with the monolithic 63fc1b6 (journal 054; Linux shows no
difference). Hypothesis: the DLL boundary itself costs the time.

## Classification
Performance, Windows only. No catalog row.

## Investigation
Built main at 446ec4d the old way: `DON_LINK_GENERATED=ON` (the CMake default), preset `windows-release`, clang-cl +
lld-link in the Windows build container. No LTO anywhere (searching `CMakeLists.txt`, `CMakePresets.json` and `cmake/`
for LTO / INTERPROCEDURAL finds nothing), like the old build. Both commands with `DON_JOBS=4`, against the main
checkout's shared `generated/`:

1. `bash tools/pipeline/build_windows.sh runtime spu2 vu gs`: 6 min 31 s in total (gs step 187 s).
2. `DON_SKIP_CONFIGURE=1 DON_SLICE_SECONDS=1500 bash tools/pipeline/build_windows.sh game`: 1909 ninja steps,
   **1370 s (23 min)**. The "takes hours" comment in the script is not true here; one slice sufficed.

`don.exe` is 113 MB. Its import table, read with `x86_64-w64-mingw32-objdump -p` (not llvm-readobj), has no `don_game.dll` (0 matches); it imports
SDL3, z, MSVCP140, VCRUNTIME140, VCRUNTIME140_1, MSVCP140_ATOMIC_WAIT and system DLLs.

Folder `don-windows-static/`, like the old test bundle but **without** `game\`: `don.exe`, the dependency DLLs from
`third_party/win-deps/clangcl/bin` (c4core, freetype, jpeg62, libpng16, libsharpyuv, libwebp*, lz4, plutosvg, plutovg,
ryml, SDL3, shaderc_shared, turbojpeg, z, zstd), `gs/don_gs.dll` + `gs/resources/`, `vu/don_vu.dll`,
`spu2/don_spu2.dll`, the three VC runtime DLLs (msvcp140, vcruntime140, vcruntime140_1 from `work/vcredist/dlls` of
the main checkout) beside the exe and in gs/ vu/ spu2/, and a README-TEST.txt. Not included: `msvcp140_atomic_wait.dll`
(not found in the repository; the old `Battle Stadium DON.exe` imports it too, so the PC already resolves it).

Wine check: `wine don.exe --help` was run in the sandbox. It printed only Wine display errors ("no driver could be
loaded", there is no display in the sandbox) and no usage text; it exited with code 53. The program starts and loads its
DLLs. This is not a functional test.

## PC result
Owner's PC (i5-9600K), ougi fight, `tools/run/windows/fight_compare.ps1`, worst 10 s window, 2 runs per build:

| build | runs (fps) | state hash |
|---|---|---|
| old monolithic 63fc1b6 | 59.0 / 58.7 | `0cd1035476044e29` |
| static 446ec4d (`-noffmpeg`) | 56.9 / 57.5 | `21c177a252a28055` |

The static build's hash `21c177a252a28055` differs from the old build's because the build is `-noffmpeg` (movies are not
played); the noffmpeg T-759 exe gives the same hash. That is a build difference, not a game difference.

## Cause
The DLL boundary is not the source of the residual gap: the static build has no DLL boundary and is still about 1.6 fps
below the old build (mean 57.2 vs 58.85), the same residual as the module build at `-O2` (journal 059). The DLL build at
`-O1` lost about 4 fps (journal 055); the `-O` level accounts for most of that. The residual 1.6 fps is open (journal 059).

## Fix
None.

## Verification
Zip: `work/t755/don-windows-static-446ec4d.zip`, 40743563 bytes, SHA-256
`11089d3c6f966a2ce8ebad07759dab1b35b90c736a09a243eb2e6fd869fab4ba`. Build log: `work/t755/build.log`.

## Temporary diagnostics
None.
