#!/usr/bin/env bash
# Fetches PCSX2 (if missing) at the pinned commit and applies the patches listed in
# patches/pcsx2/series. Safe to run repeatedly: a patch that is already applied is skipped.
# An empty series file is valid and applies nothing.
#
# The clone is shallow (only the pinned commit). Submodules: PCSX2 v2.8.2 has none (its
# dependencies come from a prebuilt package or from the build container), so none are initialised.
#
# --reset restores the pinned commit first (discarding local changes to the tracked
# files and untracked files).
#
# Usage, from any directory:  bash tools/setup/fetch_pcsx2.sh [--reset]
set -euo pipefail

PIN=fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3   # tag v2.8.2
REPO_URL=https://github.com/PCSX2/pcsx2.git
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR=third_party/pcsx2

reset=0
for arg in "$@"; do
  case "$arg" in
    --reset) reset=1 ;;
    *)
      echo "usage: bash tools/setup/fetch_pcsx2.sh [--reset]" >&2
      exit 2
      ;;
  esac
done

# DIR is relative to the repository root.
cd "$ROOT"

if [[ ! -d "$DIR/.git" ]]; then
  mkdir -p "$DIR"
  git -C "$DIR" init -q
  git -C "$DIR" remote add origin "$REPO_URL"
  git -C "$DIR" fetch -q --depth 1 origin "$PIN"
  git -C "$DIR" checkout -q FETCH_HEAD
fi

if [[ $reset -eq 1 ]]; then
  git -C "$DIR" checkout -q -f "$PIN"
  git -C "$DIR" clean -fdq
fi

head="$(git -C "$DIR" rev-parse HEAD)"
if [[ "$head" != "$PIN" ]]; then
  echo "error: $DIR is at $head, expected $PIN. Run this script with --reset." >&2
  exit 1
fi

while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ -z "$line" ]]; then
    continue
  fi
  patch="$ROOT/patches/pcsx2/$line"
  if git -C "$DIR" apply --reverse --check "$patch" 2>/dev/null; then
    echo "already applied: $line"
  elif git -C "$DIR" apply --check "$patch" 2>/dev/null; then
    git -C "$DIR" apply "$patch"
    echo "applied: $line"
  else
    echo "DOES NOT APPLY: $line" >&2
    exit 1
  fi
done < "$ROOT/patches/pcsx2/series"

exit 0
