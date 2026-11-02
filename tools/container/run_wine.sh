#!/usr/bin/env bash
# Runs a command inside the Wine image `don-wine:1` (tools/container/Dockerfile.wine).
#
#   tools/container/run_wine.sh <command...>
#
# The repository is mounted at the same absolute path as on the host and is the working
# directory; the command runs as the invoking uid:gid so created files are not owned by root.
# Environment variables whose names start with DON_ are passed through.
#
# Same two protections as run.sh (see its comments): a memory limit from DON_CONTAINER_MEMORY
# (default 55 % of MemTotal in whole GiB), and one container run at a time. The lock here is
# build/.wine.lock, distinct from run.sh's build/.container.lock, so a Wine run does not wait
# behind a build and two Wine runs do not run at once (the game under lavapipe is CPU-heavy).
#
# Checked (2026-10-16): `tools/container/run_wine.sh bash work/wine-check/wine_run.sh vulkan
# default_title_idle` runs the Windows package to exit 0 (7000 frames) on a private Xvfb and
# writes screenshots and a WAV that are byte-identical to Linux's (journal 041).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IMAGE=${DON_WINE_IMAGE:-don-wine:1}

if [ "$#" -eq 0 ]; then
	echo "usage: $0 <command...>" >&2
	exit 2
fi

# 55 % of MemTotal in whole GiB (MemTotal is in kB in /proc/meminfo).
default_memory() {
	local total_kb gib
	total_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
	[[ -n "${total_kb:-}" ]] || return 1
	gib=$(( total_kb * 55 / 100 / 1048576 ))
	(( gib > 0 )) || return 1
	printf '%sg' "$gib"
}

args=(--rm --user "$(id -u):$(id -g)" -v "$ROOT:$ROOT" -w "$ROOT" -e HOME=/tmp/home)

# A top-level symlink of the repository root that points outside it (generated/, third_party/... in a clone)
# would dangle in the container: mount its target read-only at the same absolute path.
for link in "$ROOT"/* "$ROOT"/.[!.]*; do
	[[ -L "$link" ]] || continue
	target=$(readlink -f "$link" 2>/dev/null) || continue
	[[ -e "$target" ]] || continue
	case "$target" in "$ROOT"/*) continue ;; esac
	args+=(-v "$target:$target:ro")
done

MEMORY=${DON_CONTAINER_MEMORY:-}
if [[ -z "$MEMORY" ]]; then
	MEMORY=$(default_memory || echo none)
fi
if [[ "$MEMORY" != "none" ]]; then
	args+=(--memory "$MEMORY" --memory-swap "$MEMORY")
fi

# One Wine run at a time (its own lock, not the build lock).
mkdir -p "$ROOT/build"
LOCK="$ROOT/build/.wine.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
	echo "waiting for another Wine run to finish" >&2
	flock 9
fi
exec 9>&-

# Interactive flags only when attached to a terminal; -i alone keeps pipes working.
if [ -t 0 ] && [ -t 1 ]; then
	args+=(-it)
elif [ -t 0 ]; then
	args+=(-i)
elif [ -p /dev/stdin ] || [ -f /dev/stdin ]; then
	args+=(-i)
fi

while IFS='=' read -r name _; do
	args+=(-e "$name")
done < <(env | grep '^DON_' || true)

exec docker run "${args[@]}" "$IMAGE" flock "$LOCK" "$@"
