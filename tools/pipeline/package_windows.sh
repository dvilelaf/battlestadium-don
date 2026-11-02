#!/usr/bin/env bash
# Builds the Windows download build/player/don-windows-<version>.zip from the COMMITTED HEAD (see package_windows.py).
#   bash tools/pipeline/package_windows.sh [--jobs N] [--skip-build] [--from-builds DIR] [--version V] [--out DIR]
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec uv run --no-project python -I "$HERE/package_windows.py" "$@"
