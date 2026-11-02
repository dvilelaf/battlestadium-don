#!/usr/bin/env bash
# Builds the Windows dependencies of the port (SDL3 for the host program; zlib, libpng, libjpeg-turbo,
# lz4, zstd, libwebp, FreeType, plutovg/plutosvg, rapidyaml, DirectX-Headers and shaderc for the PCSX2 GS
# library) from pinned sources into a prefix, cross-compiling with clang-cl inside the image
# `don-build-win:1` (decision D13). Run it from anywhere on the host: it starts the container itself.
#
#   bash tools/setup/build_win_deps.sh [--prefix DIR] [name ...]
#
#   --prefix DIR   (relative to the repository root) where the libraries are installed (default third_party/win-deps/clangcl, which is what the
#                  windows-release preset uses as CMAKE_PREFIX_PATH)
#   name ...       only these (zlib libpng libjpeg-turbo lz4 zstd libwebp freetype sdl3 plutovg plutosvg
#                  rapidyaml directx-headers shaderc); default all. Order matters: the list above is the
#                  order of the dependencies.
#
# Environment: DON_WIN_DEPS_SRC  downloaded archives (default third_party/win-deps-src), each checked against
#                                its SHA-256 here, in this file;
#              DON_PCSX2_DIR     PCSX2 tree that holds shaderc-changes.patch (default third_party/pcsx2);
#              DON_JOBS          compile jobs (default 4, at most 6).
#
# The versions and hashes are the ones PCSX2 v2.8.2 pins for its own Windows build
# (third_party/pcsx2/.github/workflows/scripts/windows/build-dependencies.bat) and, for the libraries that
# the Linux image has too, the same ones as tools/container/build-deps.sh, with these deviations from
# PCSX2's script: no Qt, no FFmpeg, no harfbuzz (not used by the port), libjpeg-turbo without SIMD
# (cross-compiling the assembly needs nasm), SDL3 built with video (the host program opens its window with
# it). SDL3 is the same 3.4.12 as in the Linux image. Pins are also listed in docs/knowledge/09-environment-and-tools.md.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

# ---- on the host: restart inside the Windows image --------------------------------------------------
if [[ ! -e /.dockerenv ]]; then
	# The container script of the main checkout takes the shared lock; DON_RUN_WINDOWS overrides its path
	# (a git worktree has its own copy of tools/container, which would take another lock).
	RUNNER=${DON_RUN_WINDOWS:-$ROOT/tools/container/run_windows.sh}
	quoted=$(printf '%q ' "$@")
	exec bash "$RUNNER" sh -c "cd $(printf '%q' "$ROOT") && bash tools/setup/build_win_deps.sh $quoted"
fi

# ---- inside the container ---------------------------------------------------------------------------
cd "$ROOT"
PREFIX=$ROOT/third_party/win-deps/clangcl
if [[ ${1:-} == --prefix ]]; then
	PREFIX=$(realpath -m "$2"); shift 2
fi
NAMES=("$@")
SRC=$(realpath -m "${DON_WIN_DEPS_SRC:-$ROOT/third_party/win-deps-src}")
PCSX2=$(realpath -m "${DON_PCSX2_DIR:-$ROOT/third_party/pcsx2}")
PATCHES=$PCSX2/.github/workflows/scripts/common
J=${DON_JOBS:-4}; (( J > 6 )) && J=6
TC=$ROOT/cmake/toolchain-windows-clangcl.cmake
BLD=$(mktemp -d /tmp/win-deps-build.XXXXXX)
trap 'rm -rf "$BLD"' EXIT
mkdir -p "$SRC" "$PREFIX"

# ---- pins -------------------------------------------------------------------------------------------
ZLIB=1.3.2 LIBPNG=1.6.58 JPEG=3.2.0 LZ4=1.10.0 FT=2.14.3 ZSTD=1.5.7 WEBP=1.6.0 SDL=SDL3-3.4.12
PLUTOVG=1.3.2 PLUTOSVG=0.0.7 RYML=0.12.1 DXH=1.619.1 SHADERC=2026.2
GLSLANG=275822a6261ee689aadb1da5f09a0ec2f058685c SPVH=58006c901d1d5c37dece6b6610e9af87fa951375 SPVT=6337eb62cadd7d124ac6789bf39c0f71148f0a73

fetch() { # sha256 file url
	[[ -f "$SRC/$2" ]] || curl -fsSL --retry 5 --retry-delay 5 -o "$SRC/$2" "$3"
	echo "$1  $SRC/$2" | sha256sum --check --strict - >/dev/null || { echo "[deps] BAD HASH $2" >&2; exit 1; }
}
unz() { rm -rf "$BLD/src-$2"; mkdir -p "$BLD/src-$2"; unzip -q "$SRC/$1" -d "$BLD/src-$2"; }
untar() { rm -rf "$BLD/src-$2"; mkdir -p "$BLD/src-$2"; tar xf "$SRC/$1" -C "$BLD/src-$2"; }
cm() { # srcdir name cmake-args...
	local s=$1 n=$2; shift 2
	cmake -G Ninja -S "$s" -B "$BLD/b-$n" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
		-DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@" >"$BLD/log-$n.txt" 2>&1 || { tail -30 "$BLD/log-$n.txt"; exit 1; }
	cmake --build "$BLD/b-$n" --parallel "$J" >>"$BLD/log-$n.txt" 2>&1 || { tail -40 "$BLD/log-$n.txt"; exit 1; }
	cmake --install "$BLD/b-$n" >>"$BLD/log-$n.txt" 2>&1
	echo "[deps] $n ok"
}
w() { [[ ${#NAMES[@]} -eq 0 ]] && return 0; local x; for x in "${NAMES[@]}"; do [[ "$x" == "$1" ]] && return 0; done; return 1; }

if w zlib; then
	fetch e8bf55f3017aa181690990cb58a994e77885da140609fc8f94abe9b65d2cae28 zlib132.zip "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib132.zip"
	unz zlib132.zip zlib
	cm "$BLD/src-zlib/zlib-$ZLIB" zlib -DBUILD_SHARED_LIBS=ON -DZLIB_BUILD_EXAMPLES=OFF -DZLIB_BUILD_TESTING=OFF
fi
if w libpng; then
	fetch b32f170855dbbe3e6d9e645af40b538137041773672c3ba3e02db5816c82d376 lpng1658.zip https://download.sourceforge.net/libpng/lpng1658.zip
	fetch eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0 lpng-apng.patch.gz "https://download.sourceforge.net/libpng-apng/libpng-$LIBPNG-apng.patch.gz"
	unz lpng1658.zip png; gzip -dkc "$SRC/lpng-apng.patch.gz" > "$BLD/apng.patch"
	(cd "$BLD/src-png/lpng1658" && patch -p1 < "$BLD/apng.patch" >/dev/null)
	cm "$BLD/src-png/lpng1658" libpng -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -DSKIP_INSTALL_FILES=ON
fi
if w libjpeg-turbo; then
	fetch 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e jpeg.tar.gz "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG/libjpeg-turbo-$JPEG.tar.gz"
	untar jpeg.tar.gz jpeg
	cm "$BLD/src-jpeg/libjpeg-turbo-$JPEG" libjpeg-turbo -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -DWITH_SIMD=OFF
fi
if w lz4; then
	fetch 3224b4c80f351f194984526ef396f6079bd6332dd9825c72ac0d7a37b3cdc565 lz4.zip "https://github.com/lz4/lz4/archive/refs/tags/v$LZ4.zip"
	unz lz4.zip lz4
	cm "$BLD/src-lz4/lz4-$LZ4/build/cmake" lz4 -DBUILD_SHARED_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
fi
if w zstd; then
	fetch 7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 zstd.zip "https://github.com/facebook/zstd/archive/refs/tags/v$ZSTD.zip"
	unz zstd.zip zstd
	cm "$BLD/src-zstd/zstd-$ZSTD/build/cmake" zstd -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF
fi
if w libwebp; then
	fetch e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 webp.tar.gz "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$WEBP.tar.gz"
	untar webp.tar.gz webp
	cm "$BLD/src-webp/libwebp-$WEBP" libwebp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
fi
if w freetype; then
	fetch e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1 freetype.tar.gz "https://sourceforge.net/projects/freetype/files/freetype2/$FT/freetype-$FT.tar.gz/download"
	untar freetype.tar.gz ft
	cm "$BLD/src-ft/freetype-$FT" freetype -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE
fi
if w sdl3; then
	fetch 3d4de8967a49c0451e775a0c1e9022092c19fdef41ba38a83fcf031c5a6496e2 sdl.zip "https://libsdl.org/release/$SDL.zip"
	unz sdl.zip sdl
	cm "$BLD/src-sdl/$SDL" sdl3 -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF
fi
if w plutovg; then
	fetch 4fe4e48f28aa80171b2166d45c0976ab0f21eecedb52cd4c3ef73b5afb48fac9 plutovg.zip "https://github.com/sammycage/plutovg/archive/v$PLUTOVG.zip"
	unz plutovg.zip plutovg
	cm "$BLD/src-plutovg/plutovg-$PLUTOVG" plutovg -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF
fi
if w plutosvg; then
	fetch 82dee2c57ad712bdd6d6d81d3e76249d89caa4b5a4214353660fd5adff12201a plutosvg.zip "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG.zip"
	unz plutosvg.zip plutosvg
	cm "$BLD/src-plutosvg/plutosvg-$PLUTOSVG" plutosvg -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
fi
if w rapidyaml; then
	fetch 96276f55b9fa7837ac8f3f72fd52965879cbb5d5d2e6af548c69a177fb078304 ryml.zip "https://github.com/biojppm/rapidyaml/releases/download/v$RYML/rapidyaml-$RYML-src.zip"
	unz ryml.zip ryml
	cm "$BLD/src-ryml/rapidyaml-$RYML-src" rapidyaml -DBUILD_SHARED_LIBS=ON
fi
if w directx-headers; then
	fetch 9eb8b102a90a42e4ea72a825f7d249d55ec90d164f030966c9b7784b93374927 dxh.zip "https://github.com/microsoft/DirectX-Headers/archive/v$DXH.zip"
	unz dxh.zip dxh
	cm "$BLD/src-dxh/DirectX-Headers-$DXH" directx-headers -DDXHEADERS_BUILD_TEST=OFF -DDXHEADERS_BUILD_GOOGLE_TEST=OFF
fi
if w shaderc; then
	fetch f9401cc5cb36c276cd1e072b6595dbd728148e8dba389e50f7339e2d388dbc08 shaderc.zip "https://github.com/google/shaderc/archive/refs/tags/v$SHADERC.zip"
	fetch 2b63189efad0348d88d410a5e12ec550a612e0b6ceef64624b8f45491269fb9c glslang.zip "https://github.com/KhronosGroup/glslang/archive/$GLSLANG.zip"
	fetch d2f071e94c081f5a4606559770ebf1f7d1eac92a1def0c3e10609844aa8b69b2 spvh.zip "https://github.com/KhronosGroup/SPIRV-Headers/archive/$SPVH.zip"
	fetch 4011be89aa73e3461c9deef73936a62c79a3097590c5135d058041cc9fb99c6f spvt.zip "https://github.com/KhronosGroup/SPIRV-Tools/archive/$SPVT.zip"
	unz shaderc.zip shaderc; S="$BLD/src-shaderc/shaderc-$SHADERC"
	(cd "$S/third_party" && unzip -q "$SRC/glslang.zip" && mv "glslang-$GLSLANG" glslang \
		&& unzip -q "$SRC/spvh.zip" && mv "SPIRV-Headers-$SPVH" spirv-headers \
		&& unzip -q "$SRC/spvt.zip" && mv "SPIRV-Tools-$SPVT" spirv-tools)
	(cd "$S" && patch -p1 < "$PATCHES/shaderc-changes.patch" >/dev/null)
	cm "$S" shaderc -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON
fi
echo "[deps] done: $PREFIX"
