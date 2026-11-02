#!/usr/bin/env bash
# Runs Ghidra headless on the game executable: import and analysis, claim of the code that no
# function owns (ClaimOrphanCode.java) and export of the function map.
#
# Usage: bash tools/funcmap/ghidra_export.sh [--force]
#
# The claim step uses config/ghidra_seeds.txt (hand-maintained code entry points, optional) as
# seeds, so a run from scratch reproduces the versioned map.
#
# Without --force, if work/ghidra already holds an analyzed project, the import and the
# analysis are skipped; the claim step runs once (marker work/ghidra/.orphans-claimed) and the
# export script is always run (-process ... -noanalysis).
# Outputs: config/functions.ghidra.csv, work/ghidra/don.ghidra.toml, work/ghidra/analyze.log,
# work/ghidra/claim.log, work/ghidra/export.log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
G="$ROOT/third_party/ghidra/ghidra_12.1.4_PUBLIC"
WORK="$ROOT/work/ghidra"
ELF="$ROOT/disc/SLPS_256.75"
SCRIPT_DIR="$ROOT/third_party/PS2Recomp/ps2xRecomp/tools/ghidra"
CLAIM_DIR="$ROOT/tools/funcmap/ghidra"
LAYOUT="$ROOT/config/layout.toml"
SEEDS="$ROOT/config/ghidra_seeds.txt"
TOML="$WORK/don.ghidra.toml"
CSV="$ROOT/config/functions.ghidra.csv"
LOG="$WORK/analyze.log"
CLAIM_LOG="$WORK/claim.log"
PROJECT=don
RETRIES=10
PAUSE=15

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

[[ -x "$G/support/analyzeHeadless" ]] || { echo "[ghidra_export] missing $G" >&2; exit 1; }
[[ -f "$ELF" ]] || { echo "[ghidra_export] missing $ELF" >&2; exit 1; }
mkdir -p "$WORK"

# Text range from [text] of config/layout.toml (defaults when the file does not exist).
TEXT_START=0x00100000
TEXT_END=0x003902D4
if [[ -f "$LAYOUT" ]]; then
    ts=$(sed -n '/^\[text\]/,/^\[\[/{s/^start *= *\(0x[0-9A-Fa-f]*\).*/\1/p}' "$LAYOUT" | head -n 1)
    te=$(sed -n '/^\[text\]/,/^\[\[/{s/^end *= *\(0x[0-9A-Fa-f]*\).*/\1/p}' "$LAYOUT" | head -n 1)
    [[ -n "$ts" ]] && TEXT_START=$ts
    [[ -n "$te" ]] && TEXT_END=$te
fi

# Runs analyzeHeadless with the arguments given; retries while the project is locked.
# The output goes to the file named by $RUN_LOG and to the terminal.
run_ghidra() {
    local attempt status
    for ((attempt = 1; attempt <= RETRIES; attempt++)); do
        status=0
        "$G/support/analyzeHeadless" "$@" >"$RUN_LOG" 2>&1 || status=$?
        if [[ $status -ne 0 ]] && grep -qiE 'unable to lock|project is locked|in use' "$RUN_LOG" \
            && ((attempt < RETRIES)); then
            echo "[ghidra_export] project locked, retry $attempt/$RETRIES in ${PAUSE}s" >&2
            sleep "$PAUSE"
            continue
        fi
        cat "$RUN_LOG"
        return $status
    done
}

claim_args=(-scriptPath "$CLAIM_DIR" -postScript ClaimOrphanCode.java "$TEXT_START" "$TEXT_END")
[[ -f "$SEEDS" ]] && claim_args+=("$SEEDS")
export_args=(-scriptPath "$SCRIPT_DIR" -postScript ExportPS2Functions.java "$TOML" "$CSV")
both_args=(-scriptPath "$CLAIM_DIR;$SCRIPT_DIR" -postScript ClaimOrphanCode.java "$TEXT_START" "$TEXT_END")
[[ -f "$SEEDS" ]] && both_args+=("$SEEDS")
both_args+=(-postScript ExportPS2Functions.java "$TOML" "$CSV")

if [[ $FORCE -eq 1 || ! -f "$WORK/$PROJECT.gpr" || ! -f "$WORK/.analyzed" ]]; then
    rm -f "$WORK/.analyzed" "$WORK/.orphans-claimed"
    start=$(date +%s)
    RUN_LOG="$LOG" run_ghidra "$WORK" "$PROJECT" \
        -import "$ELF" -overwrite \
        -processor "$(cat "$ROOT/config/ghidra_language.txt")" \
        "${both_args[@]}"
    echo "[ghidra_export] import+analysis+claim+export took $(( $(date +%s) - start )) s"
    touch "$WORK/.analyzed" "$WORK/.orphans-claimed"
    grep -E 'ClaimOrphanCode\.java>' "$LOG" | sed 's/^/[ghidra_export] /' || true
elif [[ ! -f "$WORK/.orphans-claimed" ]]; then
    echo "[ghidra_export] analyzed project found, claiming orphan code in $TEXT_START-$TEXT_END"
    start=$(date +%s)
    RUN_LOG="$CLAIM_LOG" run_ghidra "$WORK" "$PROJECT" -process SLPS_256.75 -noanalysis \
        "${claim_args[@]}"
    echo "[ghidra_export] claim took $(( $(date +%s) - start )) s"
    grep -q 'ClaimOrphanCode\.java> claim summary' "$CLAIM_LOG" || {
        echo "[ghidra_export] claim script did not finish" >&2; exit 1; }
    grep -E 'ClaimOrphanCode\.java>' "$CLAIM_LOG" | sed 's/^/[ghidra_export] /'
    touch "$WORK/.orphans-claimed"
    RUN_LOG="$WORK/export.log" run_ghidra "$WORK" "$PROJECT" -process SLPS_256.75 -noanalysis \
        "${export_args[@]}"
else
    echo "[ghidra_export] analyzed project with orphan code claimed found, re-running only the export"
    RUN_LOG="$WORK/export.log" run_ghidra "$WORK" "$PROJECT" -process SLPS_256.75 -noanalysis \
        "${export_args[@]}"
fi

[[ -s "$CSV" && -s "$TOML" ]] || { echo "[ghidra_export] export outputs missing" >&2; exit 1; }
echo "[ghidra_export] wrote $CSV"
