# 12 · Building the PCSX2 pieces (GS, later microVU and SPU2)

Audit for task T-103. Nothing was built. Everything below comes from reading the pinned PCSX2
tree and the two reference projects.

**Conventions.** **Measured** = read in the PCSX2 sources at the pinned commit (or run on this
machine) by this audit. **Declared** = stated in the documentation or scripts of another project
(Fukami, Killzone); not re-checked. **Inference** = a conclusion drawn here, not tested.

Paths are relative to the root of each tree: `P/` = `third_party/pcsx2` (v2.8.2, commit
`fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`), `F/` = Fukami at `e881812`, `K/` = KillzoneRecomp at
`5f07000` (cloned for reading into `/tmp/opencode/refs/`, never executed).

## Source tree

| Fact | Value | Source |
|---|---|---|
| Fetch | `bash tools/setup/fetch_pcsx2.sh` (shallow fetch of exactly the pinned commit, ~5 s) | **Measured** |
| Size of the clone | 145 MB apparent (`du -sh --apparent-size`), 151 MB on disk; of that `.git` 21 MB, `3rdparty/` 44 MB, `bin/` 9.9 MB, `pcsx2/` 13 MB. 2,934 files tracked | **Measured** |
| Submodules | **None.** `.gitmodules` exists but is 0 bytes and `git ls-tree -r HEAD` has no entry of mode `160000`. The third-party code PCSX2 needs for the GS is vendored in `P/3rdparty/` (fmt, imgui, xbyak, cpuinfo, lzma, simpleini, vulkan headers, zydis, ...). So the fetch script initialises nothing | **Measured** |
| Not vendored | The libraries searched with `find_package` (next sections): they are not in the tree and must come from the container | **Measured** (`P/cmake/SearchForStuff.cmake:14-31`) |
| Game resources | The GS needs `P/bin/resources` (shaders, fonts); it is in the tree, no download. The "patches.zip" the CI downloads (`P/.github/workflows/linux_build_qt.yml:31-34`) is for game patches, not needed | **Measured** |

## Compiler and CMake

| Question | Answer | Source |
|---|---|---|
| Supported compilers | Clang (and AppleClang), clang-cl and MSVC. GCC is detected but flagged `IS_SUPPORTED_COMPILER FALSE`: it only prints a warning "PCSX2 only supports the Clang and MSVC compilers... continue at your own risk" and continues | **Measured** `P/cmake/Pcsx2Utils.cmake:25-44`, `P/CMakeLists.txt:74-79` |
| Minimum compiler version | **Not checked anywhere in the CMake files** (no `COMPILER_VERSION` test). The only version evidence is what CI uses: **clang-17 + lld-17** from the apt.llvm.org `jammy` repository, on `ubuntu-22.04` | **Measured** `P/.github/workflows/linux_build_qt.yml:15` (os), `:103-104`, `:107`, `:113` (packages), `:139-142` (`CMAKE_C_COMPILER=clang-17`, `-fuse-ld=lld`) |
| Language standard | C++20, required (`CMAKE_CXX_STANDARD 20`, `REQUIRED ON`). The code uses `<bit>` (`std::countl_zero`) and `std::span` | **Measured** `P/cmake/BuildParameters.cmake:145-146`, `P/common/BitUtils.h:8,85`, `P/common/FileSystem.h:12` |
| Standard library | Not pinned. `std::format` is not used (no match in `common/` or `pcsx2/`), so the libstdc++ 12 of Ubuntu 22.04 (GCC 12.3) is enough for what was checked. The host has `libstdc++.so.6` with `GLIBCXX_3.4.30` (GCC 12) | **Measured** (grep; `strings` on the host library) |
| Linker | `lld` (CI and `CMakePresets.json` pass `-fuse-ld=lld`) | **Measured** `P/CMakePresets.json:17-19`, `P/.github/workflows/linux_build_qt.yml:141-142` |
| Minimum CMake | `cmake_minimum_required(VERSION 3.16...3.25)`. CI installs the Ubuntu 22.04 package (3.22.1). With DISABLE_ADVANCE_SIMD, CMake ≥ 3.24 links the per-ISA GS libraries with `WHOLE_ARCHIVE`; older versions use a plain link (fatal only on macOS) | **Measured** `P/CMakeLists.txt:3`, `P/pcsx2/CMakeLists.txt:778-786`, `P/.github/workflows/linux_build_qt.yml:107` |
| Generator | Ninja (CI and presets). No in-tree build (configure aborts) | **Measured** `P/CMakeLists.txt:9-11`, `P/.github/workflows/linux_build_qt.yml:135` |
| Languages enabled | `C CXX`; on Windows additionally `ASM_MASM` for `common/FastJmp.asm` | **Measured** `P/CMakeLists.txt:14`, `P/common/CMakeLists.txt:129-131` |
| Target CPU | x86-64 selected from `CMAKE_HOST_SYSTEM_PROCESSOR`. With `DISABLE_ADVANCE_SIMD=ON` the flags are `-msse -msse2 -msse4.1 -mfxsr` and the GS code is built three times (sse4, avx, avx2) with run-time dispatch; without it, `-march=native` (not acceptable for a distributable build) | **Measured** `P/cmake/BuildParameters.cmake:84-109`, `P/pcsx2/CMakeLists.txt:750-790` |

Compilers present on this machine (**Measured**, `apt-cache policy` on the host): `clang` 14.0
(`1:14.0-55~exp2`), `clang-15` from a third-party PPA, no `clang-17`. The host's own GCC is 11
(D11). None of them is what PCSX2 CI uses, hence the container. Fukami builds with the clang of
Ubuntu 24.04 (**Declared**, `F/tools/linux-build/Dockerfile:6-9`, `CC=clang` at `:23`); the
Killzone README only names MSVC 14.51 (**Declared**, `K/ext/kzgs/README.md:15`).

## What the build container needs (input for T-104)

**Base image.** `ubuntu:22.04` (glibc 2.35, **Measured** with `ldd --version` on the host:
`Ubuntu GLIBC 2.35-0ubuntu3.15`). This also is the OS on which PCSX2's own CI builds
(`P/.github/workflows/linux_build_qt.yml:15`). Fukami uses 24.04 / glibc 2.39 to target SteamOS
(**Declared**, `F/BUILDING.md:100`); that is not our constraint. Libraries linked into the
executable must not require a glibc symbol newer than 2.35.

**Compiler.** `clang-17` and `lld-17` from `apt.llvm.org` (jammy) as CI does; `libstdc++-12-dev`
(the newest Clang auto-selects, **Measured** candidate `12.3.0-1ubuntu1~22.04.3`). Clang ≥ 17 is
the only version with evidence; nothing older was tested. Because D11 allows static linking of
the C++ library into the bridge libraries, record `GLIBCXX_3.4.30` as the floor that
the host already provides.

**Dependencies for a build without Qt**, with the versions of the pinned PCSX2 CI script
(`P/.github/workflows/scripts/linux/build-dependencies-qt.sh:21-42`; Fukami copies the same
numbers: `F/tools/linux-build/build-deps.sh:10-16`, **Declared**). "Ubuntu 22.04" column: version
offered by this host's apt (**Measured**; the host is elementary OS 7.1, based on 22.04, with some
extra repositories, so a few `-dev` versions such as Vulkan may differ in a clean `ubuntu:22.04`).

| Dependency | What PCSX2 demands | CI / Fukami version | Ubuntu 22.04 | Decision |
|---|---|---|---|---|
| libpng | `>= 1.6.40` (`SearchForStuff.cmake:14`) | 1.6.58 + APNG patch | 1.6.37 | **Source** |
| zstd | `>= 1.5.5` (`:17`) | 1.5.7 | 1.4.8 | **Source** |
| SDL3 | `>= 3.2.6` (`:20`) | 3.4.12 | not packaged | **Source** |
| plutovg | `>= 1.1.0` (`:22`) | 1.3.2 | not packaged | **Source** |
| plutosvg | `>= 0.0.7` (`:23`) | 0.0.7 | not packaged | **Source** |
| rapidyaml (`ryml`) | any (`:24`) | 0.12.1 | not packaged | **Source** |
| shaderc | any, only if Vulkan (`:29-31`); CI patches it | 2026.2 + glslang `275822a`, SPIRV-Headers `58006c9`, SPIRV-Tools `6337eb6` + `shaderc-changes.patch` | not packaged | **Source** (patch in `P/.github/workflows/scripts/common/shaderc-changes.patch`) |
| libbacktrace | required when `USE_BACKTRACE=ON` (default, `SearchForStuff.cmake:75-77`, `BuildParameters.cmake:33`) | commit `ad106d5` | no `-dev` package | **Source**, or `-DUSE_BACKTRACE=OFF` (**Inference**, not tried) |
| libwebp | any (`:19`) | 1.6.0 | 1.2.2 | apt works; **Source** to match CI |
| libjpeg-turbo | any (`:15`) | 3.2.0 | 2.1.2 | apt works; **Source** to match CI |
| lz4 | any (`:18`) | 1.10.0 | 1.9.3 | apt works; **Source** to match CI |
| zlib | any (`:16`) | system | 1.2.11 | apt |
| FreeType | `>= 2.10` (`:21`) | system | 2.11.1 | apt |
| libcurl, libpcap, fontconfig, dbus-1, libudev | `find_package`/`pkg_check_modules` (`:41-42,57,59,80`) | system | present (`libcurl4-openssl-dev` 7.81, `libpcap-dev` 1.10.1, `libfontconfig-dev` 2.13.1, `libdbus-1-dev` 1.12.20, `libudev-dev` 249) | apt |
| X11 + XRandR | required when `X11_API=ON` (`:62-67`) | system | `libx11-dev`, `libxrandr-dev` | apt |
| Wayland + ECM + EGL | required when `WAYLAND_API=ON` (`:69-73`) | system | `libwayland-dev` 1.20, `extra-cmake-modules` 5.92, `libegl-dev` | apt |
| Vulkan headers | **bundled** in the tree (`VK_HEADER_VERSION 356`, `P/3rdparty/vulkan/include/vulkan/vulkan_core.h:69`); Vulkan is loaded at run time with `LIBVULKAN_PATH` override (`P/pcsx2/GS/Renderers/Vulkan/VKLoader.cpp:39-53`) | CI installs Vulkan-Headers 1.4.328.1 only inside the FFmpeg step (`build-dependencies-qt.sh:112-118`); Fukami installs it always (`build-deps.sh:59`) | n/a | **Not needed** for the GS |
| FFmpeg | optional; falls back to the bundled headers with a warning (`SearchForStuff.cmake:47-51`) | 8.1, not needed | not needed | **Skip** |
| Qt 6 / KDDockWidgets | only with `ENABLE_QT_UI=ON` (`:114-125`) | 6.11.1 / 2.4.1 | n/a | **Skip** (`ENABLE_QT_UI=OFF`) |
| Tools | `cmake`, `ninja`, `git`, `pkg-config`, `patch`, `curl`/`ca-certificates`, `python3` | | cmake 3.22.1, ninja 1.10.1 | apt (`nasm`, `patchelf` only needed by CI's FFmpeg/AppImage steps) |

Everything built from source goes to one prefix passed as `CMAKE_PREFIX_PATH` (and
`PKG_CONFIG_PATH`), as both CI and Fukami do. Fukami reports about 4 GB of disk for dependencies
plus builds (**Declared**, `F/BUILDING.md:18`).

## CMake options for Linux (Fukami)

From `F/scripts/build_pcsx2_gs_bridge.sh:107-127` (**Declared**), with what each one does in the
pinned tree (**Measured**):

| Option | Value | Effect in PCSX2 v2.8.2 |
|---|---|---|
| `-G Ninja`, `-DCMAKE_BUILD_TYPE=Release` | | `BuildParameters.cmake:55` accepts `Debug|Devel|MinSizeRel|RelWithDebInfo|Release` |
| `ENABLE_QT_UI` | `OFF` | `BuildParameters.cmake:10`; skips `pcsx2-qt` (`CMakeLists.txt` adds it only if ON) |
| `ENABLE_TESTS` | `OFF` | `BuildParameters.cmake:9`; skips googletest |
| `USE_VULKAN` | `ON` | `:24`; D4 uses Vulkan on both systems |
| `USE_OPENGL` | `OFF` | `:22`; removes glad |
| `X11_API`, `WAYLAND_API` | `ON`, `ON` | `:31-32`; window handles for both |
| `DISABLE_ADVANCE_SIMD` | `ON` | multi-ISA GS (sse4/avx/avx2) instead of `-march=native` (`BuildParameters.cmake:84-109`) |
| `ENABLE_SETCAP` | `OFF` | `:30`; no `setcap` on the binary |
| `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` | `-ffile-prefix-map=...` | strips builder paths from the binaries (`F/scripts/build_pcsx2_gs_bridge.sh:177,185`) |

The PCSX2 CI line adds `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`, `CMAKE_DISABLE_PRECOMPILE_HEADERS=ON`,
`USE_LINKED_FFMPEG=ON` and ccache (`linux_build_qt.yml:135-149`); those are optional for us.
**Conflict with AGENTS.md to resolve in T-104:** the repository rule "no `-ffast-math`,
`-ffp-contract=off`" applies to the game code; PCSX2 sets its own flags (`/fp:contract` on MSVC,
`P/pcsx2/CMakeLists.txt:25`) and the GS is not game code, so the GS libraries are not built with the
repository flags unless T-104 decides otherwise.

## Integration style A: Fukami: bridge built inside the PCSX2 CMake project

**Declared** (Fukami docs) and **Measured** (patch text read; PCSX2 side checked).

| Aspect | What Fukami does | Source |
|---|---|---|
| Pin | PCSX2 v2.8.2, commit `fd9d310c`, the same pin as ours | `F/tools/patches/pcsx2/README.md:3` |
| Clean checkout | PCSX2 is never modified in place: clone `--no-local --no-checkout`, `checkout --detach <rev>`, then `git apply --index` of the ordered series | `F/scripts/build_pcsx2_gs_bridge.sh:134-171`, `F/BUILDING.md:39-40` |
| The hook (patch `0001`) | In `P/CMakeLists.txt`, after `add_subdirectory(pcsx2)`: `if(DEFINED RRV_PCSX2_GS_BRIDGE_SOURCE_DIR) ... add_subdirectory("${RRV_PCSX2_GS_BRIDGE_SOURCE_DIR}" pcsx2-gs-bridge) endif()`. 15 added lines in the top-level CMake file | `F/tools/patches/pcsx2/0001-bridge-target-and-local-memory-entry-points.patch` (diff lines 1-26) |
| Rest of `0001` | New GS entry points to read and write local memory (`GS.cpp`, `GS.h`, `GSState.*`, `GSLocalMemory.h`) and a config bit in `Config.h` | same file; description in `F/tools/patches/pcsx2/README.md:19` |
| Bridge target | `add_library(rrv-pcsx2-gs-bridge SHARED bridge.cpp gsrunner_host.cpp ...)`, linked `PRIVATE PCSX2_FLAGS PCSX2`, include dirs of the PCSX2 tree, `CXX_VISIBILITY_PRESET hidden`; the `bin/resources` directory is copied next to the library post-build | `F/tools/pcsx2-gs-bridge/CMakeLists.txt:4-6,35-58` |
| Host layer | `gsrunner_host.cpp` supplies the `Host::` functions PCSX2's GS needs; it is derived from PCSX2's own `pcsx2-gsrunner/Main.cpp` ("upstream GPL-3.0+") | `F/tools/pcsx2-gs-bridge/CMakeLists.txt:1-3` |
| Other patches | `0004` (`GSState::Transfer` with split PACKED data) and `0005` (texture cache scratch capacity) are generic fixes; `0003`/`0010` replace the software renderer's JIT; the rest is macOS/Metal or Ridge Racer V | `F/tools/patches/pcsx2/README.md:17-29` |
| Presentation | The bridge is built for Vulkan; Linux gets an extra surface adapter (`linux_surface_adapter.cpp`) | `F/tools/pcsx2-gs-bridge/CMakeLists.txt:24-33` |

**Measured on the PCSX2 side that makes this possible.** PCSX2 already has a Qt-free host
executable built on exactly this shape: `pcsx2-gsrunner` links `PCSX2_FLAGS PCSX2`
(`P/pcsx2-gsrunner/CMakeLists.txt:18-21`), is added with `EXCLUDE_FROM_ALL` unless
`ENABLE_GSRUNNER=ON` (`P/CMakeLists.txt:67-71`). The bridge is a second such consumer. But the
`PCSX2` target is the whole emulator (EE, IOP, VU, DEV9, USB, CDVD, ... ~358 `.cpp` entries in
`P/pcsx2/CMakeLists.txt`), so the bridge library carries all of it unless the link drops it.
With DISABLE_ADVANCE_SIMD the `PCSX2` target is an OBJECT library (`P/pcsx2/CMakeLists.txt:4-9`).

## Integration style B: Killzone: selected PCSX2 sources compiled outside the tree

**Declared** (all of this is in the Killzone README and CMake file); the Killzone CMake file was
read in full, it was not built.

| Aspect | What Killzone does | Source |
|---|---|---|
| Tree | `ext/pcsx2` is a depth-1 clone of PCSX2 master, verified against `646df006a4`; "kzgs does not modify anything in `ext/pcsx2`". **No patch series on PCSX2** | `K/ext/kzgs/README.md:7-8`, `K/ext/kzgs/CMakeLists.txt:16-17` |
| Result | One static library `kzgs.lib` (`add_library(kzgs STATIC ...)`), ~160 translation units, 165 Ninja steps, ~87 MB with debug info | `K/ext/kzgs/README.md:29`, `K/ext/kzgs/CMakeLists.txt:299-309` |
| Sources taken from the tree | 4 groups of object libraries: `kzgs_3rdparty` (fmt, imgui, d3d12memalloc, simpleini, cpuinfo, lzma: `CMakeLists.txt:154-180`), `kzgs_common` (`common/*.cpp` plus `Windows/WinThreads.cpp`, `WinHostSys.cpp`, `WinMisc.cpp`, `FastJmp.asm`: `:190-215`), `kzgs_gs` (the `GS/` list at `:223-288`: GS core, HW and SW renderers, D3D11, D3D12, Vulkan; `Pcsx2Config.cpp`, `SourceLog.cpp`), and `shim/*.cpp` |
| Replacement files (`shim/`) | `HostShim.cpp` (166 lines: `Host::` window, OSD, settings), `EmuShims.cpp` (154 lines: stubs for `VMManager`, MTGS, SPU2, USB, Pad, `eeHw`, `SysMemory::GetCodePtr` with a 64 MB RWX region for the SW renderer's JIT, ...), `ImGuiShim.cpp` (87 lines), `kzgs_imconfig.h`, `kzgs_internal.h`. Settings getters always return defaults; configuration only through `KzgsConfig` | `K/ext/kzgs/README.md:168-186`, line counts measured with `wc -l` |
| ImGui isolation | A generated header renames ~1,760 ImGui identifiers to `Kz<name>` and is force-included (`/FI`) in every kzgs C++ file, so the game can link its own ImGui | `K/ext/kzgs/README.md:188-199`, `K/ext/kzgs/CMakeLists.txt:94-148` |
| ISA | One ISA fixed at configure time (`KZGS_ISA`, default AVX2); PCSX2's run-time multi-ISA dispatch is not used. The "unshared" per-ISA sources are compiled once | `K/ext/kzgs/CMakeLists.txt:18,253-263`, `K/ext/kzgs/README.md:251-252` |
| Flags | All private: `/permissive-`, `/Zc:preprocessor`, `/arch:AVX2`, C++20, PCSX2 defines (`_M_X86=1`, `__SSE4_1__=1`, `ENABLE_VULKAN`, ...) | `K/ext/kzgs/README.md:49`, `K/ext/kzgs/CMakeLists.txt:40-69` |
| Toolchain | MSVC x64 only; the CMake file aborts otherwise (`if(NOT MSVC ...) FATAL_ERROR "kzgs only supports MSVC x64."`) | `K/ext/kzgs/README.md:15`, `K/ext/kzgs/CMakeLists.txt:11-13` |
| Known gaps | No local-to-host transfers (`TRXDIR=1` reads), no savestates, one instance per process, `SIGNAL/FINISH/LABEL` must be handled by the caller | `K/ext/kzgs/README.md:181,245-260` |

## Windows

| Question | Answer | Source |
|---|---|---|
| Which dependency package does PCSX2 use? | The Visual Studio solution (`PCSX2_qt.sln`, `.vcxproj` files with `$(DepsIncludeDir)`) consumes a prebuilt dependency package; the official one is the repository `pcsx2/pcsx2-windows-dependencies`, rebuilt when a PR labelled `requires-win-deps-build` is merged. The CI also has a script that builds the same set from source: `build-dependencies.bat` (needs Visual Studio, 7-Zip and Git's `patch`/`bash`) | **Measured** `P/.github/workflows/windows_deps_dispatch.yml:9-17`, `P/pcsx2/pcsx2.vcxproj:34-36`, `P/.github/workflows/scripts/windows/build-dependencies.bat:6-30`; **Declared** that the release asset is `pcsx2-windows-dependencies.7z` (~170 MB): `K/ext/kzgs/README.md:9-11,19-21` |
| Versions in that script | Same as Linux plus: FreeType 2.14.3, HarfBuzz 14.2.0, zlib 1.3.2, **DirectX-Headers 1.619.1**, **Agility SDK 1.619.2** (PCSX2 requires DirectX-Headers `>= 1.618.1`) | **Measured** `build-dependencies.bat:78-100`, `P/cmake/SearchForStuff.cmake:25-27` |
| Does PCSX2 build with CMake on Windows? | Yes, as a CI configuration named `CMake`: `vcvars64.bat`, `cmake . -B build -DCMAKE_PREFIX_PATH=...\deps -DQT_BUILD=ON -DCMAKE_BUILD_TYPE=Release -DDISABLE_ADVANCE_SIMD=ON -G Ninja`, then `cmake --build`. The MSBuild solution is the other (default) configuration | **Measured** `P/.github/workflows/windows_build_qt.yml:137-150`; the other configuration is the MSBuild solution (`:153`) |
| Windows-only sources and libraries | `P/common/Windows/*`, `FastJmp.asm` (MASM), D3D11/D3D12 renderers, `D3D12MemAlloc`, `winpixeventruntime`, `winwil`, DEV9 Win32 pcap, `rainterface` | **Measured** `P/cmake/SearchForStuff.cmake:34-38,128-130`, `P/common/CMakeLists.txt:128-131`, `P/pcsx2/CMakeLists.txt:666-673` |
| What Killzone had to replace | It does not use PCSX2's CMake or PCSX2's Windows project at all. It rebuilt the source list by hand (`kzgs_common` takes only the Windows files it needs), supplied the whole `Host::`/`VMManager`/MTGS/SPU2/Pad/USB surface as shims, fixed ImGui symbols, removed multi-ISA dispatch, linked the dependency bundle's `.lib` files with `/DELAYLOAD` and copies its DLLs next to the executable (`zstd`, `libpng16`, `z`, `libwebp`, `libsharpyuv`, `jpeg62`, `dxcompiler`, `shaderc_shared`) | **Declared** `K/ext/kzgs/README.md:49-62,168-186`, `K/ext/kzgs/CMakeLists.txt:317-343` |
| Wine / cross-compile | Neither reference project tests under Wine or cross-compiles. Fukami has no Windows build (`F/BUILDING.md:10`); Killzone supports MSVC only (`K/ext/kzgs/CMakeLists.txt:11-13`) | **Declared** |

### What D13 (Windows cross-built from Linux, tested under Wine) implies

Open per D13: the toolchain (`clang-cl` + `xwin` or `llvm-mingw`). PCSX2 supports `clang-cl` and
MSVC and does not list MinGW (`P/cmake/Pcsx2Utils.cmake:26-40`, "only supports the Clang and MSVC
compilers", `P/CMakeLists.txt:79`).

| | Style A (Fukami, in-tree CMake hook) | Style B (Killzone, out-of-tree list + shims) |
|---|---|---|
| Cross-compile with `clang-cl` + `xwin` | Uses PCSX2's own Windows CMake path (the `CMake` CI configuration, Ninja, `vcvars64`): needs a CMake toolchain file with the xwin sysroot, `CMAKE_ASM_MASM_COMPILER` for `FastJmp.asm` (`llvm-ml` instead of `ml64`), and the Windows dependency prefix in `CMAKE_PREFIX_PATH`. Compiles the *whole* `PCSX2` target for Windows, including DEV9, USB, CDVD, ... (Inference: larger cross-build surface and more chance to hit an MSVC-only construct) | Only ~160 translation units and our own CMake file, which T-400 can drive with the same toolchain file. But the file is MSVC-only today (`K/ext/kzgs/CMakeLists.txt:11-13`, `/FI`, `/arch:AVX2`, `ASM_MASM`) and would need `clang-cl` spellings of every flag. We would maintain the source list per platform |
| `llvm-mingw` | Not a PCSX2-supported compiler; the CMake path may or may not configure (Inference, untested) | Own CMake file, so possible, but D3D12/Windows headers and `winwil` expect MSVC-style SDK headers (Inference, untested) |
| Native Windows build by the end user (D13 constraint) | Same CMake, same preset: PCSX2 runs it natively in its CI (`windows_build_qt.yml:137-150`) | Same CMake, but needs Visual Studio's MSVC; Killzone's only supported toolchain |
| Under Wine, headless, software renderer | Fukami's patches `0003`/`0010` take the SW rasterizer off the VM-wide JIT code map; they would be in `patches/pcsx2/` anyway for the Linux bridge | Killzone's shim reserves a 64 MB RWX region near the executable for the SW renderer's JIT (`EmuShims.cpp`, README line 180); needs RWX `VirtualAlloc` under Wine (Inference: expected to work, untested) |
| Dependency DLLs | Dependency prefix from `build-dependencies.bat` or the official bundle; `shaderc` must be a DLL on the Wine path for Vulkan; D3D12 needs `dxcompiler.dll` (neither is needed for the SW renderer, **Declared** `K/ext/kzgs/README.md:62`) | Same bundle, `/DELAYLOAD`ed so a run that touches only one backend loads none of the optional ones |

**Wine and the renderers.** D13 says the Wine run is headless with the software renderer. In
the PCSX2 tree the renderers are chosen at run time, the SW renderer is always compiled
(`K/ext/kzgs/CMakeLists.txt:220-222` says it must be, because `GS.cpp` references it) and its code
is generated at run time by xbyak (**Measured**: `P/pcsx2/GS/Renderers/Common/GSFunctionMap.cpp:16-17`
uses `SysMemory::GetSWRec()`/`GetSWRecEnd()` as the JIT area). That area is the one thing a
bridge has to provide, in either style.

## Recommendation

Use **style A** (Fukami's): a patch `patches/pcsx2/0001-...` that adds the `add_subdirectory` hook,
with the bridge sources living in `src/gsbridge/` in this repository and written once for both
systems; clean pinned clone plus the ordered `series` (already what D7 and `fetch_pcsx2.sh`
implement). It keeps PCSX2's own source list, per-ISA dispatch and compile flags, which upstream
maintains for both Linux and Windows (`windows_build_qt.yml:137-150` builds the very same CMake project
on Windows), so the bridge code is identical on both systems and only the toolchain file differs.
Style B is cheaper per build but would put us in charge of a hand-written source list, a shim of
every `Host::` function and a single-ISA GS for each platform, and its only existing consumer is
MSVC-only and pinned to a different commit (`646df006a4` vs `fd9d310c`).

The risk to measure first, in T-104/T-400: that the `PCSX2` target compiles and links for
Windows with `clang-cl` + `xwin` from Linux. If that proves intractable, style B's source list
(`K/ext/kzgs/CMakeLists.txt:223-288`) is the fallback for Windows only, and the bridge code behind the
C API would not change.

## Not verified by this audit

- Nothing was compiled or configured: CMake was not run, so version floors that are only detected at
  configure time are those in the `find_package` lines above.
- That Clang older than 17 fails or works; only 17 has evidence.
- That `-DUSE_BACKTRACE=OFF` configures cleanly.
- That the bridge links without dragging in the whole emulator, and its size.
- Anything about `clang-cl` + `xwin` or `llvm-mingw` building PCSX2, or about Wine running the result.

## What was built (T-105, T-106)

**Measured** on 2026-10-11. The audit above recommended style A; this is what it turned into.

| Piece | Where | Notes |
|---|---|---|
| CMake hook | `patches/pcsx2/0001-bridge-cmake-hook.patch` | `-DDON_GS_BRIDGE_SOURCE_DIR=<repo>/src/gsbridge` makes PCSX2's project build our directory against its internal targets |
| Generic GS fixes from Fukami | `patches/pcsx2/0002`, `0003` | GIF PACKED data split across transfers; texture cache scratch buffer. Still needed at v2.8.2. Not yet exercised by a real dump |
| Executable memory for the software renderer | `patches/pcsx2/0004-gs-software-renderer-own-code-memory.patch` | Without the virtual machine there is no code area for the rasterizer's generated code; the patch gives it its own block. Written for this project (Fukami avoids the problem by another route). The Windows branch has not been compiled |
| Library | `build/gs/libdon_gs.so` (33 MB) | Built by `tools/pipeline/build_gs.sh` inside the container in about 40 s from clean. Exports only `don_gs_*`. C++ runtime linked statically |
| Test program | `build/gs/gsreplay` | Uses only `src/gsbridge/don_gs.h`. `--synthetic` draws a known scene; `tests/gs` checks its pixels |

Facts that the next tasks depend on:

- **The library contains the whole emulator.** The `PCSX2` target is one set of 296 objects,
  so the GS brings the EE, IOP, VU, SPU2 and device code with it. Nothing but the GS is
  initialised. (Corrected on 2026-10-11: this first concluded that the SPU2 and microVU "need
  an API, not another build". Both tasks found the opposite: PCSX2's SPU2 and microVU read
  global state of the emulator that is not initialised here, so each was built as its own
  small library from PCSX2's unmodified sources with replacement headers:
  `build/spu2/libdon_spu2.so`, 492 KB, T-301; `build/vu/libdon_vu.so`, 2.5 MB, T-204.)
- **Host layer.** PCSX2's own `pcsx2-gsrunner/Main.cpp` is compiled into the library (with its
  `main` and window functions renamed) to provide the `Host::` functions. The window surface
  is supplied by our code (`src/gsbridge/don_gs_host.cpp`).
- **Vulkan is needed even for the software renderer**: the device presents the frame. The
  build container has Mesa's software Vulkan driver for that; on the host both renderers pass.
- PCSX2 loads `libshaderc_shared.so.1` at run time to compile its shaders; it must sit next to
  the library, like the other libraries copied from the image (`$ORIGIN` run path).
- **SIGNAL, FINISH and LABEL are not handled by PCSX2's GS** (its GIF unit does that, and it is
  outside this library). Whoever feeds the bridge must track them and the CSR bits (T-108).
- **After `don_gs_load_state` the hardware renderer shows nothing until something is drawn**:
  it does not rebuild its display target from local memory. The software renderer shows the
  picture at once. A dump replay must draw the dump's frame before taking the image (T-107).
- The state block that `don_gs_load_state` takes is the one inside a PCSX2 GS dump (version
  word, drawing registers, 4 MiB of local memory, transfer state; 4,194,813 bytes at version
  9), without the privileged registers. **Unverified** against a real dump: only a round trip
  with `don_gs_save_state` was tested.
- PCSX2's game database has no graphics fixes for `SLPS-25675`, so none is applied.
- Times on this machine for the synthetic scene: opening the GS 40 to 170 ms, one frame 6 to
  60 ms depending on renderer and driver (the first Vulkan frame is slower: it compiles
  pipelines). They say nothing yet about the cost of a real frame of the game.
