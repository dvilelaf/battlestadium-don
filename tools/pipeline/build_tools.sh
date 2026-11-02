#!/usr/bin/env bash
# Builds the PS2Recomp host tools (ps2xAnalyzer and ps2xRecomp) from the
# patched checkout in third_party/PS2Recomp. The runtime and studio are off:
# only the command-line tools are needed by the pipeline.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SRC="third_party/PS2Recomp"
BUILD="$SRC/out/build-tools"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build "$BUILD" -j "$(nproc)"

echo "PS2Recomp analyzer: $ROOT/$BUILD/ps2xAnalyzer/ps2_analyzer"
echo "PS2Recomp recompiler: $ROOT/$BUILD/ps2xRecomp/ps2_recomp"
