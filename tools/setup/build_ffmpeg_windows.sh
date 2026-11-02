#!/usr/bin/env bash
# Windows twin of build_ffmpeg.sh: the same minimal static FFmpeg (pinned 7.1.1, MPEG-1/2 video
# decoder, parser, swscale; LGPL v2.1+, no --enable-gpl/--enable-nonfree, no external libraries),
# cross-built for the target the game uses (x86-64, clang-cl, MSVC ABI, static .lib) into
# third_party/ffmpeg/windows. T-305.
#
# Runs inside the image don-build-win:1 (started here through tools/container/run_windows.sh, so it
# takes the project's lock and memory limit), with the Microsoft CRT and SDK that xwin fetched
# (third_party/xwin/sysroot, or DON_XWIN_ROOT). No assembly (--disable-x86asm): the image has no
# nasm, and the decoder's C code is fast enough for 640x448 video.
# The CRT is the DLL one (/MD), the same as the rest of the Windows build; the objects carry no
# /DEFAULTLIB for anything else.
#
# Usage, from any directory:  bash tools/setup/build_ffmpeg_windows.sh      (DON_JOBS caps jobs, default 4, at most 6)
set -euo pipefail

VERSION=7.1.1
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1   # same tarball as build_ffmpeg.sh
URL=https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE=$ROOT/third_party/ffmpeg
PREFIX=$BASE/windows

if [[ ! -e /.dockerenv ]]; then
  # download and unpack on the host (network), compile in the container
  if [[ -f "$PREFIX/lib/avcodec.lib" && -f "$PREFIX/VERSION" && "$(cat "$PREFIX/VERSION")" == "$VERSION" ]]; then
    echo "ffmpeg $VERSION already built in $PREFIX"; exit 0
  fi
  mkdir -p "$BASE"
  if [[ ! -f "$BASE/ffmpeg-$VERSION.tar.xz" ]]; then
    curl -fsSL --retry 5 -o "$BASE/ffmpeg-$VERSION.tar.xz" "$URL"
  fi
  echo "$SHA256  $BASE/ffmpeg-$VERSION.tar.xz" | sha256sum --check --strict -
  rm -rf "$BASE/src-win" "$BASE/obj-win" "$PREFIX"
  mkdir -p "$BASE/src-win" "$BASE/obj-win"
  tar -xf "$BASE/ffmpeg-$VERSION.tar.xz" -C "$BASE/src-win" --strip-components=1
  export DON_JOBS=${DON_JOBS:-4}
  exec bash "${DON_RUN_WINDOWS:-$ROOT/tools/container/run_windows.sh}" bash "$ROOT/tools/setup/build_ffmpeg_windows.sh"
fi

J=${DON_JOBS:-4}; (( J > 6 )) && J=6
XWIN=${DON_XWIN_ROOT:-$ROOT/third_party/xwin/sysroot}
INC="/imsvc $XWIN/crt/include /imsvc $XWIN/sdk/include/ucrt /imsvc $XWIN/sdk/include/um /imsvc $XWIN/sdk/include/shared"
LIBP="/libpath:$XWIN/crt/lib/x86_64 /libpath:$XWIN/sdk/lib/um/x86_64 /libpath:$XWIN/sdk/lib/ucrt/x86_64"
cd "$BASE/obj-win"
# clang-cl has -ffp-contract on by default; FFmpeg's decoders are integer code, but keep it off like the rest.
"$BASE/src-win/configure" --prefix="$PREFIX" --toolchain=msvc --arch=x86_64 --target-os=win32 --enable-cross-compile \
  --cc=clang-cl --cxx=clang-cl --ld=lld-link --ar=llvm-lib --nm=llvm-nm --ranlib=true --windres=llvm-rc --dep-cc=clang-cl --host-cc=clang --host-ld=clang \
  --extra-cflags="--target=x86_64-pc-windows-msvc -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /clang:-ffp-contract=off $INC" \
  --extra-ldflags="$LIBP" \
  --disable-x86asm --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --enable-static --disable-shared \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo \
  --enable-demuxer=mpegps
# configure only recognises Microsoft's lib.exe by its banner; llvm-lib takes the same arguments
sed -i -e 's/^ARFLAGS=.*/ARFLAGS=-nologo/' -e 's/^AR_O=.*/AR_O=-out:$@/' ffbuild/config.mak
make -j"$J"
make install
echo "$VERSION" > "$PREFIX/VERSION"
du -sh "$PREFIX"
