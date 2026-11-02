# Third-party notices

*Battle Stadium D.O.N for PC* is licensed under the **GNU General Public License v3.0 or later** (see
[LICENSE](LICENSE)). It contains, links or is built with the pieces below. Each keeps its own licence and
copyright; nothing here changes them. Exact pinned versions and commits are in `tools/setup/` and in
`patches/*/series`.

## What the program is made of

| Piece | Used for | Licence | Source |
|---|---|---|---|
| **LAS Recomp / PS2Recomp** (recompiler, runtime, IOP emulator; the project's patches are in `patches/ps2recomp/`) | translates the game's code and runs it | **GPL-3.0** (the `LICENSE` file of the checkout pinned by `tools/setup/fetch_ps2recomp.sh`). Our patches are GPL-3.0-or-later | https://github.com/ran-j/PS2Recomp |
| **PCSX2** (GS renderer, microVU / VU interpreter, SPU2, Dear ImGui copy; patches in `patches/pcsx2/`) | graphics, vector units, sound chip | GPL-3.0-or-later; its bundled libraries keep their own licences (see its `3rdparty/`) | https://github.com/PCSX2/pcsx2 |
| **Dear ImGui** 1.92.x (PCSX2's vendored copy, plus the SDL3 backends in `src/host/ui/imgui_backends/`) | start screen and options | MIT | https://github.com/ocornut/imgui |
| **LAS Recomp launcher design** (palette, layout) | look of the start screen | the owner's own project, GPL-3.0; only the design is reused | — |
| **LLVM / clang** 17 and lld (inside the build image) | compiles the generated code and the program | Apache-2.0 with LLVM exceptions | https://llvm.org |
| **llvm-mingw**, **xwin** (Windows cross build) | Windows toolchain, Microsoft CRT / SDK headers fetched at build time | llvm-mingw: Apache-2.0 and MIT-style; xwin: MIT or Apache-2.0. The Microsoft CRT and SDK are **not redistributed** by this project and remain under Microsoft's terms | https://github.com/mstorsjo/llvm-mingw , https://github.com/Jake-Shadle/xwin |
| **FFmpeg** 7.1.1 (unmodified official tarball, SHA-256 pinned in `tools/setup/build_ffmpeg.sh`) | MPEG-1/2 video decoding for the game's videos | **LGPL-2.1-or-later**. Configuration: no `--enable-gpl`, no `--enable-nonfree`, no external libraries; only the MPEG-1/2 video decoder, its parser, the MPEG-PS demuxer and swscale are enabled. Statically linked into a GPL-3.0 program, which LGPL allows; its source is the pinned tarball, and our configure line is in the two build scripts | https://ffmpeg.org |
| **Nunito** (Regular, ExtraBold) | font of the start screen, embedded | **SIL Open Font License 1.1** (copy: `src/host/ui/fonts/OFL.txt`, copyright 2014 The Nunito Project Authors) | https://github.com/googlefonts/nunito |
| **SDL3** | window, input, audio output | zlib | https://libsdl.org |
| **miniupnpc** 2.2.8 | fetched into `third_party/` by the setup scripts (the project does not open ports automatically) | BSD-3-Clause | https://miniupnp.tuxfamily.org |

## Libraries shipped next to the Windows program

Built from source by `tools/setup/build_win_deps.sh` with pinned versions; the list is also in
`tools/package_windows/THIRD-PARTY.txt`. The Windows download (`tools/pipeline/package_windows.py`, T-742) ships these
DLLs beside `Battle Stadium DON.exe`; its `licenses/LIBRARIES.txt` lists the DLLs shipped and the packaging script stops,
naming the DLL, when one has no licence text (the DLL-to-project map is `WINDOWS_LIBRARY_PROJECTS` in
`tools/module/stage_install.py`).

| Library | Licence |
|---|---|
| zlib | zlib |
| libpng (with the APNG patch) | libpng |
| libjpeg-turbo | BSD-3-Clause / IJG / zlib |
| lz4 (library) | BSD-2-Clause |
| zstd | BSD-3-Clause or GPL-2.0 |
| libwebp (with sharpyuv) | BSD-3-Clause |
| FreeType | FreeType licence (or GPL-2.0) |
| plutovg, plutosvg | MIT |
| rapidyaml, c4core | MIT |
| shaderc, glslang, SPIRV-Tools, SPIRV-Headers | Apache-2.0 / BSD |

`gs/resources/` is PCSX2's resources folder: GPL-3.0 plus the licences of the fonts and icons inside it (see the
PCSX2 source).

**Microsoft Visual C++ runtime DLLs** (`msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`,
`vcruntime140_1.dll`, and any other the shipped programs import): copied unmodified from Microsoft's Visual C++
2015-2022 x64 Redistributable (`vc_redist.x64.exe`, the version pinned in `tools/pipeline/vcredist-windows.sha256`) and
placed beside the program that loads them, so it starts on a Windows PC without the redistributable installed. They are
Microsoft's files, redistributed under Microsoft's redistribution terms, whose text is `licenses/Microsoft-VCRedist-license.rtf`;
they are not under this project's GPL (the GPL-3.0 system library exception). The Windows Universal C Runtime
(`ucrtbase.dll`, `api-ms-win-crt-*`) is part of Windows 10 and later and is not shipped. The packaging script refuses a
package in which a program imports a Visual C++ runtime DLL that is not shipped (`tools/pipeline/package_checks.py`).

## What the Windows download adds

Besides the program and the DLLs above, the Windows archive (`don-windows-<version>.zip`) carries the pieces that build
the game's code on the player's PC, as the Linux one does:

| Piece | Used for | Licence | Source |
|---|---|---|---|
| **LLVM 17.0.6 win64** (`clang++.exe`, `lld-link.exe` and clang's resource headers in `toolchain/`, taken from the official `LLVM-17.0.6-win64.exe` by `tools/pipeline/fetch_toolchain.py --platform windows`) | compiles the translated game code on the player's PC | Apache-2.0 with LLVM exceptions (`licenses/LLVM-LICENSE.TXT`, the same text as the Linux archive) | https://github.com/llvm/llvm-project (release llvmorg-17.0.6) |
| **SDL3** (`SDL3.dll`) | window, input, audio device | zlib (`licenses/SDL3-LICENSE.txt`) | https://github.com/libsdl-org/SDL |
| **PS2Recomp** (`bin/ps2_recomp.exe`) | the recompiler | GPL-3.0 (`licenses/PS2Recomp-LICENSE`) | https://github.com/ran-j/PS2Recomp |
| **PCSX2** (`don_gs.dll`, `don_vu.dll`, `don_spu2.dll`, `resources/`) | graphics, vector units, sound chip | GPL-3.0-or-later (`licenses/PCSX2-COPYING.GPLv3`) | https://github.com/PCSX2/pcsx2 |

## What the Linux download adds

The Linux archive (`tools/pipeline/package_linux.py`) carries, besides the program, the pieces that build the game's
code on the player's PC. The archive's `licenses/` folder holds the licence text of every piece below, and
`licenses/LIBRARIES.txt` lists the shared libraries shipped. The packaging script stops, naming the library, when a
shipped library has no licence text (the texts of the dependencies come from the build image's `/opt/deps/share`).

| Piece | Used for | Licence | Source |
|---|---|---|---|
| **LLVM 17.0.6** (`clang++`, `ld.lld` and clang's resource headers in `toolchain/`) | compiles the translated game code on the player's PC | Apache-2.0 with LLVM exceptions (`licenses/LLVM-LICENSE.TXT`: the full text of `llvm/LICENSE.TXT` at tag llvmorg-17.0.6, fetched with a pinned SHA-256 by `tools/pipeline/fetch_toolchain.py`; the binary release archive itself carries only a six-line notice, `include/llvm/Support/LICENSE.TXT`, which is not shipped) | https://github.com/llvm/llvm-project (release llvmorg-17.0.6) |
| **SDL3** (`libSDL3.so.0`) | window, input, audio device | zlib (`licenses/SDL3-LICENSE.txt`) | https://github.com/libsdl-org/SDL |
| **PS2Recomp** (`bin/ps2_recomp`) | the recompiler | GPL-3.0 (`licenses/PS2Recomp-LICENSE`) | https://github.com/ran-j/PS2Recomp |
| **PCSX2** (`libdon_gs.so`, `libdon_vu.so`, `libdon_spu2.so`, `resources/`) | graphics, vector units, sound chip | GPL-3.0-or-later (`licenses/PCSX2-COPYING.GPLv3`) | https://github.com/PCSX2/pcsx2 |

LLVM publishes no signature or digest for the binary release archive, so `tools/pipeline/fetch_toolchain.py` checks it
against the SHA-256 pinned in `tools/pipeline/toolchain.sha256`, which is recorded on the first download (trust on first
use; decided by the owner, 2026-10-27).

Every other shared library beside `don` (the soname-to-project map is `LIBRARY_PROJECTS` in
`tools/module/stage_install.py`; the versions are in the build image's `VERSIONS.txt`):

| Library | Project and version | Licence | Text in `licenses/` |
|---|---|---|---|
| `libjpeg`, `libturbojpeg` | libjpeg-turbo 3.2.0 | BSD-3-Clause / IJG / zlib | `libjpeg-turbo-LICENSE.md`, `libjpeg-turbo-README.ijg` |
| `libpng` | libpng 1.6.58 (APNG patch) | libpng | `libpng-*` |
| `libwebp`, `libsharpyuv` | libwebp 1.6.0 | BSD-3-Clause | `libwebp-*` |
| `liblz4` | lz4 1.10.0 | BSD-2-Clause (library) | `lz4-*` |
| `libzstd` | zstd 1.5.7 | BSD-3-Clause or GPL-2.0 | `zstd-*` |
| `libplutovg`, `libplutosvg` | plutovg 1.3.2, plutosvg 0.0.7 | MIT | `plutovg-*`, `plutosvg-*` |
| `libshaderc_shared` (loaded by PCSX2 with `dlopen`) | shaderc 2026.2 with glslang, SPIRV-Tools, SPIRV-Headers | Apache-2.0 / BSD | `shaderc-*` |

The system libraries the program also loads (glibc, libstdc++, X11/Wayland, Vulkan loader) are not shipped.

## What is not in this project, and what the player supplies

- **The game.** *Battle Stadium D.O.N* is © Bandai Namco / Capcom / Eighting and is not included. The player
  supplies their **own disc image** of the PS2 game (`SLPS-25675`), made from a disc they own. See
  [docs/HOW_TO_MAKE_A_DISC_IMAGE.md](docs/HOW_TO_MAKE_A_DISC_IMAGE.md).
- **Optional: the English fan translation** by Lord Izen. It is not included; the player applies their own copy to
  their own disc image, or uses the Japanese disc.
- **Their own PC:** the game's code is translated and compiled on their machine from their disc, and the result
  stays there.

## Sources

The complete corresponding source of the program is this repository plus the pinned upstream checkouts and the
patches in `patches/`. Anyone who receives a binary can get it there.
