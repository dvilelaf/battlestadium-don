#!/usr/bin/env bash
# Builds libdon_spu2.so (the PCSX2 SPU2 behind a C API) and spu2_selftest inside the build
# container.
#
#   bash tools/pipeline/build_spu2.sh
#
# Needs third_party/pcsx2 at the pinned commit (bash tools/setup/fetch_pcsx2.sh). Only the SPU2
# source files of PCSX2 are compiled, unmodified, against the replacement headers in
# src/spu2bridge/shim. Incremental: the build tree stays in build/pcsx2-spu2/. The result goes to
# build/spu2/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SRC=third_party/pcsx2
BUILD=build/pcsx2-spu2
OUT=build/spu2
start=$(date +%s)

if [[ ! -f "$SRC/pcsx2/SPU2/spu2sys.cpp" ]]; then
  echo "error: $SRC is missing; run: bash tools/setup/fetch_pcsx2.sh" >&2
  exit 1
fi

bash tools/container/run.sh cmake -S src/spu2bridge -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  "-DDON_PCSX2_SOURCE_DIR=$ROOT/$SRC"

bash tools/container/run.sh cmake --build "$BUILD" --target don_spu2 spu2_selftest

mkdir -p "$OUT"
. tools/pipeline/copy_safe.sh   # never write in place into a library a running program has mapped (016)
copy_safe "$BUILD/libdon_spu2.so" "$OUT/libdon_spu2.so"
copy_safe "$BUILD/spu2_selftest" "$OUT/spu2_selftest"

echo "built $OUT/libdon_spu2.so in $(( $(date +%s) - start )) s"
