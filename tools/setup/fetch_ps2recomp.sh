#!/usr/bin/env bash
# Clones PS2Recomp (if missing), checks it out at the pinned commit and applies the
# patches listed in patches/ps2recomp/series. Safe to run repeatedly: a patch that is
# already applied is skipped.
#
# --reset restores the pinned commit first (discarding local changes to the tracked
# files and untracked files, but keeping the build output in out/).
#
# Usage, from any directory:  bash tools/setup/fetch_ps2recomp.sh [--reset]
set -euo pipefail

PIN=2c5fbb9389e11dd95693385969490c9e8e6f57b4
REPO_URL=https://github.com/ran-j/PS2Recomp.git
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR=third_party/PS2Recomp

reset=0
for arg in "$@"; do
  case "$arg" in
    --reset) reset=1 ;;
    *)
      echo "usage: bash tools/setup/fetch_ps2recomp.sh [--reset]" >&2
      exit 2
      ;;
  esac
done

# DIR is relative to the repository root.
cd "$ROOT"

if [[ ! -d "$DIR" ]]; then
  git clone "$REPO_URL" "$DIR"
  git -C "$DIR" checkout "$PIN"
fi

if [[ $reset -eq 1 ]]; then
  git -C "$DIR" checkout -f "$PIN"
  git -C "$DIR" clean -fd -e out/
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
  patch="$ROOT/patches/ps2recomp/$line"
  if git -C "$DIR" apply --reverse --check "$patch" 2>/dev/null; then
    echo "already applied: $line"
  elif git -C "$DIR" apply --check "$patch" 2>/dev/null; then
    git -C "$DIR" apply "$patch"
    echo "applied: $line"
  else
    echo "DOES NOT APPLY: $line" >&2
    exit 1
  fi
done < "$ROOT/patches/ps2recomp/series"

exit 0
