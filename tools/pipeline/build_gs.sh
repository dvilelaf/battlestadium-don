#!/usr/bin/env bash
# Builds libdon_gs.so (the PCSX2 GS behind a C API) inside the build container.
#
#   bash tools/pipeline/build_gs.sh
#
# Needs third_party/pcsx2 at the pinned commit with patches/pcsx2/series applied
# (bash tools/setup/fetch_pcsx2.sh). Incremental: PCSX2's build tree stays in
# build/pcsx2-gs/ and only changed files are rebuilt. The result goes to build/gs/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SRC=third_party/pcsx2
BUILD=build/pcsx2-gs
OUT=build/gs
start=$(date +%s)

if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
  echo "error: $SRC is missing; run: bash tools/setup/fetch_pcsx2.sh" >&2
  exit 1
fi
if ! grep -q DON_GS_BRIDGE_SOURCE_DIR "$SRC/CMakeLists.txt"; then
  echo "error: patches/pcsx2 are not applied to $SRC; run: bash tools/setup/fetch_pcsx2.sh" >&2
  exit 1
fi

bash tools/container/run.sh cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld \
  -DENABLE_QT_UI=OFF \
  -DENABLE_TESTS=OFF \
  -DUSE_VULKAN=ON \
  -DUSE_OPENGL=OFF \
  -DX11_API=ON \
  -DWAYLAND_API=ON \
  -DDISABLE_ADVANCE_SIMD=ON \
  -DENABLE_SETCAP=OFF \
  "-DDON_GS_BRIDGE_SOURCE_DIR=$ROOT/src/gsbridge"

# pcsx2-gsrunner (PCSX2's own headless dump runner, unmodified) is a target of the same project that
# is not built by default; replay_all.sh compares gsreplay against it.
bash tools/container/run.sh cmake --build "$BUILD" --target don_gs gsreplay pcsx2-gsrunner

mkdir -p "$OUT"
. tools/pipeline/copy_safe.sh
# copy_safe, not `cp -f`: libdon_gs.so is mapped by every run of the port (journal 018).
copy_safe "$BUILD/don-gs-bridge/libdon_gs.so" "$OUT/libdon_gs.so"
copy_safe "$BUILD/don-gs-bridge/gsreplay" "$OUT/gsreplay"
# The runner looks for PCSX2's resources next to itself (EmuFolders::SetAppRoot): same folder.
copy_safe "$BUILD/bin/pcsx2-gsrunner" "$OUT/pcsx2-gsrunner"
rm -rf "$OUT/resources"
cp -r "$BUILD/don-gs-bridge/resources" "$OUT/resources"

# The libraries built from source in the image (/opt/deps) do not exist on the host: copy
# the ones libdon_gs.so needs (directly or through each other) next to it, and make each of
# them look for its own dependencies next to itself ($ORIGIN). The system libraries are left to
# the host. libshaderc is not linked: PCSX2 loads it by name with dlopen when it compiles its
# shaders, so it is copied explicitly.
bash tools/container/run.sh bash -c '
  set -euo pipefail
  out=$1
  . tools/pipeline/copy_safe.sh
  rm -f "$out"/*.so.*
  { ldd "$out/libdon_gs.so"; ldd "$out/pcsx2-gsrunner"; } | sort -u | awk "/=> \/opt\/deps\// { print \$3 }" | while read -r lib; do
    copy_safe "$lib" "$out/$(basename "$lib")"
  done
  copy_safe /opt/deps/lib/libshaderc_shared.so.1 "$out/libshaderc_shared.so.1"
  for lib in "$out"/*.so.*; do
    patchelf_safe "$lib" "\$ORIGIN"
  done
  patchelf_safe "$out/pcsx2-gsrunner" "\$ORIGIN"
' _ "$OUT"

echo "built $OUT/libdon_gs.so in $(( $(date +%s) - start )) s"
