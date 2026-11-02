#!/usr/bin/env bash
# Usage: bash tools/ref/name_capture.sh <name>
# Moves the newest screenshot (.png), savestate (.p2s) and single-frame GS dump (.gs.zst) from
# work/ref/pcsx2/raw/ to work/ref/pcsx2/<name>.png, <name>.p2s and <name>.gs.zst.
# Fails, and moves nothing, if any of the three is missing. A capture that already has this
# name is replaced.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

RAW="work/ref/pcsx2/raw"
OUT="work/ref/pcsx2"
NAMES="01-title 02-menu 03-charselect 04-match-1v1 05-match-4p 06-results"

die() { echo "[name_capture] ERROR: $*" >&2; exit 1; }

if [[ $# -ne 1 ]]; then
    echo "usage: bash tools/ref/name_capture.sh <name>" >&2
    echo "names: $NAMES" >&2
    exit 1
fi
name="$1"
case " $NAMES " in
    *" $name "*) ;;
    *) die "unknown name '$name'; use one of: $NAMES" ;;
esac
[[ -d "$RAW" ]] || die "$RAW does not exist; run tools/ref/run_pcsx2.sh and capture first"

# newest FILE_SUFFIX: path of the newest file in $RAW whose name ends with the suffix
newest() {
    find "$RAW" -maxdepth 1 -type f -name "*$1" -printf '%T@ %p\n' | sort -n | tail -n 1 | cut -d' ' -f2-
}

png="$(newest .png)"
p2s="$(newest .p2s)"
gs="$(newest .gs.zst)"

missing=0
[[ -n "$png" ]] || { echo "[name_capture] missing: a screenshot (.png) in $RAW (key F8)" >&2; missing=1; }
[[ -n "$p2s" ]] || { echo "[name_capture] missing: a savestate (.p2s) in $RAW (key F1)" >&2; missing=1; }
[[ -n "$gs" ]] || { echo "[name_capture] missing: a GS dump (.gs.zst) in $RAW (keys Shift+F8)" >&2; missing=1; }
[[ $missing -eq 0 ]] || die "nothing was moved"

mv -f -- "$png" "$OUT/$name.png"
mv -f -- "$p2s" "$OUT/$name.p2s"
mv -f -- "$gs" "$OUT/$name.gs.zst"
echo "[name_capture] $name:"
echo "  $png -> $OUT/$name.png"
echo "  $p2s -> $OUT/$name.p2s"
echo "  $gs -> $OUT/$name.gs.zst"
