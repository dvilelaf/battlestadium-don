#!/usr/bin/env bash
# Runs a command inside the build image `don-build-win:1`.
#
#   tools/container/run_windows.sh <command...>
#
# The repository is mounted at the same absolute path as on the host and is the working
# directory; the command runs as the invoking uid:gid so created files are not owned by root.
# Environment variables whose names start with DON_ are passed through.
#
# Memory and concurrency (same two protections as run.sh, see its comments for the history: the
# owner's machine froze on 2026-10-13 under parallel builds):
#   * --memory and --memory-swap with the same value, DON_CONTAINER_MEMORY (default 55 % of MemTotal
#     in whole GiB, "none" turns it off);
#   * one container run of this project at a time: an exclusive flock on build/.container.lock, the
#     SAME file run.sh uses, so a Windows build and a Linux build never run together. The lock is
#     taken inside the container (`flock <lock> <command...>`), so it lives as long as the run; a
#     short host-side probe only prints "waiting" and is released before the container starts.
#     The image don-build-win:1 is based on don-build:1 and has flock.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IMAGE=${DON_BUILD_WIN_IMAGE:-don-build-win:1}

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

# One run at a time. The lock is held *inside* the container (`flock` execs the command with the lock
# on the file, which the repository mount makes the same file), so it lives exactly as long as the run
# does: a docker client that is killed (as happened on 2026-10-13) does not release it while the
# container is still building. The short probe here is only what makes "waiting" show up in the common
# case and serialises the start of two runs; it is released before the container starts.
mkdir -p "$ROOT/build"
LOCK="$ROOT/build/.container.lock"
exec 9>"$LOCK"
if ! flock -n 9; then
	echo "waiting for another container run to finish" >&2
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
