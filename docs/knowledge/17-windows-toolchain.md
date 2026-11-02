# 17. Windows toolchain (T-400)

What it takes to produce Windows binaries of the pieces of the port. Everything was done on the
Linux host, cross-compiling inside `don-build-win:1` and running under Wine 6.0.3. Nothing was
run on a real Windows machine. Markers: **Measured** (ran it, saw it), **Declared** (read in a
document or source), **Unverified**.

Two routes were tried:

- **Route 1, clang-cl**: clang 17 (`clang-cl`), `lld-link`, `llvm-ml`, `llvm-rc`, MSVC CRT 14.44 and
  Windows SDK 10.0.26100 fetched with `xwin` 0.10.0 (Apache-2.0/MIT; downloads Microsoft's
  redistributable packages).
- **Route 2, llvm-mingw**: llvm-mingw 20261006, UCRT, libc++, GNU-style driver.

## 1. Results

| Piece | Route | Builds | Runs under Wine 6.0.3 | Evidence |
|---|---|---|---|---|
| SPU2 library (`don_spu2.dll` + `spu2_selftest.exe`) | 1 clang-cl | **yes** (Measured, about 3 s, 13 steps) | **yes** (Measured) | `PASS: 24 checks, 0 failed` |
| SPU2 library | 2 llvm-mingw | **yes**, with one source diff (P3) (Measured) | **yes** (Measured) | `PASS: 24 checks, 0 failed` |
| GS library (`don_gs.dll` + `gsreplay.exe`) | 1 clang-cl | **yes** (Measured, 738 steps, 1 m 16 s at `-j8`) | **no** (Measured): loads only with stub DLLs, then `GSopen` fails to create a render device | see 3.2 |
| GS library | 2 llvm-mingw | **no** (Measured): 266 failed steps, PCSX2 does not support MinGW | not tried | see 3.3 |
| PS2Recomp runtime (`ps2_runtime`, with raylib) | 1 clang-cl | **yes**, with a launcher that rewrites flags (P3) (Measured, 1 m 24 s at `-j8`) | not tried (no executable: it is a static library) | `ps2_runtime.lib` 6.0 MB |
| PS2Recomp runtime | 2 llvm-mingw | **yes**, no change (Measured, 2 m 00 s including configure and the raylib clone) | not tried | `libps2_runtime.a` 5.5 MB |
| Generated-code sample (24 unity batches + 2 large files + `don_overrides.cpp`, 27 objects) | 1 clang-cl | **yes**, 0 errors (Measured, 14.5 s at `-j8`) | not tried | |
| Generated-code sample | 2 llvm-mingw | **yes**, 0 errors (Measured, 8.2 s at `-j8`) | not tried | |
| Full `don.exe` (all of `generated/`, link) | both | **not built** (by design, about 40 min of all cores on Linux) | not tried | |

Extrapolation (Unverified): 1876 objects, about 0.3-0.55 s of wall time each at `-j8`, so roughly
10-17 minutes of compilation for the whole game on this host, plus a thin-LTO link. Other agents
were using the machine during the measurements (load average 11-15), so the times are upper
bounds. The first 24 batches may not be representative of the heaviest ones.

## 2. Problems, most important first

| # | Problem | Where | Fix | Route |
|---|---|---|---|---|
| P1 | **Wine 6.0.3 lacks `VirtualAlloc2`, `MapViewOfFile3`, `UnmapViewOfFile2`**; `don_gs.dll` imports them (api-ms-win-core-memory-l1-1-5/6), so it does not even load | `common/Windows/WinHostSys.cpp:150,206,215,245`, `pcsx2/GS/GS.cpp:940,952,958` | Real Windows 10 1803+ has them. For Wine: a newer Wine, or stub DLLs (used here, see 3.2 and 4.3) | 1 |
| P2 | **The GS library cannot create a render device under Wine 6.0.3 headless**; the Wine log shows wined3d on llvmpipe and no Vulkan initialisation | `GSopen` | Unverified why. Try a newer Wine (winevulkan) or real Windows. The reference run must be on Windows anyway. **(2026-10-16, measured: under Wine 9.0 + lavapipe in `don-wine:1`, `tools/container/Dockerfile.wine` / `run_wine.sh`, the Vulkan renderer opens headless and the full game runs with the GS and sound; the *software* renderer still fails — it wants a D3D11 device, which headless Xvfb cannot provide. Journal 041, section 11.7.)** | 1 |
| P3 | **FP flags**: `-ffp-contract=off` in the root `CMakeLists.txt` is *ignored* by clang-cl (`warning: unknown argument ignored`). clang-cl's default is contraction on, and `-march=x86-64-v3` has FMA, so the generated floating point would fuse differently. Also PS2Recomp's `cmake/ReleaseMode.cmake:18` adds `/fp:fast` for `MSVC` (which is true under clang-cl), against the AGENTS.md rule, and `ReleaseMode.cmake:24` adds `/Qspectre-`, which clang-cl rejects (`no such file or directory: '/Qspectre-'`): the runtime does not build | root `CMakeLists.txt` (flags block), `third_party/PS2Recomp/ps2xRuntime/cmake/ReleaseMode.cmake:18,24` | In the root file, `if(MSVC)` use `/clang:-ffp-contract=off`, `/fp:precise`, `/arch:AVX2` (or `/clang:-march=x86-64-v3`). A patch in `patches/ps2recomp/` for ReleaseMode.cmake (drop `/fp:fast`, `/Qspectre-`). Scratch workaround: `work/win/fixflags.sh` (4.3) | 1 |
| P4 | clang 17 vs xwin's MSVC STL 14.44, which requires Clang >= 19 (`yvals_core.h:908`) | the image | Temporary `-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` (it worked for everything built here). Proper fix: clang >= 19 in the image, or an older xwin manifest (MSVC 14.40 / VS 17.10 LTSC) | 1 |
| P5 | PCSX2 does not build with MinGW: `Windows.h` (capitalised include, `common/RedtapeWindows.h:21`), `_snwprintf` redefinition (`corecrt_wstdio.h:940`), `posix_memalign` chosen on non-MSVC (`common/HeapArray.h:110`), `gmtime_r` (`pcsx2/SIO/Memcard/MemoryCardFolder.cpp:73`), `std::free` (`common/SmallString.cpp:55`) | PCSX2 source | Not worth fixing: PCSX2 itself only supports MSVC/clang-cl (Declared, `docs/knowledge/12-building-pcsx2.md`) | 2 |
| P6 | mingw's `_mingw.h:303` redefines `__forceinline` as plain `inline`; the shim's external-definition meaning is lost and `GetMemPtr`, `TimeUpdate`, `CounterUpdate` are undefined at link | `src/spu2bridge/shim/common/Pcsx2Defs.h` (~20-22), `src/spu2bridge/shim/don_spu2_prelude.h` | Diff in 4.1 (include system headers, then `#undef` and redefine) | 2 |
| P7 | `src/spu2bridge/CMakeLists.txt` uses GNU flags (`-include`, `-fno-strict-aliasing`, `-ffp-contract=off`); clang-cl ignores `-include` and compiles its path as a source file | `src/spu2bridge/CMakeLists.txt` (`target_compile_options`) | Diff in 4.1 (`/FI`, `/clang:...` under `if(MSVC)`) | 1 |
| P8 | PCSX2 `cmake/BuildParameters.cmake:~100` adds `-msse4.1` to all languages, which `llvm-ml` rejects | PCSX2 | `$<$<COMPILE_LANGUAGE:C,CXX>:-msse4.1>` (4.1) | 1 |
| P9 | `cmake/SearchForStuff.cmake:35` has `add_subdirectory(3rdparty/D3D12MemAlloc)` but the directory is `d3d12memalloc` (case) | PCSX2 | Only on case-sensitive file systems; scratch symlink. Fine on Windows | 1 |
| P10 | `BuildParameters.cmake` reads `CMAKE_HOST_SYSTEM_PROCESSOR` for the architecture, wrong when cross-compiling from a non-x86 host | PCSX2 | Not hit here (x86-64 host) | both |
| P11 | Case sensitivity of the xwin sysroot: headers and libs are referenced as `Winver.h`, `PowrProf.lib`, `OneCore.lib` and xwin only links some names | sysroot | `work/win/xwin_case_links.py` added 1827 symlinks; two libs by hand. Not an issue on Windows | 1 |
| P12 | Build-system details when cross-compiling dependencies: `llvm-rc` needs the SDK include paths (`CMAKE_RC_FLAGS_INIT`); zlib tests need `-DZLIB_BUILD_TESTING=OFF`; libpng needs `-DSKIP_INSTALL_FILES=ON`; `winrt` include dir is needed by winwil | `work/win/build-win-deps.sh`, toolchain | In the scripts | 1 |
| P13 | The clang-cl SPU2 DLL imports `MSVCP140.dll`/`VCRUNTIME140.dll`; Wine supplies them, a Windows user needs the VC++ redistributable (or `/MT`). **Resolved by T-770 (2026-10-28):** the zip ships the runtime DLLs app-local | | Resolved: the four DLLs (`msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) from the pinned Microsoft redistributable (`tools/pipeline/fetch_vcredist.py`, pin `tools/pipeline/vcredist-windows.sha256`), placed beside the PEs that import them; import check in `tools/pipeline/package_checks.py`; loaded from the zip folder under Wine (journal 064) and on the owner's PC (HANDOFF.md, 2026-10-28) | 1 |
| P14 | Configuring the root project takes 80 s (57,724 generated files globbed) and fetches raylib, imgui and rlImGui with `git clone` (needs network) | root `CMakeLists.txt`, PS2Recomp `ps2xRuntime/CMakeLists.txt:69-120` | Pin and vendor those into `third_party/` before releasing | both |

Also noted: `llvm-17` in the Ubuntu image has no `clang-cl` package; `clang-cl` is a symlink to `clang`.

## 3. Evidence

### 3.1 SPU2 library

```
# route 1 (inside don-build-win:1)
cmake -S work/win/spu2bridge ... -DCMAKE_TOOLCHAIN_FILE=work/win/toolchain-clangcl.cmake ; ninja
$ WINEPREFIX=$PWD/work/wine/prefix xvfb-run -a wine ./spu2_selftest.exe
PASS: 24 checks, 0 failed        (peak 0.4972, RMS 0.4592, whole-run peak 0.8657: same as Linux)
```

`don_spu2.dll` 105,984 bytes, `spu2_selftest.exe` 25,088 bytes (route 1); `libdon_spu2.dll`
685,056 bytes, `spu2_selftest.exe` 537,600 bytes (route 2). Logs: `work/wine/spu2-clangcl.log`,
`spu2-mingw.log` (not versioned).

### 3.2 GS library, route 1

Configure (scratch PCSX2 copy `work/win/pcsx2` = `third_party/pcsx2` with patches 0001-0004, plus the
P8 diff and the P9 symlink):

```
cmake -S work/win/pcsx2 -B build/win-gs-clangcl -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=work/win/toolchain-clangcl.cmake -DCMAKE_PREFIX_PATH=third_party/win-deps/clangcl \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DUSE_VULKAN=ON -DUSE_OPENGL=OFF -DDISABLE_ADVANCE_SIMD=ON \
  -DDON_GS_BRIDGE_SOURCE_DIR=work/win/gsbridge
cmake --build build/win-gs-clangcl --target don_gs gsreplay -j8     # first build 1 m 16 s, rebuild 7-12 s
```

`don_gs.dll` 9.4 MB (65 MB PDB), `gsreplay.exe` 30 KB; exports `don_gs_*`. The Windows branch of patch
0004 (`VirtualAlloc`) and `don_gs_host.cpp` compiled unchanged. `common/FastJmp.asm` assembles with
`llvm-ml --m64`.

Run, in a stage directory with all dependency DLLs:

```
$ unset DISPLAY; WINEPREFIX=$PWD/work/wine/prefix xvfb-run -a wine ./gsreplay.exe --synthetic out.png --renderer software
err:module:import_dll Library api-ms-win-core-memory-l1-1-6.dll not found      (and -l1-1-5)
```

With stub DLLs `api-ms-win-core-memory-l1-1-5.dll`/`-6.dll` (built with llvm-mingw, 4.3) the library
loads, then:

```
[don-gs] don_gs_open: GSopen(software) failed: ReportErrorAsync: Error: Failed to create render device. ...
[gsreplay] open failed: ...        (exit 1)
```

Wine's log shows `wined3d_guess_card ... llvmpipe` and nothing from `winevulkan`. Not verified whether the
cause is Vulkan under Wine 6.0.3 or the build (the host has RADV and lavapipe; `vulkan-1.dll` and
`winevulkan.dll` are present in the prefix).
**Runs under Wine: no.** A real Windows run is needed to settle it.

### 3.3 GS library, route 2

Dependencies cross-build (all of them, about 3 min): `third_party/win-deps/mingw`, 35 MB. PCSX2 configures
(2.7 s) but the build stops: 266 failed steps (`-k 0`, 40 s). Top errors: `use of undeclared identifier
'_Format'` and `'_MaxCount'` x112-224 (the `_snwprintf` redefinition), `Windows.h file not found` x112,
`posix_memalign` x70, `gmtime_r`, `std::free`. First lines are in P5. **Builds: no.**

### 3.4 Runtime and sample of generated code

Root project configured read-only (`-S .`, `-B build/win-clangcl` and `build/win-mingw`) with the toolchain
files below and `-G Ninja -DCMAKE_BUILD_TYPE=Release`; `generated/` untouched.

Effective flags (from `compile_commands.json`):

| Flag | clang-cl | llvm-mingw |
|---|---|---|
| `-ffp-contract=off` | **ignored with a warning** (P3); needs `/clang:-ffp-contract=off` | applied |
| `-march=x86-64-v3` | accepted | applied |
| fast-math | `/fp:fast` added by ReleaseMode.cmake (P3) | not added |
| LTO | `-flto=thin` | `-flto=thin` |

Commands:

```
cmake --build build/win-clangcl --target ps2_runtime -j8                       # 1 m 24 s, 0 failures (with fixflags.sh)
ninja -C build/win-clangcl -j8 <24 unity .obj> <FUN_001bcb40.obj> <FUN_00182ef0.obj> <don_overrides.obj>   # 14.5 s, 0 failures
cmake --build build/win-mingw --target ps2_runtime -j8                         # 2 m 00 s, 0 failures
ninja -C build/win-mingw -j8 <same 27 objects>                                 # 8.2 s, 0 failures
```

The two large files are the 10th and 40th smallest of the 75 standalone files (208,842 and 255,247 bytes), i.e. mid-sized, not the biggest;
the largest file was deliberately not compiled.

## 4. Scratch changes (all under `work/win/`, not versioned)

### 4.1 Diffs

- `spu2-clangcl-flags.diff` (P7): in `src/spu2bridge/CMakeLists.txt`, an `if(MSVC)` branch with
  `"/FI${CMAKE_CURRENT_LIST_DIR}/shim/don_spu2_prelude.h" /clang:-fno-strict-aliasing /clang:-ffp-contract=off`,
  the existing GNU options in the `else()`.
- `spu2-mingw-forceinline.diff` (P6): appended to `shim/don_spu2_prelude.h`:

  ```cpp
  #ifdef __MINGW32__
  #include <stdlib.h> <string.h> <wchar.h> <malloc.h> <math.h> <cmath> <cstdint> <deque> <vector> <string>
  #undef __forceinline
  #define __forceinline __attribute__((always_inline, unused))
  #endif
  ```
- `pcsx2-clangcl-masm-flags.diff` (P8): `add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-msse4.1>")` in `cmake/BuildParameters.cmake`.
- Symlink `3rdparty/D3D12MemAlloc -> d3d12memalloc` in the scratch PCSX2 (P9).

### 4.2 Toolchain files

`toolchain-clangcl.cmake` (route 1): `CMAKE_SYSTEM_NAME Windows`, `CMAKE_SYSTEM_PROCESSOR AMD64`;
`clang-cl` for C and CXX with `--target=x86_64-pc-windows-msvc -fuse-ld=lld-link -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`;
`/imsvc` for `crt/include`, `sdk/include/{ucrt,um,shared,winrt}` of `$DON_XWIN_ROOT`; `/libpath:` for
`crt/lib/x86_64`, `sdk/lib/{um,ucrt}/x86_64`; `lld-link`, `llvm-lib`, `llvm-rc` (with `-I` of the same includes),
`llvm-mt`, `CMAKE_ASM_MASM_COMPILER llvm-ml` with `--m64`; `CMAKE_TRY_COMPILE_CONFIGURATION Release`;
`CMAKE_FIND_ROOT_PATH` = the sysroot.

`toolchain-mingw.cmake` (route 2): `x86_64-w64-mingw32-clang`/`clang++`/`windres` from `$LLVM_MINGW`, `llvm-ar`,
`llvm-ranlib`, `CMAKE_FIND_ROOT_PATH $LLVM_MINGW/x86_64-w64-mingw32`.

### 4.3 Scripts

- `build-win-deps.sh <toolchain> <prefix> <builddir> [names]`: cross-builds the PCSX2 Windows dependencies. Versions and
  hashes are those of PCSX2's `.github/workflows/scripts/windows/build-dependencies.bat`: zlib 1.3.2, libpng 1.6.58 (+APNG patch),
  libjpeg-turbo 3.2.0, lz4 1.10.0, zstd 1.5.7, libwebp 1.6.0, FreeType 2.14.3, SDL3 3.4.12, plutovg 1.3.2, plutosvg 0.0.7,
  rapidyaml 0.12.1, DirectX-Headers 1.619.1, shaderc 2026.2 (glslang `275822a`, SPIRV-Headers `58006c9`, SPIRV-Tools `6337eb6`,
  `shaderc-changes.patch`). Qt, FFmpeg and harfbuzz skipped. Sources cached in `third_party/win-deps-src/`.
  Both prefixes built completely (clang-cl: about 1.5 min, mingw: about 3 min).
- `xwin_case_links.py` (P11).
- `fixflags.sh` (P3), used as `CMAKE_{C,CXX}_COMPILER_LAUNCHER`: drops `/Qspectre-`, rewrites `/fp:fast` to `/fp:precise` and
  `-ffp-contract=off` to `/clang:-ffp-contract=off`.
- `stubs/stub5.c`, `stub6.c`: `__declspec(dllexport)` functions `UnmapViewOfFile2` and `VirtualAlloc2`/`MapViewOfFile3` that
  set `ERROR_CALL_NOT_IMPLEMENTED` and fail; built as `api-ms-win-core-memory-l1-1-5.dll` and `-6.dll` with
  `x86_64-w64-mingw32-clang -shared`. Diagnostic only (AGENTS.md rule 4): they are not a solution.

## 5. The image and downloads

`tools/container/Dockerfile.windows` (`FROM don-build:1`, the existing image is untouched), `build_image_windows.sh`,
`run_windows.sh` (copy of `run.sh` using `DON_BUILD_WIN_IMAGE`). The image `don-build-win:1` is 1.82 GB (the base is 1.06 GB) and
builds in about 45 s. It adds `llvm-17` tools (symlinks for `lld-link`, `llvm-ml`, `llvm-lib`, `llvm-rc`, `llvm-mt`), `clang-cl`,
the `xwin` binary (0.10.0, 3.4 MB, sha256 `d870eb4b...fc88`) and llvm-mingw (84 MB, sha256 `5f9c6ed9...8ab4`). The xwin sysroot is
not in the image: it is fetched on the host into `third_party/xwin/sysroot`.

| Download | Size | Where |
|---|---|---|
| xwin binary | 3.4 MB | `third_party/xwin-bin/` |
| llvm-mingw 20261006 UCRT | 84 MB (81 MB unpacked) | `third_party/llvm-mingw/` |
| MSVC CRT + SDK through xwin | 313 MB download, 751 MB unpacked cache, 641 MB sysroot | `third_party/xwin-cache`, `third_party/xwin/sysroot` (xwin splat: 26 s) |
| Dependency sources | 56 MB | `third_party/win-deps-src/` |
| Built dependencies | 29 MB (clang-cl), 35 MB (mingw) | `third_party/win-deps/` |
| raylib, imgui, rlImGui (git clone at configure) | small | `build/*/_deps` |

## 6. What an end user needs on native Windows

| | Route 1, clang-cl | Route 2, llvm-mingw |
|---|---|---|
| Compiler | Visual Studio Build Tools (MSVC CRT + Windows SDK, accepting Microsoft's licence) plus LLVM clang-cl >= 19 (`llvm-ml` for PCSX2's `FastJmp.asm`, or `ml64`) | llvm-mingw, one zip, no licence |
| Others | CMake >= 3.21, Ninja, Git (for the raylib/imgui clones), Python via `uv` | same |
| PCSX2 pieces (GS) | supported by PCSX2 (Declared) | **not buildable** (P5) |
| Runtime and game | works with the P3 fixes | works as is, and the GNU-style flags in the root `CMakeLists.txt` apply |
| End-user binary | needed the VC++ redistributable unless `/MT` (corrected: resolved by T-770, the runtime DLLs ship app-local, P13) | needs `libc++`/`libunwind` DLLs of llvm-mingw next to the exe, or static linking |

Whether the same CMake presets work: the preset file is Linux-only (`linux-release`). A Windows preset would set the toolchain
file and `DON_XWIN_ROOT`. Unverified: no run on Windows.

## 7. Recommendation

1. **Use clang-cl for the pieces that include PCSX2 code (GS), and it is also the only route that can build `don_gs.dll`.** Use
   the same compiler for everything shipped in one package so a single toolchain is documented. llvm-mingw cannot build the GS
   (P5), so route 2 only makes sense for the runtime and game, which would mean two toolchains. Not recommended.
2. Before relying on it, make a patch for PS2Recomp's `ReleaseMode.cmake` (drop `/fp:fast`, `/Qspectre-`) and change the flag block of
   the root `CMakeLists.txt` to `/clang:-ffp-contract=off` and `/arch:AVX2` under clang-cl (P3). Without it the generated floating point
   differs from Linux (AGENTS.md "Build options that are not touched").
3. Cross-compiling from Linux is fast and workable for building (the whole GS in 76 s) but **cannot validate the GS**: Wine 6.0.3 lacks
   the Windows 10 memory APIs and could not create a render device. The first run of `don_gs.dll` must be on a real Windows machine,
   or with a newer Wine (to be tried in a separate task; it needs a decision about installing one in `third_party/`).
4. Vendor raylib, imgui and rlImGui (P14) and update the image to clang >= 19 (P4) before a release.

## 8. Not done / unverified

- Full `don.exe` build and link; any run of the game or of the runtime under Wine.
- Whether `-flto=thin` with clang-cl produces the same floating-point results as Linux (needs a comparison run).
- GS on a newer Wine, on Windows, or with the D3D11 renderer.
  **(2026-10-16, measured, journal 041: under Wine 9.0 + lavapipe the GS *does* open and the whole game
  runs with the GS and sound — Vulkan renderer only; the software renderer stays unusable under Wine
  (D3D11 headless). The Windows-under-Wine Vulkan and WAV output is byte-identical to Linux for the
  title and practice routes and differs by 1 pixel in 4 of 41 frames of the ougi cutscene.)**
- Time box: about 3 h of 6 h used.

## 9. State on 2026-10-13 (T-401, first piece: what does not compile)

A survey, not a port. Same route as section 1 (clang-cl 17 + lld-link + xwin MSVC CRT 14.44 / SDK 10.0.26100, inside `don-build-win:1`),
cross-compiled from Linux. Objects only, no link of the game. Nothing under `src/`, `third_party/`, `generated/`, `config/`, `patches/` or the
root CMake files was edited; everything else is in `work/win-survey/` and `build/win-survey*/` (not versioned). **Measured** everywhere below;
nothing was run on Windows or under Wine.

### 9.1 Safety: `run_windows.sh` now has the same two protections as `run.sh`

`tools/container/run_windows.sh` (versioned) got, copied from `run.sh`: `--memory`/`--memory-swap` from `DON_CONTAINER_MEMORY` (default 55 % of
MemTotal in whole GiB; 16g on this 30 GB host), and one container run at a time through an exclusive `flock` on `build/.container.lock` taken
*inside* the container (`flock "$LOCK" <command>` as the container command, after a host-side probe that prints "waiting"). It is the same lock
file as `run.sh`, so a Windows build and a Linux build never run together. `flock` exists in `don-build-win:1` (it is `FROM don-build:1`); the
image was not rebuilt. Check (two runs started 3 s apart; the second printed the waiting line and started only after the first ended):

```
A start 20:28:18 ... A end 20:28:26          (sleep 8, real 11.3 s)
waiting for another container run to finish   (B, stderr)
B start 20:28:28   mem: 17179869184           (cgroup memory.max = 16 GiB)
```

### 9.2 Commands (all from the repository root; compile jobs <= 6)

```
tools/container/run_windows.sh work/win-survey/configure.sh        # root project, -B build/win-survey, 7 s + 67 s generate; log configure.log
tools/container/run_windows.sh work/win-survey/compile-ab.sh       # (a) + (b): ninja -k 0 -j6 on the 87 runtime/host objects; log compile-ab.log
tools/container/run_windows.sh work/win-survey/bridges.sh          # GS (3 files), SPU2 and VU standalone projects, CMakeLists unchanged
tools/container/run_windows.sh work/win-survey/bridges-fixed.sh    # SPU2 and VU with scratch copies of their CMakeLists (flag fixes only)
tools/container/run_windows.sh work/win-survey/bridges-fixed-vu.sh # VU again with a fake <sys/mman.h> (scratch, diagnostic)
uv run --no-project python -I work/win-survey/sample.py $PWD       # the 200-file sample -> work/win-survey/sample.txt
tools/container/run_windows.sh work/win-survey/compile-sample.sh   # (c) + don_overrides.cpp, xargs -P6; results sample-result.txt
```

`configure.sh` is the root `cmake -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=work/win-survey/toolchain-clangcl.cmake
-DCMAKE_PREFIX_PATH=third_party/win-deps/clangcl` with `work/win-survey/fixflags.sh` as compiler launcher (rewrites `-ffp-contract=off` to
`/clang:-ffp-contract=off`, as in section 4.3). The toolchain file is the one of section 4.2, copied. Flags of the objects (from `build.ninja`):
`-march=x86-64-v3 /clang:-ffp-contract=off /O2 /Ob2 /DNDEBUG -std:c++20 -MD /EHsc`, no `/fp:` option, no LTO (see cause 6).
The sample is `names[i*57650/200]` for `i = 0..199` of `generated/sources.txt` (the 72 files of `sources_big.txt` are not in that list and were
not sampled); each file is compiled alone (not in a unity batch) with the `don` target's flags plus `-I generated`.

### 9.3 Results

| Group | What | Compiled | Failed |
|---|---|---|---|
| a | PS2Recomp runtime as the root build sees it: `ps2_iop` (23) + `ps2_runtime` (46) | **69 / 69** | **0** |
| b1 | `src/host` (`don_host`, 17 files, plus `main.cpp`) and `src/game/don_overrides.cpp` | **19 / 19** | **0** |
| b2 | GS bridge, port side (`don_gs.cpp`, `don_gs_host.cpp`, `gsreplay.cpp`), inside a scratch copy of PCSX2 with patches 0001-0004 | **3 / 3** | **0** |
| b3 | SPU2 bridge, `src/spu2bridge/CMakeLists.txt` unchanged (`don_spu2.cpp`, `spu2_selftest.cpp` + 9 PCSX2 SPU2 sources) | **1 / 11** | 10 (all the same flag error, cause 2) |
| b3' | the same with the `/FI` and `/clang:` flag fix in a scratch CMakeLists | **11 / 11, and it links** (`don_spu2.dll`, `spu2_selftest.exe`) | 0 |
| b4 | VU bridge, `src/vubridge/CMakeLists.txt` unchanged (`don_vu.cpp`, 2 shims + 5 PCSX2 VU sources + 20 PCSX2 common/emitter/fmt sources = 28 objects) | 20 / 28 | 8 (all the flag error, cause 2) |
| b4' | the same with the flag fix (scratch) | 27 / 28 | 1: `shim/don_vu_shims.cpp`, `sys/mman.h` (cause 3) |
| b4'' | the same with a fake `mman.h` so the file compiles | 28 / 28; the link then stops at `PathCchCanonicalizeEx`, `PathCchSkipRoot` (need `pathcch.lib`), `mmap`, `munmap` | 0 compile |
| c | 200 generated files, one object each, + `don_overrides.cpp` | **200 / 200** (201 objects, none empty) | **0** |

(b3 as run: 10 of the 11 objects failed, the 9 PCSX2 SPU2 files and `don_spu2.cpp`; `spu2_selftest.cpp`, which has no `-include`, compiled.)
Of the runtime's 74 `.cpp` files, 69 are in the targets above; the other five are `src/main.cpp`, `src/runner/register_functions.cpp`,
`ps2_debug_panel.cpp`, `ps2_android_runtime.cpp`, `ps2_vita_runtime.cpp`. They belong to the raylib runner, Android and Vita and are not part of
this port's build (`PS2X_WITH_RAYLIB=OFF`); not compiled.

Warnings only, none blocking: `-Wdeprecated-declarations` for `getenv`, `strncpy`, `fopen`, `strcpy`, `strcat`, `strerror`, `_wfopen` (the MSVC CRT
"unsafe" family; define `_CRT_SECURE_NO_WARNINGS` for the target, or ignore) and four `-Wswitch` in `ps2_gs_memory.h` (also on Linux).

### 9.4 What does not compile, by cause (most blocking first)

The runtime, `src/host`, `src/game`, the GS bridge and 200 generated files all compile for Windows as they are. Everything that fails is in
build files or in one VU shim.

| # | Cause | Files | Example | Fix | Belongs in |
|---|---|---|---|---|---|
| 1 | None of the above: no compile error at all in groups a, b1, b2, c. The earlier work (P3-P4 of section 2) plus the port's own `_WIN32` branches (`gs_glue.cpp:22`, `spu2_glue.cpp:15`, `vu_glue.cpp:16`, `don_gs_host.cpp:34`, `window_native.cpp:52`) already cover them | 0 | | | |
| 2 | **GNU-style flags in the standalone bridge CMake files**: `-include` (clang-cl takes the path as a second source file: `cannot specify '/Fo...' when compiling multiple source files`), `-fno-strict-aliasing`, `-fPIC`, `-fvisibility=hidden`, `-pthread`, `-static-libstdc++`/`-static-libgcc` (lld-link: unknown argument, warning only), `-Wl,--version-script` (guarded by `UNIX`) | SPU2: 10 of 11; VU: 8 of 28 (every file that gets `-include`) | `src/spu2bridge/CMakeLists.txt:59` (`SHELL:-include ...prelude.h`); `src/vubridge/CMakeLists.txt:107` (`COMPILE_FLAGS "-include ...don_vu_prefix.h"`) | `if(MSVC)` branches: `/FI<header>`, `/clang:-fno-strict-aliasing`, `/clang:-ffp-contract=off`; drop `-fPIC -fvisibility=hidden -pthread`; drop `__POSIX__` from `don_vu_flags`; for the DLL exports use `don_spu2.h`/`don_vu.h`'s `_WIN32` branch (already there) instead of the version script. The SPU2 fix is the diff of section 4.1 (P7) and was re-verified to compile and link; the VU one was applied in `work/win-survey/vubridge-fixed/CMakeLists.txt` | build files (`src/spu2bridge`, `src/vubridge`) |
| 3 | **`sys/mman.h`** in the VU shim: it maps microVU's code cache with `mmap(PROT_EXEC)` at hints near the library image (so rel32 jumps reach it) and `munmap`; `<cpuid.h>` and `__get_cpuid`/`__cpuid_count`/`xgetbv` inline asm do compile with clang-cl | 1 | `src/vubridge/shim/don_vu_shims.cpp:17` (`mmap` at 128-140, `munmap` at 156) | `VirtualAlloc(hint, size, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE)` with the same search loop over hints, `VirtualFree(p, 0, MEM_RELEASE)`, error text from `GetLastError`; `#ifdef _WIN32` | the port's own code (`src/vubridge/shim`) |
| 4 | **Missing import library at link** (VU, found past the compile): `PathCchCanonicalizeEx`, `PathCchSkipRoot` come from PCSX2's `common/FileSystem.cpp` | 1 link | `lld-link: undefined symbol: __declspec(dllimport) PathCchCanonicalizeEx` | add `pathcch` (and whatever else PCSX2's own `common` links: `Pathcch.lib`) to `don_vu` under `if(WIN32)` | build files (`src/vubridge`) |
| 5 | **Third-party headers**: SDL3 headers/libs were found (`third_party/win-deps/clangcl`, 3.4.12, built by `work/win/build-win-deps.sh`; on Linux the container has them under `/opt/deps`). Nothing else is needed for groups a-c. For the GS library itself (not part of this piece): zlib, libpng, libjpeg-turbo, lz4, zstd, libwebp, FreeType, plutovg/plutosvg, rapidyaml, DirectX-Headers, shaderc, from the same prefix | | | Reproducible only with `work/win/build-win-deps.sh`, which is scratch. A versioned script (T-401 proper) | tools / build files |
| 6 | **`ReleaseMode.cmake` is not applied** in this configuration: with `PS2X_WITH_RAYLIB=OFF` (as the root `CMakeLists.txt` sets it) no "Enabling optimization" message appears at configure, and `build.ninja` has no `/fp:fast`, no `/Qspectre-`, no `/GL`, no LTO. So P3's two ReleaseMode problems do **not** hit the root build today (why the call at `ps2xRuntime/CMakeLists.txt:567` is not reached was not investigated). The root flag block still needs `/clang:-ffp-contract=off` (cause 7) | | | Keep it so; if a patch ever turns it on, drop `/fp:fast` there | runtime patch (only if it changes) |
| 7 | **`-ffp-contract=off` is ignored by clang-cl** (`warning: unknown argument ignored`) and `-march=x86-64-v3` has FMA, so without a fix the generated float code could fuse. Here a compiler launcher rewrites it. Note `-march=x86-64-v3` itself is accepted | all C++ of the root build | root `CMakeLists.txt` (flag block, `if(CMAKE_CXX_COMPILER_ID MATCHES "GNU\|Clang")`) | `if(MSVC)`: `/clang:-ffp-contract=off` (keep `-march=x86-64-v3` or `/arch:AVX2`); also `src/host/CMakeLists.txt` guards with `if(NOT MSVC)`, which skips the options under clang-cl: the host gets neither, add them there too | build files (root, `src/host`) |
| 8 | **Compiler/STL mismatch** (P4): `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` is still needed (clang 17 vs STL 14.44) | all | `yvals_core.h:908` | clang >= 19 in the image or an older xwin manifest | image / toolchain file |
| 9 | **Configure fetches nothing here** (`PS2X_WITH_RAYLIB=OFF`): P14 (raylib, imgui, rlImGui clones) does not apply to the root build; only to PS2Recomp's own runner | | | none | |

### 9.5 Things that compile but will behave differently on Windows (read from the source; nothing run)

1. **Game thread stack size.** `ps2_runtime.cpp:2508` starts the game with `std::thread`. On Windows the stack is the PE header's reserve (1 MB by
   default, set by `/STACK`), on Linux glibc gives 8 MB. The generated functions call each other as C++ calls. Set `/STACK:` for `don.exe` (or
   create the thread with an explicit stack) and measure the deepest host recursion before choosing.
2. **Floating-point control register** (`src/host/fp_env.cpp`): `_mm_getcsr`/`_mm_setcsr` with 0xFFC0 compile under clang-cl (`_M_X64` is in the
   guard). The code sets it once per game thread. On Linux a new thread inherits its parent's MXCSR; on Windows a new thread starts with the
   default 0x1F80 and each thread (GS library thread, audio callbacks, SDL threads) keeps its own, so anything that assumed inheritance now
   differs. Also `/clang:-ffp-contract=off` and the CRT's `libm` (`sqrt`, `sin`, `fmod`...) differ in the last bit from glibc's; with FTZ/DAZ and
   round-toward-zero on, UCRT math functions are not specified to behave as glibc (need a conformance comparison; the `tests/conformance`
   suite is the place).
3. **Dynamic loading of the three PCSX2 libraries**: the library names have a `_WIN32` branch (`don_gs.dll`, `don_spu2.dll`, `don_vu.dll`) and the load
   goes through `SDL_LoadObject(absolute.string().c_str())`. On Windows `fs::path::string()` is the ANSI code page, SDL expects UTF-8: a path with
   non-ASCII characters (a user name with an accent) fails to load. Use `u8string()`. Also: the search path (`$ORIGIN`/`BUILD_RPATH` on Linux,
   `SetDllDirectory`/the exe's folder on Windows), and the DLL needs the VC++ runtime or `/MT` (P13). `RUNTIME_OUTPUT_DIRECTORY` and rpath settings in the bridge CMake files are Linux-only.
4. **File paths and encoding**: the runtime's VFS (`ps2_vfs.cpp`, `ps2_path.cpp`) and the syscall stubs use `fopen`/`std::ifstream` with narrow strings (the CRT's
   deprecation warnings point at them); guest names are Shift-JIS/ASCII, host paths are the ANSI code page on Windows. Also case sensitivity
   (Linux is case sensitive, NTFS is not: a lookup that works only because it is case-exact on Linux is fine on Windows, the reverse is the risk), `\\` vs `/`, and `Common.h:27-28`
   including `<unistd.h>`/`<sys/stat.h>` (compiles because of `_WIN32` guards; the stat mode macros differ).
5. **`profile_linux.cpp` compiles to nothing**; `profile.cpp` provides the "not available" fallback (`start_sampler` returns false). `DON_PROFILE` does nothing on Windows.
6. **Signals**: `host.cpp:561` uses `std::signal(SIGINT/SIGTERM)`; on Windows a console close or Ctrl+Break does not deliver SIGTERM, and the handler runs on a new thread.
7. **`ThreadNaming.h`** uses `pthread_setname_np` on Linux; on Windows it takes another path (compiled, not read in detail).
8. **Guest memory** is `new uint8_t[...]` (`ps2_memory.cpp:344-384`), not an address-space reservation: no `mmap`/`VirtualAlloc` dependence there. Large `new` of the RDRAM (32 MB) is fine on both.
9. **Console subsystem**: the host `main()` links as a console program; a release build may want `/SUBSYSTEM:WINDOWS` with `mainCRTStartup` (`PS2X_SHOW_WINDOWS_CONSOLE` exists in the runtime's CMake for the runner only).

### 9.6 What the next piece should be, and what is not verified

Next: T-401 proper. (1) A versioned Windows toolchain file and a CMake preset (`windows-release`) that carry cause 7 and the flag set, no launcher. (2) The
`if(MSVC)`/`if(WIN32)` branches of the SPU2 and VU CMake files (causes 2, 4) and the `VirtualAlloc` replacement in `don_vu_shims.cpp` (cause 3). (3) A
`pathcch`/system-library list for the VU and GS DLLs. (4) The full `don.exe` link: nothing here proves the link (1875 objects, ~57,000 generated
functions, `register_functions.cpp`, the LTO decision, `/STACK`). (5) Move the dependency build (`build-win-deps.sh`) into `tools/`.

Not verified: the 72 files of `sources_big.txt` and the other ~57,400 of `sources.txt` (the sample is 200 files from the small set, and the first survey compiled 24 batches plus two large
files); link of anything except the SPU2 scratch build; any run (Windows or Wine); whether the generated floating point is bit-identical to Linux with `-ffp-contract=off` under clang-cl (no comparison
run); the five runtime files not in the targets; the list in 9.5 is from reading source, not from running.

## 10. State on 2026-10-13, second piece: what is versioned (branch `agent/t401-windows`)

Everything below was **run** inside `don-build-win:1` through `tools/container/run_windows.sh`, in a git worktree.

| What | Where |
|---|---|
| Toolchain file (clang-cl, lld-link, xwin sysroot; `DON_XWIN_ROOT` overrides the sysroot; keeps `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` for clang 17) | `cmake/toolchain-windows-clangcl.cmake` |
| Preset `windows-release` (configure and build) | `CMakePresets.json` |
| Float options for MSVC-like compilers: `-march=x86-64-v3 /clang:-ffp-contract=off`, no `/fp:*` (a later `/fp:precise` would turn contraction back on) | `CMakeLists.txt`, `src/host/CMakeLists.txt` |
| `/STACK:16777216` on `don` | `CMakeLists.txt` |
| SPU2 and VU bridges: forced include `/FI`, `/clang:` flags, no `-fPIC`/visibility/`-pthread`/`__POSIX__`, no static libstdc++, `pathcch` for the VU DLL | `src/spu2bridge/CMakeLists.txt`, `src/vubridge/CMakeLists.txt` |
| Code cache memory in one file per platform (`mmap` / `VirtualAlloc`), the search near the image stays in `don_vu_shims.cpp` | `src/vubridge/shim/don_vu_codemem*.{h,cpp}` |
| PCSX2 patch 0005: `-msse4.1` only for C/C++ (llvm-ml rejects it), `D3D12MemAlloc` folder case | `patches/pcsx2/0005-...` |
| Dependencies build, pinned | `tools/setup/build_win_deps.sh` (pins at the top, the same as `tools/container/build-deps.sh` for the libraries both have; SDL3 3.4.12) |
| xwin case links (`winver.h` -> `Winver.h`), needed once after `xwin splat` | `tools/setup/xwin_case_links.py` |
| Driver | `tools/pipeline/build_windows.sh` |

**Checks run.**
- Preset configures (`cmake --preset windows-release`, 80 s, mostly the globbing of generated sources). Compile lines (`compile_commands.json`):
  `unity_0_cxx.cxx`: `-march=x86-64-v3 /clang:-ffp-contract=off /O2 /Ob2`; `ps2_memory.cpp`: the same; `src/host/fp_env.cpp`: `-march=x86-64-v3 /clang:-ffp-contract=off` (twice: root flags and target flags).
  `grep -c fp:fast build.ninja` prints 0; `/STACK:16777216` appears in `build.ninja`. A disassembly of `unity_0` has no `vfmadd*`, but it has no float arithmetic either, so that is not evidence by itself.
- `build_win_deps.sh --prefix build/win-deps` with the cached archives: all 13 libraries `ok` in 2 min 18 s; the preset configured against that prefix (`CMAKE_PREFIX_PATH` in the cache) and built `ps2_iop ps2_runtime don_host don_host_main` (88 steps, 107 s). **Not run:** the script's downloads (the archives came from the existing cache; the SHA-256 check ran on them).
- `don_spu2.dll` (13/13), `don_vu.dll` + `vu_selftest.exe` (31/31) and `don_gs.dll` (736/736, 83 s with 6 jobs) build **and link** for Windows. The GS used a copy of `third_party/pcsx2` with patch 0005 applied (`build/pcsx2-win`) because the shared tree must not be edited from the worktree; `fetch_pcsx2.sh --reset` from clean was **not** run (the patch was checked with `git apply --check` and applied to the copy).
- Linux unchanged: in the worktree, `libdon_vu.so`, `vu_selftest`, `libdon_spu2.so`, `spu2_selftest` build; `vu_selftest`: `PASS: 64 checks, 0 failed`; `tests/vuglue` (it finds the library at `<repository root of the test file>/build/vu/libdon_vu.so`, so the worktree's own build was copied there and the tests were built in `build/vuglue-tests` of the worktree): `4 passed`.

**Stack.** The PE header's reserve applies to the main thread and to every `std::thread` created without an explicit size: the game thread and the host's audio/input threads. Threads made by the DLLs (GS, VU, SPU2) with their own `CreateThread` call get the same default of the *executable*. 16 MB is twice the Linux default; the deepest recursion has not been measured.

**Not verified / open.** `don.exe` link (SDL3.dll and the bridge DLLs must sit next to it); that `VirtualAlloc` finds a block within 1 GiB of the DLL image (compiled only; the search goes through 16 MiB steps of the 64 KiB granularity, which fits); the bridges' `_CRT_SECURE_NO_WARNINGS` (one deprecation warning in `spu2_selftest.cpp`); the MSVC C++ runtime is linked dynamically (corrected: the VC++ redistributable was therefore needed unless `/MT` was chosen; resolved by T-770, the zip ships the runtime DLLs app-local, P13); any run under Wine.

## 11. State on 2026-10-14, third piece: the whole game builds and links; the first run does not start

**Build (run).** `build_windows.sh game` (preset `windows-release`, 4 jobs) in 6 slices of at most 230 s each (container lock released between them), 1338 s in total, 1914 steps at the first slice. No compile error and no link error in any slice (`grep -h 'FAILED\|error:'` over the six logs prints nothing), the 72 big files included, inside the 16 GiB limit. `don.exe` is 104,794,112 bytes. `DON_SLICE_SECONDS` and `DON_SKIP_CONFIGURE` in the script exist for this. Nothing needed a change in a source file.

**Imports (measured with `llvm-objdump -p`).** `don.exe`: KERNEL32, SDL3.dll, MSVCP140, VCRUNTIME140, VCRUNTIME140_1 and the `api-ms-win-crt-*` set. `don_vu.dll` and `don_spu2.dll`: the same C++ runtime. `don_gs.dll`: also SDL3, freetype, jpeg62, libpng16, libwebp, lz4, plutosvg, plutovg, ryml, z, zstd and Direct3D (d3d11, d3d12, dxgi, D3DCOMPILER_47). The runtime DLLs (MSVCP140...) are not in the xwin sysroot; on a PC they came from the Visual C++ redistributable (corrected: since T-770 the Windows zip ships them app-local, from the pinned Microsoft redistributable, journal 064), under Wine from Wine itself unless the zip's copies are placed and overridden. The program looks for the bridges in `--gs-dir`, `--vu-dir`, `--spu2-dir` (`don_gs.dll`, ... in `src/host/gs_glue.cpp`).

**Run folder** (`build/win-run`, scratch): `don.exe`, `SDL3.dll` (from `build/win-deps/bin`), `gs/don_gs.dll`, `vu/don_vu.dll`, `spu2/don_spu2.dll`. The GS DLL's own dependencies (above) are not copied: that run was not tried.

**Linux reference.** `DON_TRACE_STATE=60 build/linux-dev/don --headless --sound off --input-script work/det/quit70.txt disc/SLPS_256.75`: exit 0 in 2.4 s, `state blank=60 eeCycle=334387803 iopCycles=39055054 iopInstructions=34431632 ramHash=84f60b981246d942`.

**Wine run.** Private Xvfb `:97`, prefixes under `work/`, never the owner's display. Wine 6.0.3 (the installed one) and Wine 10.0 staging (the binary of `~/.steam/.../GE-Proton10-34/files`, used in place, nothing installed) give the same result: the process dies at start, before any line of the program is printed, with `err:msvcrt:_invalid_parameter` and exception `0xc0000417`. With `+msvcrt` the last calls are the static initializers of the executable, `_initterm returned`, then `_register_thread_local_exe_atexit_callback`, then the invalid parameter. The backtrace is `kernelbase` <- `ucrtbase` <- `don+0x102e` <- the entry point of `don.exe`: the failure is in the C runtime's startup code of the executable, not in the game code. (Wine 6.0.3 additionally leaves a debugger waiting; use `timeout`.) Hypothesis, not verified: a clash between the dynamic-CRT startup of this clang-cl executable and Wine's `ucrtbase`. The remedy to try is the static CRT (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, `/MT`), which also removes the need for the redistributable on a PC. It rebuilds everything (about 22 minutes in slices), so it was not done in this piece (rule: no compiling). A one-file clang-cl hello with the dynamic CRT under Wine would say whether the toolchain or `don.exe` is the cause. The Wine-versus-Linux state comparison, the classifier and the bridges under Wine were not run for that reason.

### 11.1 Why don.exe died at start-up (2026-10-14): `setvbuf`, not the C runtime link

The reading "the runtime start-up is in the image twice" is **false** for this link; the cause is a line of `src/host/main.cpp`.

- **Four one-file programs** (clang-cl, `-MD -O2 -march=x86-64-v3`, same sysroot; scratch `work/t401-hello`): `a` plain hello; `b` with a `thread_local` object that has a destructor; `c` = `b` + linked with SDL3 and calling `SDL_GetVersion` (src/host/main.cpp is a plain `main`, it does not use SDL's main wrapper, so there is none to test); `d` = `b` + a `std::thread`. **All four run correctly** on Wine 6.0.3 and on Wine 10.0 staging (hello, `T dtor`, SDL version 3.4.12). The toolchain, the dynamic CRT, `thread_local` destructors and SDL3 are not the problem.
- **What the link of `don.exe` pulled in** (`/verbose` re-link into a scratch file, `/map`; `llvm-readobj --coff-directives` on unity objects, runtime and host objects): every object asks for `msvcrt.lib`, `oldnames.lib` and `msvcprt.lib` and carries `/FAILIFMISMATCH:RuntimeLibrary=MD_DynamicRelease` (1982 `msvcprt`, 2102 `msvcrt` mentions in the verbose output); one flavour only. `libcmt.lib`, `libucrt.lib` and `libvcruntime.lib` appear once each only as text inside a directive of `msvcrt.lib(initializers.obj)`, never as a file read. `vcruntime.lib` and `ucrt.lib` (the import libraries) are read once. The start-up objects `exe_main.obj` (`mainCRTStartup`) and `tlsdtor.obj` are loaded once from `msvcrt.lib`. All 1966 compile lines have `-MD`. So the C runtime is in the program once.
- **The real cause**: with `+msvcrt` Wine traces CRT calls, and `setvbuf` is not traced, so the last traced call (`_register_thread_local_exe_atexit_callback`, the start-up code's, harmless) only marks where `main` starts. The backtrace frame `don+0x102e` is the first function of the first object of the link, `main()` of `src/host/main.cpp`, whose first statement is `std::setvbuf(stdout, nullptr, _IOLBF, 0)`. The Universal CRT does not implement line buffering (`_IOLBF` is treated as `_IOFBF`) and requires a buffer size of at least 2 for it: size 0 is an invalid parameter, handled by default with the fast-fail `0xc0000417`. It would fail the same way on real Windows. Fix (in `main.cpp`, `#ifdef _WIN32`): `_IONBF` (same guarantee, nothing lost when a run is stopped). Only `main.cpp` was recompiled and `don.exe` re-linked.

### 11.2 After the fix: how far the run gets (Wine 10.0 staging, private Xvfb `:97`)

`DON_TRACE_STATE=60 wine64 don.exe --headless --sound off --input-script work/det/quit70.txt disc/SLPS_256.75` (Wine 6.0.3 has no `wined3d` dependencies for the GS DLL and was not used further). In order, each one found by running:

1. `don_gs.dll` needs its DLLs beside the program: the whole of `build/win-deps/bin` was copied next to `don.exe`, plus `libvkd3d-1.dll`, `libvkd3d-shader-1.dll` (and the rest of `lib/vkd3d/x86_64-windows`) from the Proton Wine in `~/.steam/.../GE-Proton10-34/files`, because that Wine's builtin `wined3d.dll` (imported by `dxgi.dll`, by `D3DCOMPILER_47.dll`) needs them and its prefix did not have them.
2. **Program bug found by Windows**: `HostGsParams::window` (a `DonGsConfig`) was never initialised; on Linux it read as zero, on Windows `don_gs_open` printed `invalid window_type 22746432`. Fixed in `src/host/gs_glue.h` (`DonGsConfig window{};`), host objects rebuilt. This also applies to Linux but changes nothing there (zero was the value it had).
3. The GS needs its `resources` folder beside `don_gs.dll` (`build/windows-gs/don-gs-bridge/resources` copied to `gs/resources`).
4. **Where it stops**: the ELF loads (`ELF file loaded successfully. Entry point: 0x100008`, the override is applied, the floating-point mode line is printed), and then `don_gs_open: GSopen(software) failed: ... Failed to create render device`. PCSX2's Windows GS creates a Direct3D device even for the software renderer; Xvfb under Wine has no GPU. Exit code 3, before the first vertical blank, so there is **no `state blank=60` line on Wine** to compare with Linux (`ramHash=84f60b981246d942` at blank 60 there). `--headless` does not run without the GS DLL: the host requires it.

Not verified: the state comparison, the classifier, the SPU2 and VU bridges under Wine, any run on a real Windows machine. Next piece: either a Wine with a working D3D11 (a GPU or `llvmpipe` through Vulkan for DXVK/vkd3d), or a way to run the GS bridge headless without a render device, or a real Windows machine.

### 11.3 The first frames under Wine (2026-10-14, fourth piece)

Wine 10.0 staging (GE-Proton10-34, used in place), private Xvfb `:97`, private prefix. Script: `work/t401-wine.sh` (not versioned).

- **Device**: `--headless --renderer vulkan` works as it is: the Wine Vulkan layer goes to the host's driver (Radeon RX 6800 XT, Mesa) with no surface, as on Linux. No Direct3D 11 layer or software Vulkan was needed. The earlier failure was the *software* renderer, which on Windows wants a D3D11 device.
- **Classifier**: `--headless --sound off --renderer vulkan --seconds 60`: `RESULT: OK_VSYNC_LOOP 2460 2580` (frames 2460, vsyncs 2580).
- **State line**, `DON_TRACE_STATE=60`, `quit70.txt`: Wine `state blank=60 eeCycle=334387803 iopCycles=39055054 iopInstructions=34431632 ramHash=84f60b981246d942`, identical to Linux, all four fields. The longer route `work/3d/menu_extra.txt` (3300 blanks, `DON_TRACE_STATE=600`) is also identical at every line (e.g. blank 3000: `ramHash=90de0d426bdee3fc`), so floating point and the guest's behaviour agree over title and main menu.
- **Vector unit** (`--vu-dir`): the VirtualAlloc code cache finds a block; `VU1 opened on microVU`. On the title route no program runs (0 started), on the menu route `287421 programs started, 442905238 cycles, 298513 XGKICK packets, 394422144 bytes`, identical to Linux. microVU and the PCSX2 interpreter give the same state line on `quit70`. (Interpreter on the long route not run on Wine.)
- **Sound** (`--sound on --spu2-dir ... --wav`): a 40 s run with no input is silent on both systems; the `menu_extra` route gives a 55.01 s stereo 48 kHz WAV whose SHA-256 is byte-identical to Linux's (peak 27992, 27.5 % non-zero samples).

### 11.4 What a player's PC needs beside don.exe

| File | From | Licence |
|---|---|---|
| `SDL3.dll` | built from the SDL3 source in the prefix | zlib |
| `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll` (+ `api-ms-win-crt-*` / UCRT, already in Windows 10+) | Corrected: the zip ships them app-local (T-770) from the pinned VC++ 2015-2022 x64 redistributable; the table used to say they were not shipped by the repo | Microsoft redistributable (`licenses/Microsoft-VCRedist-license.rtf`) |
| `gs/don_gs.dll` + `gs/resources/` | built from PCSX2 (patches/pcsx2) | GPL-3.0 |
| `vu/don_vu.dll`, `spu2/don_spu2.dll` | built from this repo + PCSX2 code | GPL-3.0 |
| beside `don_gs.dll`/exe: `c4core, freetype, jpeg62, libpng16, libsharpyuv, libwebp*, lz4, plutosvg, plutovg, ryml, shaderc_shared, turbojpeg, z, zstd` DLLs | `build/win-deps/bin` | BSD/MIT/zlib/Apache/FTL (mixed) |

The Wine-only extras (`libvkd3d*.dll`) are not needed on Windows. DLL search for `don_gs.dll`'s dependencies is the exe's folder, so the dependency DLLs go beside `don.exe`. **Launcher**: `tools/launcher/launch.bat` runs `launch.py`, whose default binary on Windows is `build/linux-dev/don.exe` (`launch.py:332`), so it does NOT find `build/win-run/don.exe`; `DON_BIN` points it there. It also would need to pass `--gs-dir/--vu-dir/--spu2-dir` for the `gs/ vu/ spu2/` layout (not checked). Proposal: a Windows default path of `build/win-run/don.exe`.

Not verified: any real Windows machine, the static-runtime build, the launcher run on Windows, microVU vs interpreter on the long route under Wine.

### 11.5 The folder to copy (2026-10-14, fifth piece)

- **The default search already was the layout**: the three `*_glue.cpp` look in `<program>`, `<program>/<name>`, `<program>/../<name>` when no option or `DON_*_DIR` is given. The `--gs-dir/--vu-dir/--spu2-dir` of the earlier runs were never needed for `gs/`, `vu/`, `spu2/` beside the exe. No source change; a test now fixes the order of the places (`test_default_search_...`, runs a copy of the host test program with no library; passes).
- **`tools/pipeline/package_windows.sh`** copies a finished build into `build/windows-package/don-windows` (133 MB: `don.exe` 104.8 MB, `gs/don_gs.dll` 9.4 MB, `gs/resources` 113 files, `shaderc_shared.dll` 6.6 MB, `SDL3.dll` 2.7 MB, the rest small). Player files in `tools/package_windows/`: `README.txt`, `THIRD-PARTY.txt`, `start.bat`, `game_folder.txt`.
- **Launcher decision: a plain `start.bat`, `launch.py` untouched.** `launch.py` needs Python with Tk, `7z` and an ISO to make variant folders: too much for a copied folder, and changing its binary lookup would risk `tests/test_launcher.py` for nothing. `start.bat` reads one line (`game_folder.txt`, or a folder given/dragged as first argument) and runs `don.exe <folder>\SLPS_256.75 --mc-dir saves ...`. English variants: point it at the folder the launcher made.
- **Proof under Wine** (folder copied to a scratch path in `/tmp`, removed after; no library option): with the default sound on, `state blank=60 eeCycle=334387803 iopCycles=38186506 iopInstructions=1328160 ramHash=f88d021904cda385`, identical to Linux's default-sound run; with `--sound off`, `iopCycles=39055054 iopInstructions=34431632 ramHash=84f60b981246d942`, the earlier line. (Sound on changes the IOP counters and hash on both systems: compare like with like.) VU1 opened on microVU from `vu/` with no option. `wine cmd /c start.bat --headless --seconds 8 --sound off` with a `game_folder.txt` (CRLF, with a `#` line) starts the game and creates `saves\mc0`, `mc1`.
- **Wine-only**: the Proton Wine's `wined3d` needs `libvkd3d*.dll`; they were put in my private prefix's `system32`, not in the folder (a Windows PC has `dxgi`, `d3d11`, `D3DCOMPILER_47`).
- **Not verified**: any real Windows; a windowed run (under Xvfb the Vulkan surface cannot create a device: `Failed to create render device`; the headless path is what was measured); `start.bat` on real cmd.exe (Wine's `findstr` ignores `/b`, so the comment-line skip is untested); that the app-local runtime DLLs are all a clean PC lacks (T-770 ran the zip on the owner's PC, HANDOFF.md 2026-10-28; whether that PC had the redistributable installed is not recorded).

### 11.6 The folder with the game of 2026-10-14 (sixth piece)

Rebuilt from `main` at `e3bb5fb` (runtime patches up to 0062 applied in the shared tree; **0063 had not landed**: `patches/ps2recomp/0063-*` did not exist when the last build slice ran and said `no work to do`). The runtime, the host and the SPU2/VU bridges were rebuilt (runtime 104 s; SPU2 and VU seconds); `don_gs.dll` was **not**: `src/gsbridge` and `patches/pcsx2` did not change since the last build (the GS step also refused to run, its cache was made from another PCSX2 folder). The game step was 1876 steps in 5 slices of 230 s (~16 min, 4 jobs; last line `[137/137] Linking CXX executable don.exe`), then one more slice: `ninja: no work to do`. Package: `build/windows-package/don-windows`, 133 MB (`don.exe` 104,800,768 bytes), README brought in line with docs/RUNNING.md (music works, `--players 4`, videos skipped).

Under Wine 10 (copy of the folder in `/tmp`, no library option, `quit70.txt`, `DON_TRACE_STATE=60`), against `build/linux-dev/don` of 11:50 the same day:

| sound | both systems print |
|---|---|
| off | `eeCycle=334387803 iopCycles=38903537 iopInstructions=34280130 ramHash=1bc3b23d1246d942` |
| on  | `eeCycle=334387803 iopCycles=38186506 iopInstructions=1328160 ramHash=f88d021904cda385` |

(The sound-off line is not yesterday's `84f60b98...`: the runtime changed today; the two builds agree with each other.)

Recording of the `work/3d/menu_extra.txt` route to the main menu (`--sound on --wav`, 55 s): **not always byte-identical, on either system.** Three different files appeared (SHA-256 starting `6fd6171d`, `4bb74048`, `7df0067d`; lengths 5280996, 5281140 and 5281042 samples; same peak 28475 and rms 1606 where compared). Linux gave `6fd6` once and `4bb7` twice; Wine gave `7df0` once and `6fd6` twice, so Wine did produce Linux's `6fd6171d...` byte for byte, twice. Where two runs differ they match until a point (4.56 s or 38.97 s) and then drift by some frames: the recording depends on host timing (the machine was loaded by other builds), not on the platform. Whether the SPU2 output is a function of guest time only is for the audio side to settle. The level in the menu (last 15% of the file) is rms 2366 in both, i.e. the menu music is there.

### 11.7 The Windows build under Wine 9 + lavapipe, and the friend's bug (2026-10-16, journal 041)

A friend ran the package on real Windows (RTX 3060): after Naruto's ougi cutscene in a 4-player fight the
textures corrupt (HUD garbage/noise tiles, stage surfaces white/grey, characters scrambled) and the game runs
at ~0.5× with broken sound. To reproduce it here, a Wine container was built (nothing installed on the host):
`tools/container/Dockerfile.wine` (`don-wine:1`, ubuntu:24.04 pinned by digest, wine 9.0~repack-4build3,
mesa-vulkan-drivers 25.2.8/lavapipe, vulkan-tools, xvfb) and `tools/container/run_wine.sh` (mount, uid:gid,
memory limit, `build/.wine.lock`). The package runs headless on a private Xvfb, `--renderer vulkan`,
`VK_ICD_FILENAMES=…/lvp_icd.json`, sound via `--wav`.

Measured against Linux (`build/player/don-linux/don`, same commit `cfcd786c`), all Vulkan:

| Route | Windows-under-Wine vs Linux |
|---|---|
| `default_title_idle` | 4 landmarks byte-identical; WAV byte-identical (5 604 351 frames) |
| `fight_practice` | 6 landmarks byte-identical; WAV byte-identical (6 322 789 frames) |
| `multiplayer_ougi` (4-player + ougi, 41 shots/30 blanks 6600–7800) | first diff at blank 6780; 4/41 differ (6780, 6840, 7500, 7530), each exactly 1 pixel, max delta 1; the other 37 byte-identical; Linux-RADV differs from Linux-lavapipe on all 41 (driver difference) |

The friend's texture corruption and half speed are **not reproduced**: the only Windows-vs-Linux difference is a
1-pixel off-by-1 floating-point rounding in four cutscene frames, with no persistent corruption after the cutscene.
What still differs on the friend's PC: the NVIDIA Vulkan driver (vs lavapipe/RADV), real-Windows thread/timing (the
GS bridge's guest-memory-pointer reuse), and the ~0.5× speed. Wine 9 quirks (scratch, not game bugs): it cannot
create the PCSX2 data directory non-recursively (`gs/data/PCSX2/inis` must pre-exist), the `wine` process hangs at
teardown after a clean exit, and the software renderer is still unusable under Wine (D3D11 headless). The script
`tools/run/scripts/multiplayer_ougi.txt` is the 4-player ougi route.
