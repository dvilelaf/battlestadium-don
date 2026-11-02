#!/usr/bin/env bash
# Builds the build image `don-build:1` (see tools/container/Dockerfile).
# The context is a temporary directory: this folder plus the dependency patches that PCSX2's CI
# applies, copied from third_party/pcsx2 (never committed here). Extra arguments go to `docker build`.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
TAG=don-build:1
PATCHES_SRC="$ROOT/third_party/pcsx2/.github/workflows/scripts/common"

if [ ! -f "$PATCHES_SRC/shaderc-changes.patch" ]; then
	echo "[don] $PATCHES_SRC/shaderc-changes.patch not found; run tools/setup/fetch_pcsx2.sh first" >&2
	exit 1
fi

CTX=$(mktemp -d)
trap 'rm -rf "$CTX"' EXIT
cp "$ROOT/tools/container/Dockerfile" "$ROOT/tools/container/build-deps.sh" "$CTX/"
mkdir "$CTX/patches"
cp "$PATCHES_SRC/shaderc-changes.patch" "$CTX/patches/"

DOCKER_BUILDKIT=1 docker build --tag "$TAG" "$@" "$CTX"
echo "[don] built $TAG"
