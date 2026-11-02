#!/usr/bin/env bash
# Runs a command inside the build image `don-build:1`.
#
#   tools/container/run.sh <command...>
#
# The repository is mounted at the same absolute path as on the host and is the working
# directory; the command runs as the invoking uid:gid so created files are not owned by root.
# Environment variables whose names start with DON_ are passed through.
#
# Memory and concurrency (added 2026-10-13). On 2026-10-13 the owner's machine (32 GB) froze and had to
# be reset: three agents were building and running the game at once and the container had no memory
# limit, so a build that ran out of memory took the desktop down with it. Two rules prevent it now:
#
#   * the container gets --memory and --memory-swap with the SAME value (no extra swap), taken from
#     DON_CONTAINER_MEMORY (for example "16g"). The default is 55 % of MemTotal, rounded down to whole
#     GiB; DON_CONTAINER_MEMORY=none turns the limit off. A build that needs more then dies inside the
#     container (one compiler killed, a visible error) instead of freezing the host.
#   * one container run of this project at a time: an exclusive flock on build/.container.lock, taken
#     *inside* the container (`flock <lock> <command...>`: flock execs the command with the lock held),
#     so it lives exactly as long as the run. A short host-side probe prints
#     "waiting for another container run to finish" once and serialises the common case; it is released
#     before the container starts. (The first version took the lock in the host shell and a killed
#     docker client released it while its container was still building.)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
IMAGE=${DON_BUILD_IMAGE:-don-build:1}

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

# ccache (added 2026-10-26, T-706): with CCACHE_DIR set on the host, the cache is mounted at the same path and
# the ccache and CMake launcher variables are passed on, so a fresh build folder reuses compiled objects. Unset,
# nothing changes. CCACHE_BASEDIR is the repository, which the container mounts at the same path.
if [[ -n "${CCACHE_DIR:-}" ]]; then
	mkdir -p "$CCACHE_DIR"
	args+=(-v "$CCACHE_DIR:$CCACHE_DIR" -e CCACHE_DIR -e CCACHE_BASEDIR \
		-e CMAKE_C_COMPILER_LAUNCHER -e CMAKE_CXX_COMPILER_LAUNCHER)
fi

while IFS='=' read -r name _; do
	args+=(-e "$name")
done < <(env | grep '^DON_' || true)

exec docker run "${args[@]}" "$IMAGE" flock "$LOCK" "$@"
