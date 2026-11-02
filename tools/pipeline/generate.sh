#!/usr/bin/env bash
# Runs the translation chain: the analyzer (only if its config is missing), make_config.py,
# the recompiler and the audit of its log. Outputs: generated/ (C++ made from the game; local,
# never versioned), work/logs/recomp.log and work/reports/translation_audit.json.
# The recompiler writes into work/gen-staging; generated/ is then updated so that only files
# whose content changed are rewritten (unchanged files keep their modification time, so the
# build does not recompile them), files that no longer exist are removed, and the lists that
# CMake reads (generated/sources.txt: ordinary files, generated/sources_big.txt: files of at
# least 200000 bytes, which are compiled alone) are rewritten only when their content changes.
# Usage: generate.sh [--lists-only]   (--lists-only only rewrites the two lists)
# Environment: DON_MANUAL_EXTRA=<file> is passed to make_config.py as --manual-extra.
# Exit code: the failing step's code if the analyzer or make_config.py fails; otherwise the exit
# code of audit.py. If the recompiler fails, the audit still runs and the script exits with 1.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# Updates generated/ from a staging folder (argument 1; empty = only rewrite the lists).
sync_generated() {
  uv run --no-project python -I - "$1" <<'PYEOF'
import os, sys
from pathlib import Path

BIG_BYTES = 200000
LISTS = ("sources.txt", "sources_big.txt")
gen = Path("generated")
stage = Path(sys.argv[1]) if sys.argv[1] else None
gen.mkdir(exist_ok=True)

def same(a, b):
    if a.stat().st_size != b.stat().st_size:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True

if stage is not None:
    added = changed = removed = kept = 0
    new = {p.relative_to(stage): p for p in stage.rglob("*") if p.is_file()}
    for rel, src in sorted(new.items()):
        dst = gen / rel
        if not dst.exists():
            added += 1
        elif same(src, dst):
            kept += 1
            continue
        else:
            changed += 1
        dst.parent.mkdir(parents=True, exist_ok=True)
        os.replace(src, dst)
    for p in sorted(gen.rglob("*")):
        # iop_native/ is made by tools/iop_recomp/make_native.py, not by the recompiler: it stays.
        if p.is_file() and p.name not in LISTS and p.relative_to(gen) not in new and p.relative_to(gen).parts[0] != "iop_native":
            p.unlink()
            removed += 1
    print(f"[generate] sync: {added} added, {changed} changed, {removed} removed, {kept} left alone")

def write_if_changed(path, text):
    if path.exists() and path.read_text() == text:
        return False
    path.write_text(text)
    return True

cpps = sorted(p.name for p in gen.glob("*.cpp"))
big = [n for n in cpps if (gen / n).stat().st_size >= BIG_BYTES]
bigset = set(big)
rewritten = [
    write_if_changed(gen / "sources.txt", "".join(n + "\n" for n in cpps if n not in bigset)),
    write_if_changed(gen / "sources_big.txt", "".join(n + "\n" for n in big)),
]
print(f"[generate] lists: {len(cpps) - len(big)} ordinary + {len(big)} big sources, "
      f"{'rewritten' if any(rewritten) else 'unchanged'}")
PYEOF
}

if [[ "${1:-}" == "--lists-only" ]]; then
  sync_generated ""
  exit 0
fi

BUILD="third_party/PS2Recomp/out/build-tools"
ANALYZER="$BUILD/ps2xAnalyzer/ps2_analyzer"
RECOMP="$BUILD/ps2xRecomp/ps2_recomp"

mkdir -p work/analyzer work/logs work/reports

if [[ -f work/analyzer/config.toml ]]; then
  echo "[generate] analyzer: work/analyzer/config.toml exists, skipped"
else
  echo "[generate] analyzer: work/analyzer/config.toml missing, running ps2_analyzer"
  rc=0
  "$ANALYZER" disc/SLPS_256.75 work/analyzer/config.toml > work/analyzer/analyzer.log 2>&1 || rc=$?
  if (( rc != 0 )); then
    echo "[generate] analyzer failed with exit code $rc (see work/analyzer/analyzer.log)"
    exit "$rc"
  fi
  echo "[generate] analyzer: wrote work/analyzer/config.toml"
fi

rc=0
MAKE_CONFIG_ARGS=(--generated-dir work/gen-staging)
if [[ -n "${DON_MANUAL_EXTRA:-}" ]]; then
  MAKE_CONFIG_ARGS+=(--manual-extra "$DON_MANUAL_EXTRA")
fi
uv run --no-project python -I tools/pipeline/make_config.py "${MAKE_CONFIG_ARGS[@]}" || rc=$?
if (( rc != 0 )); then
  echo "[generate] make_config.py failed with exit code $rc"
  exit "$rc"
fi
echo "[generate] make_config.py: wrote work/don.toml"

echo "[generate] recompiler: running ps2_recomp into work/gen-staging"
rm -rf work/gen-staging
start=$(date +%s)
recomp_rc=0
"$RECOMP" work/don.toml > work/logs/recomp.log 2>&1 || recomp_rc=$?
echo "[generate] recompiler: took $(( $(date +%s) - start )) s, exit code $recomp_rc (log: work/logs/recomp.log)"

audit_rc=0
uv run --no-project python -I tools/pipeline/audit.py \
  --log work/logs/recomp.log --layout config/layout.toml --out work/gen-staging \
  --exit-code "$recomp_rc" || audit_rc=$?
echo "[generate] audit: exit code $audit_rc (report: work/reports/translation_audit.json)"

if (( recomp_rc != 0 )); then
  echo "[generate] the recompiler failed, so the run fails (generated/ was not touched)"
  exit 1
fi
sync_generated work/gen-staging
rm -rf work/gen-staging

# The translated IOP sound module (generated/iop_native/, patch 0085) from the same disc. A failure only means the
# build goes on without it (the IOP interprets the module), so it does not fail the run.
if ! uv run --no-project --with rabbitizer python -I tools/iop_recomp/make_native.py --disc disc; then
  echo "[generate] iop_native: could not be made, the build will not include it"
fi
exit "$audit_rc"
