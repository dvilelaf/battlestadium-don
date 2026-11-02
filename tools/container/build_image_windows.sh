#!/usr/bin/env bash
# Builds the Windows cross-build image `don-build-win:1` (see tools/container/Dockerfile.windows)
# on top of `don-build:1`. Extra arguments go to `docker build`.
#
# The Microsoft CRT and Windows SDK are not part of the image. Fetch them once with:
#   tools/container/run_windows.sh xwin --accept-license --cache-dir third_party/xwin-cache \
#       --arch x86_64 --variant desktop splat --output third_party/xwin/sysroot
# (the owner accepted Microsoft's license terms on 2026-10-11; the result stays in third_party/,
# which git ignores, and is never redistributed).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TAG=don-build-win:1

if ! docker image inspect don-build:1 >/dev/null 2>&1; then
	echo "[don] image don-build:1 not found; run tools/container/build_image.sh first" >&2
	exit 1
fi

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp "$ROOT/tools/container/Dockerfile.windows" "$CTX/Dockerfile"

DOCKER_BUILDKIT=1 docker build --tag "$TAG" "$@" "$CTX"
echo "[don] built $TAG"
