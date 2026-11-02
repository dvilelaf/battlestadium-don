#!/usr/bin/env bash
# Verifies a disc image and, if it is a known variant, extracts the files the port needs.
# Usage: tools/disc/extract.sh <iso> [destination=disc]
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: extract.sh <iso> [destination=disc]" >&2
    exit 1
fi

ISO="$1"
DEST="${2:-disc}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# verify_disc.py exits non-zero for an unknown or wrong disc; set -e stops here in that case.
uv run --no-project python -I "$HERE/verify_disc.py" "$ISO"

7z x -y "-o$DEST" "$ISO" SLPS_256.75 SYSTEM.CNF DIR.DIR MODULES
