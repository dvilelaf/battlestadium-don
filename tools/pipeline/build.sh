#!/usr/bin/env bash
# Configures (only when the build folder does not exist yet) and builds the `don` executable
# into build/<preset>, inside the build container (clang + lld, SDL3 from /opt/deps; see
# tools/container/). Ninja re-runs CMake by itself when a CMake file or generated/sources*.txt
# changes. Afterwards libSDL3.so.0 is copied next to the executable and its run path is set to
# $ORIGIN:$ORIGIN/../gs, so that the GS library in build/gs is found from build/<preset>/.
# Needs generated/ (run tools/pipeline/generate.sh first) and third_party/PS2Recomp (patched).
# Never run cmake for these folders on the host: a configure done there must not be mixed with
# a build done in the container.
# Usage: build.sh [preset]   (linux-release by default; linux-dev for the edit-and-run loop)
# Environment: DON_JOBS = parallel jobs (default 12), DON_CONTAINER_MEMORY = the container's memory limit
# (see tools/container/run.sh; on this machine 16g by default, and one container run at a time via its lock).
# Memory (2026-10-13, T-207): a full linux-dev build from nothing at DON_JOBS=8 peaked at 6,938,214,400
# bytes (6.46 GiB) and took 8m19s; the peak is printed at the end of every build. The default of 12 jobs
# is the largest of 6/8/10/12 whose estimated peak (peak@8 x jobs/8 = 9.7 GiB) stays under 80 % of the
# 16 GiB limit. Never two full builds at once on a 32 GB machine.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PRESET="${1:-linux-release}"
JOBS="${DON_JOBS:-12}"
DIR="build/$PRESET"

if [[ ! -d generated ]] || [[ -z "$(find -L generated -maxdepth 1 -name '*.cpp' -print -quit)" ]]; then
  echo "[build] generated/ is missing or empty: run tools/pipeline/generate.sh first" >&2
  exit 1
fi

start=$SECONDS
if [[ ! -f "$DIR/build.ninja" ]]; then
  bash tools/container/run.sh cmake --preset "$PRESET"
fi
# The build and, as the last command of the same container run, the peak memory the container used
# (the container's own cgroup; /sys/fs/cgroup/memory.peak only exists while it runs). A build that
# needs more than the limit fails there with a killed compiler instead of freezing the host
# (tools/container/run.sh, 2026-10-13).
bash tools/container/run.sh bash -c '
  set -uo pipefail
  cmake --build --preset "$1" -j "$2"
  rc=$?
  peak=$(cat /sys/fs/cgroup/memory.peak 2>/dev/null || true)
  if [[ -n "$peak" ]]; then
    printf "[build] peak memory: %s bytes\n" "$peak"
  else
    echo "[build] peak memory: not available"
  fi
  exit "$rc"
' _ "$PRESET" "$JOBS"

# What the executable needs to start on the host. Never write in place into a file a running program may
# have mapped (see tools/pipeline/copy_safe.sh and journal 018): a `cp -f` onto libSDL3.so.0 truncates a
# mapped library and the next SDL call jumps to a link-time address (that was the crash at 0x66F80).
bash tools/container/run.sh bash -c '
  set -euo pipefail
  dir=$1
  . tools/pipeline/copy_safe.sh
  copy_safe /opt/deps/lib/libSDL3.so.0.4.12 "$dir/libSDL3.so.0"
  patchelf_safe "$dir/don" "\$ORIGIN:\$ORIGIN/../gs"
' _ "$DIR"

elapsed=$((SECONDS - start))
printf '[build] %s done in %dh %02dm %02ds\n' "$PRESET" $((elapsed / 3600)) $((elapsed % 3600 / 60)) $((elapsed % 60))
if [[ ! -f build/gs/libdon_gs.so ]]; then
  echo "[build] warning: build/gs/libdon_gs.so is missing: $DIR/don builds but cannot start (run tools/pipeline/build_gs.sh)" >&2
fi
