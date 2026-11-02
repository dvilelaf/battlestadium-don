#!/usr/bin/env bash
# Builds the host program's synthetic test (src/host + a synthetic guest) inside the build container.
#
#   bash tools/pipeline/build_host.sh
#
# Uses the PS2Recomp tree in DON_PS2RECOMP_DIR (default third_party/PS2Recomp, with patches/ps2recomp
# applied). Incremental: the CMake build tree stays in build/host-build/; the executable and the SDL3
# library it needs go to build/host/. The GS library is not linked (it is loaded at run time): it is looked
# up in <exe>/gs, <exe>/../gs (build/gs) or DON_GS_DIR / --gs-dir.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

RECOMP="${DON_PS2RECOMP_DIR:-third_party/PS2Recomp}"
BUILD=build/host-build
OUT=build/host
start=$(date +%s)

if [[ ! -f "$RECOMP/ps2xRuntime/CMakeLists.txt" ]]; then
  echo "error: $RECOMP has no ps2xRuntime; run: bash tools/setup/fetch_ps2recomp.sh (or set DON_PS2RECOMP_DIR)" >&2
  exit 1
fi
if [[ ! -f "$RECOMP/ps2xRuntime/include/runtime/ps2_host_pad.h" ]]; then
  echo "error: patches/ps2recomp are not applied to $RECOMP (no ps2_host_pad.h)" >&2
  exit 1
fi

bash tools/container/run.sh cmake -S tests/host -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DPS2X_WITH_RAYLIB=OFF \
  "-DPS2RECOMP_DIR=$(cd "$RECOMP" && pwd)"

bash tools/container/run.sh cmake --build "$BUILD" -j6 --target host_synthetic_test --target pacing_stats_test --target audio_prebuffer_test --target audio_rate_test

mkdir -p "$OUT"
. tools/pipeline/copy_safe.sh
copy_safe "$BUILD/host_synthetic_test" "$OUT/host_synthetic_test"
copy_safe "$BUILD/pacing_stats_test" "$OUT/pacing_stats_test"
copy_safe "$BUILD/audio_prebuffer_test" "$OUT/audio_prebuffer_test"
copy_safe "$BUILD/audio_rate_test" "$OUT/audio_rate_test"
# SDL3 is built from source in the image (/opt/deps); keep a copy next to the executable. copy_safe, not
# `cp -f`: the executable may be running (journal 018).
bash tools/container/run.sh bash -c '
  set -euo pipefail
  out=$1
  . tools/pipeline/copy_safe.sh
  copy_safe /opt/deps/lib/libSDL3.so.0.4.12 "$out/libSDL3.so.0"
  patchelf_safe "$out/host_synthetic_test" "\$ORIGIN:\$ORIGIN/../gs"
' _ "$OUT"

echo "built $OUT/host_synthetic_test in $(( $(date +%s) - start )) s"
