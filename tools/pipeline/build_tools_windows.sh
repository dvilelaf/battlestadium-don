#!/usr/bin/env bash
# Cross-builds ps2_recomp.exe (the recompiler the Windows player's PC runs) with clang-cl /MT and static
# dependencies, inside don-build-win:1. Windows twin of build_tools.sh; needs patches/ps2recomp/0092-recomp-windows-static.patch
# applied to the PS2Recomp checkout (third_party/PS2Recomp, or the copy named by DON_PS2RECOMP_SRC).
#
#   tools/pipeline/build_tools_windows.sh        (from the host: re-runs itself inside the container)
#
# Output: build/windows-tools/ps2xRecomp/ps2_recomp.exe. Run it under Wine with tools/container/run_wine.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ ! -x /usr/local/bin/clang-cl ]]; then
	exec "$ROOT/tools/container/run_windows.sh" bash "$ROOT/tools/pipeline/build_tools_windows.sh" "$@"
fi

SRC="${DON_PS2RECOMP_SRC:-$ROOT/third_party/PS2Recomp}"
BUILD="${DON_TOOLS_WIN_BUILD:-$ROOT/build/windows-tools}"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-windows-clangcl.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
	-DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_ANALYZER=OFF
cmake --build "$BUILD" -j "${DON_JOBS:-4}" --target ps2_recomp

echo "PS2Recomp recompiler (Windows): $BUILD/ps2xRecomp/ps2_recomp.exe"
