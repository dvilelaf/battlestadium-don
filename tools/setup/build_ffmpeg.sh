#!/usr/bin/env bash
# Builds a minimal static FFmpeg (only what the runtime's sceMpeg stub calls: the MPEG-1/2 video
# decoder, its parser, swscale) from a pinned release, into third_party/ffmpeg/linux. T-305.
#
# What the stub (third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp) uses:
# avcodec_find_decoder(MPEG2VIDEO), av_parser_*, avcodec_send_packet/receive_frame, sws_*.
# The runtime's CMake also asks for libavformat and libswresample (pkg-config, REQUIRED), so
# they are built too, with no demuxers, muxers or protocols.
#
# Licence: with this configuration (no --enable-gpl, no --enable-nonfree, no external libraries)
# the libraries are LGPL v2.1 or later. Static linking into a GPL-3.0 program is fine.
#
# The compilation runs inside the build container (same compiler and glibc as the game build) through
# tools/container/run.sh, so it takes the project's lock and memory limit.
#
# Usage, from any directory:  bash tools/setup/build_ffmpeg.sh        (DON_JOBS caps the parallelism, default 4)
set -euo pipefail

VERSION=7.1.1
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1   # first download, 2026-10-14
URL=https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE=$ROOT/third_party/ffmpeg
PREFIX=$BASE/linux

if [[ -f "$PREFIX/lib/pkgconfig/libavcodec.pc" && -f "$PREFIX/VERSION" && "$(cat "$PREFIX/VERSION")" == "$VERSION" ]]; then
  echo "ffmpeg $VERSION already built in $PREFIX"
  exit 0
fi

mkdir -p "$BASE"
if [[ ! -f "$BASE/ffmpeg-$VERSION.tar.xz" ]]; then
  curl -fsSL --retry 5 -o "$BASE/ffmpeg-$VERSION.tar.xz" "$URL"
fi
echo "$SHA256  $BASE/ffmpeg-$VERSION.tar.xz" | sha256sum --check --strict -
rm -rf "$BASE/src" "$BASE/obj" "$PREFIX"
mkdir -p "$BASE/src" "$BASE/obj"
tar -xf "$BASE/ffmpeg-$VERSION.tar.xz" -C "$BASE/src" --strip-components=1

JOBS=${DON_JOBS:-4}
export DON_JOBS=$JOBS
bash "$ROOT/tools/container/run.sh" bash -c "
set -euo pipefail
cd '$BASE/obj'
'$BASE/src/configure' --prefix='$PREFIX' --cc=clang --cxx=clang++ \
  --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
  --disable-debug --enable-static --disable-shared --enable-pic \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=mpeg2video --enable-decoder=mpeg1video --enable-parser=mpegvideo \
  --enable-demuxer=mpegps
make -j$JOBS
make install
echo '$VERSION' > '$PREFIX/VERSION'
"
du -sh "$PREFIX"
