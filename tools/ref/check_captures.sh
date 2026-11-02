#!/usr/bin/env bash
# Usage: bash tools/ref/check_captures.sh
# Lists the 18 expected reference files (6 moments x .png, .p2s, .gs.zst) in work/ref/pcsx2/ as
# present or missing. Exit 0 only if all 18 are present.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT="work/ref/pcsx2"
NAMES="01-title 02-menu 03-charselect 04-match-1v1 05-match-4p 06-results"
EXTS="png p2s gs.zst"

present=0
missing=0
for name in $NAMES; do
    for ext in $EXTS; do
        f="$OUT/$name.$ext"
        if [[ -f "$f" ]]; then
            echo "present  $f"
            present=$((present + 1))
        else
            echo "missing  $f"
            missing=$((missing + 1))
        fi
    done
done

echo "$present of 18 present, $missing missing"
[[ $missing -eq 0 ]]
