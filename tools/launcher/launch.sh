#!/usr/bin/env bash
# Linux/macOS wrapper for the launcher (tools/launcher/launch.py); adapted from the f7gs project's launch.sh.
# Runs from the repository root. DON_PYTHON overrides the interpreter.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ -n "${DON_PYTHON:-}" ]; then
    exec "$DON_PYTHON" tools/launcher/launch.py "$@"
elif command -v uv >/dev/null 2>&1; then
    exec uv run --no-project python -I tools/launcher/launch.py "$@"
elif command -v python3 >/dev/null 2>&1; then
    exec python3 -I tools/launcher/launch.py "$@"
fi
echo "Python 3 is required for the launcher (install uv or python3, or set DON_PYTHON)" >&2
exit 1
