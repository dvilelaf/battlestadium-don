#!/usr/bin/env bash
# Smoke run of the port: starts $DON_BUILD_DIR/don (default build/linux-dev) headless for a few
# seconds, keeps the output in work/logs/ and classifies it (tools/run/classify_log.py).
# Usage: tools/run/smoke.sh [seconds=60]
# The program's own end after --seconds (exit status 0) is the normal end of a run; `timeout` is
# only a safety net for a run that hangs before reaching its own limit. The exit code is that of
# classify_log.py (0 only for OK_VSYNC_LOOP).
set -euo pipefail

SECONDS_TO_RUN="${1:-60}"
LOG_CAP_BYTES=$((200 * 1024 * 1024))

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [ -f local.env ]; then
    set -a
    # shellcheck disable=SC1091
    . ./local.env
    set +a
fi

: "${DON_ISO:?DON_ISO is not set (see local.env.example)}"
EXE="${DON_BUILD_DIR:-build/linux-dev}/don"
ELF="disc/SLPS_256.75"
[ -x "$EXE" ] || { echo "missing $EXE: run tools/pipeline/build.sh" >&2; exit 2; }
[ -f "$ELF" ] || { echo "missing $ELF" >&2; exit 2; }

# The game must never open a window on the user's display: --headless asks the host program for
# no window at all, and whatever DISPLAY the caller had is not passed on.
unset DISPLAY WAYLAND_DISPLAY

mkdir -p work/logs
LOG="work/logs/smoke-$(date +%Y%m%d-%H%M%S).log"

# A core dump would contain the game's memory.
ulimit -c 0

export DON_TTY=1 DON_TRACE_FRAMES=1
# T-524: the program writes its log/cards/cache under its user folder; keep them in work/ (never the owner's).
export DON_USER_DIR="${DON_USER_DIR:-$PWD/work/logs/user-dir}"
mkdir -p "$DON_USER_DIR"
set +e
timeout --kill-after=5 $((SECONDS_TO_RUN + 20)) \
    "$EXE" --headless --sound off --seconds "$SECONDS_TO_RUN" "$ELF" 2>&1 | head -c "$LOG_CAP_BYTES" > "$LOG"
STATUS="${PIPESTATUS[0]}"
set -e
echo "[smoke] exit status $STATUS" >> "$LOG"
echo "smoke: log $LOG ($(wc -c < "$LOG") bytes), exit status of the run $STATUS (0 = the program's own end after --seconds, 124 = the safety timeout fired)"

set +e
uv run --no-project python -I tools/run/classify_log.py "$LOG"
CODE=$?
set -e
exit "$CODE"
