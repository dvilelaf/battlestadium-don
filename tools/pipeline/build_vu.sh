#!/usr/bin/env bash
# Builds libdon_vu.so (the PCSX2 microVU behind a C API) and vu_selftest inside the build
# container.
#
#   bash tools/pipeline/build_vu.sh
#
# Needs third_party/pcsx2 at the pinned commit (bash tools/setup/fetch_pcsx2.sh). Only the VU
# source files of PCSX2 are compiled, unmodified, against the replacement headers in
# src/vubridge/shim. Incremental: the build tree stays in build/pcsx2-vu/. The result goes to
# build/vu/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SRC=third_party/pcsx2
BUILD=build/pcsx2-vu
OUT=build/vu
start=$(date +%s)

if [[ ! -f "$SRC/pcsx2/x86/microVU.cpp" ]]; then
  echo "error: $SRC is missing; run: bash tools/setup/fetch_pcsx2.sh" >&2
  exit 1
fi

bash tools/container/run.sh cmake -S src/vubridge -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  "-DDON_PCSX2_SOURCE_DIR=$ROOT/$SRC"

bash tools/container/run.sh cmake --build "$BUILD" --target don_vu vu_selftest

mkdir -p "$OUT"
. tools/pipeline/copy_safe.sh   # never write in place into a library a running program has mapped (016)
copy_safe "$BUILD/libdon_vu.so" "$OUT/libdon_vu.so"
copy_safe "$BUILD/vu_selftest" "$OUT/vu_selftest"

echo "built $OUT/libdon_vu.so in $(( $(date +%s) - start )) s"
