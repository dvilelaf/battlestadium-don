#!/usr/bin/env bash
# Replays every GS dump of the reference PCSX2 through our bridge (gsreplay) and through PCSX2's own
# headless dump runner (pcsx2-gsrunner), with the same renderer, and compares the two images.
#
#   bash tools/verify/replay_all.sh [--renderers software,vulkan] [--min-ssim 0.98] [dump ...]
#
# Without dump arguments it takes work/ref/pcsx2/*.gs.zst (or $DON_GS_DUMP_DIR/*.gs.zst).
# Renderers: default "auto" = software, plus Vulkan when this machine has a Vulkan device that opens.
# Builds nothing: run `bash tools/pipeline/build_gs.sh` first (it also builds the runner).
#
# The check that counts is gsreplay against pcsx2-gsrunner: both come from the same GS code, so they
# must agree (SSIM >= 0.98). The screenshot saved next to the dump is one frame older than the dump
# (T-107 card), so the comparison against it is printed as information only.
#
# Everything is written under work/gsdump/ (ignored by git): in/ (copies of the dumps, the originals
# are never touched), out/ (images and difference images), runner/ (the runner's frames and logs).
# Exit code: 0 all OK; 1 at least one FAIL or error; 2 something missing (dumps, build).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

GS="${DON_GS_BUILD_DIR:-$ROOT/build/gs}"
WORK="$ROOT/work/gsdump"
SRC_DIR="${DON_GS_DUMP_DIR:-$ROOT/work/ref/pcsx2}"
MIN_SSIM=0.98
RENDERERS=auto
DUMPS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --renderers) RENDERERS="$2"; shift 2 ;;
    --min-ssim) MIN_SSIM="$2"; shift 2 ;;
    -h|--help) sed -n 2,19p "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) echo "error: unknown option $1" >&2; exit 2 ;;
    *) DUMPS+=("$1"); shift ;;
  esac
done

if [[ ! -x "$GS/gsreplay" || ! -x "$GS/pcsx2-gsrunner" ]]; then
  echo "error: $GS/gsreplay or $GS/pcsx2-gsrunner is missing; run: bash tools/pipeline/build_gs.sh" >&2
  exit 2
fi
if [[ ${#DUMPS[@]} -eq 0 ]]; then
  shopt -s nullglob
  DUMPS=("$SRC_DIR"/*.gs.zst "$SRC_DIR"/*.gs)
  shopt -u nullglob
fi
if [[ ${#DUMPS[@]} -eq 0 ]]; then
  echo "error: no GS dump found in $SRC_DIR (*.gs.zst); take them with tools/ref/pcsx2_auto.sh" >&2
  exit 2
fi

# The programs load on the host when its libraries allow it; otherwise inside the build container.
PREFIX=()
if "$GS/gsreplay" 2>&1 | grep -q "error while loading shared libraries"; then
  PREFIX=(bash "$ROOT/tools/container/run.sh")
  echo "[replay_all] the host cannot load the programs; running them in the build container"
fi

mkdir -p "$WORK/in" "$WORK/out" "$WORK/runner" "$WORK/xdg"
# PCSX2 keeps its settings and caches under XDG_CONFIG_HOME: keep them out of the user's home.
export XDG_CONFIG_HOME="$WORK/xdg"
export PCSX2_NOCONSOLE=1
# Make the runner save the image at the GS's internal resolution, like don_gs_snapshot_rgba
# (ScreenshotSize 2 = InternalResolutionUncorrected); its default is the window size.
printf '[EmuCore/GS]\nScreenshotSize = 2\n' > "$WORK/runner.ini"

parity() { # parity <reference.png> <image.png> [diff.png]
  uv run --no-project --with pillow --with numpy --with scikit-image python -I \
    "$ROOT/tools/verify/image_parity.py" "$1" "$2" --min-ssim "$MIN_SSIM" ${3:+--diff "$3"}
}

if [[ "$RENDERERS" == auto ]]; then
  RENDERERS=software
  probe="$WORK/out/probe-vulkan.png"
  if "${PREFIX[@]}" "$GS/gsreplay" --synthetic "$probe" --renderer vulkan >/dev/null 2>&1; then
    RENDERERS=software,vulkan
  else
    echo "[replay_all] no usable Vulkan device on this machine: Vulkan comparison skipped"
  fi
fi
IFS=, read -r -a RENDERER_LIST <<< "$RENDERERS"

fails=0
total=0
for dump in "${DUMPS[@]}"; do
  base="$(basename "$dump")"
  name="${base%.gs.zst}"; name="${name%.gs}"
  if [[ ! -f "$dump" ]]; then
    echo "$name: MISSING ($dump)"; fails=$((fails + 1)); continue
  fi
  copy="$WORK/in/$base"
  cp -f "$dump" "$copy"
  shot="$(dirname "$dump")/$name.png"
  for renderer in "${RENDERER_LIST[@]}"; do
    total=$((total + 1))
    tag="$name.$renderer"
    ours="$WORK/out/$tag.gsreplay.png"
    rm -f "$ours"
    rdir="$WORK/runner/$tag"
    rm -rf "$rdir"; mkdir -p "$rdir"
    runner_renderer="$renderer"; [[ "$renderer" == software ]] && runner_renderer=sw

    t0=$(date +%s.%N)
    "${PREFIX[@]}" "$GS/gsreplay" --dump "$copy" "$ours" --renderer "$renderer" \
      >"$WORK/out/$tag.gsreplay.log" 2>&1
    rc_ours=$?
    t1=$(date +%s.%N)
    "${PREFIX[@]}" "$GS/pcsx2-gsrunner" -renderer "$runner_renderer" -ini "$WORK/runner.ini" -surfaceless \
      -dumpdir "$rdir" -logfile "$rdir/emulog.txt" -- "$copy" </dev/null >"$rdir/stdout.log" 2>&1
    rc_ref=$?
    t2=$(date +%s.%N)
    # The runner writes <name>_frameNNNNN.png for each frame it presents; the last one is the image
    # after the dump's last vertical sync (frame N is the picture composed at vsync N+1).
    ref="$(ls "$rdir"/*_frame*.png 2>/dev/null | sort | tail -n 1)"

    times="gsreplay $(awk -v a=$t0 -v b=$t1 'BEGIN{printf "%.1f", b-a}') s, runner $(awk -v a=$t1 -v b=$t2 'BEGIN{printf "%.1f", b-a}') s"
    if [[ $rc_ours -ne 0 || ! -f "$ours" ]]; then
      echo "$tag ERROR gsreplay failed (exit $rc_ours): $(tail -n 2 "$WORK/out/$tag.gsreplay.log" | tr '\n' ' ')"
      fails=$((fails + 1)); continue
    fi
    if [[ $rc_ref -ne 0 || -z "$ref" ]]; then
      echo "$tag ERROR pcsx2-gsrunner failed (exit $rc_ref, no frame written); see $rdir/emulog.txt"
      fails=$((fails + 1)); continue
    fi
    diff_png="$WORK/out/$tag.diff.png"
    rm -f "$diff_png"
    result="$(parity "$ref" "$ours" "$diff_png")"; rc=$?
    # Keep a difference image only when the comparison failed.
    if [[ $rc -eq 0 ]]; then rm -f "$diff_png"; else fails=$((fails + 1)); fi
    info="no screenshot"
    if [[ -f "$shot" ]]; then
      info="$(parity "$shot" "$ours" | sed 's/ OK$//; s/ FAIL$//')"
    fi
    echo "$tag $result (gsreplay against pcsx2-gsrunner, $(basename "$ref")) | info: against screenshot $info | $times"
    [[ $rc -ne 0 ]] && echo "  difference image: $diff_png; images: $ours and $ref"
  done
done

if [[ $fails -eq 0 ]]; then
  echo "replay_all: all $total comparisons OK (SSIM >= $MIN_SSIM)"
  exit 0
fi
echo "replay_all: $fails of $total comparisons failed or could not run" >&2
exit 1
