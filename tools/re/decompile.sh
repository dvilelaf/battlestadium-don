#!/usr/bin/env bash
# Prints the decompiled C and the disassembly of the functions that contain the given addresses.
#
# Usage: bash tools/re/decompile.sh 0xADDR [0xADDR...]
#
# Opens the analyzed project work/ghidra (read only) and runs tools/re/ghidra/DecompileAt.java.
# Only the text between the "=== BEGIN" and "=== END" markers is printed. If the project is
# locked by another Ghidra process, it retries 10 times with a 15 s pause.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="$ROOT/third_party/ghidra/ghidra_12.1.4_PUBLIC"
WORK="$ROOT/work/ghidra"
PROJECT=don
RETRIES=10
PAUSE=15

[[ $# -ge 1 ]] || { echo "usage: decompile.sh 0xADDR [0xADDR...]" >&2; exit 2; }
[[ -x "$G/support/analyzeHeadless" ]] || { echo "[decompile] missing $G" >&2; exit 1; }
[[ -f "$WORK/$PROJECT.gpr" ]] || { echo "[decompile] missing project $WORK/$PROJECT.gpr (run tools/funcmap/ghidra_export.sh)" >&2; exit 1; }

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

for ((attempt = 1; attempt <= RETRIES; attempt++)); do
    status=0
    "$G/support/analyzeHeadless" "$WORK" "$PROJECT" -process SLPS_256.75 -noanalysis -readOnly \
        -scriptPath "$ROOT/tools/re/ghidra" -postScript DecompileAt.java "$@" >"$LOG" 2>&1 || status=$?
    if grep -q 'DecompileAt\.java> === BEGIN ' "$LOG"; then
        break
    fi
    if grep -qiE 'lock|in use|unable to lock' "$LOG" && ((attempt < RETRIES)); then
        echo "[decompile] project locked, retry $attempt/$RETRIES in ${PAUSE}s" >&2
        sleep "$PAUSE"
        continue
    fi
    if grep -qiE 'lock|in use' "$LOG"; then
        echo "[decompile] project $WORK is still locked after $RETRIES attempts" >&2
    else
        echo "[decompile] analyzeHeadless failed (exit $status); last log lines:" >&2
        tail -n 20 "$LOG" >&2
    fi
    exit 1
done

# Keep only the script output, drop Ghidra's "INFO  DecompileAt.java> " prefix.
sed -n 's/^[A-Z]* *DecompileAt\.java> //; T; s/ (GhidraScript) *$//; p' "$LOG" | sed -n '/^=== BEGIN /,/^=== END /p'
