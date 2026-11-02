#!/usr/bin/env python3
"""Compare the speed of a candidate build with a baseline build in the 4-player ougi fight (the pre-handover gate).

What: runs tools/run/perf_fight.py (ougi fight, 7800 blanks, --renderer vulkan --sound on by default) N times
for the baseline binary and N times for the candidate, alternating (B C B C ...) so a slow minute of the machine
hits both. Prints per run the game thread's CPU ms per blank over the fight (the busiest thread, from
perf_fight.py), its CPU %, the worst 10 s window (fps, from the game's "[don] exit:" line, advisory only) and
the state hash; then a verdict.

Verdict: FAIL (exit 1) when
  - any run has no result (the fight was not reached, or no game-thread time was measured), or
  - the state hashes differ (between the runs or between the builds): it is not the same game, or
  - the candidate's median game-thread ms per blank is higher than the baseline's median by more than the
    baseline's own spread (max - min of the baseline's runs).
Why ms per blank and not fps: on a shared machine with software Vulkan (llvmpipe) the fps depends on what else
is running (a build compared with itself gave 26.4 vs 20.7 fps, same hash), while the game thread's CPU time
per blank stays within a narrow band (T-753: 11.05-11.75 ms across runs). The worst-window fps is printed as
advisory and never decides the Linux verdict (on a Windows PC with a GPU, fight_compare.ps1 compares fps).
Why the spread and not a fixed percentage: how noisy the number is depends on the machine and the renderer
(llvmpipe in the sandbox, a GPU on a PC), so a fixed threshold would either fail on noise or hide a real
loss. The baseline's own runs say how far two runs of the same build differ here; a difference bigger than that
is not noise. On 2026-10-28 the module build lost 58.4 -> 54.3 fps (Windows) with a baseline spread of 0.1 fps.
With --runs 1 the spread is 0, so any loss fails: use at least 3 runs for a real verdict.

How (from the repository root; module builds need their prepared user folder and the module):

    uv run --no-project python -I tools/run/fight_compare.py --baseline OLD/don --candidate NEW/don
    uv run --no-project python -I tools/run/fight_compare.py --baseline A --candidate B --runs 5 \\
        --candidate-user-dir work/user-new --candidate-module NEW/libdon_game.so
"""
import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PERF = Path(__file__).resolve().parent / "perf_fight.py"
GAME_TIMEOUT = 900  # seconds; a fight is 130 s at 60 fps, 200 s at the most (perf_fight --seconds)


def median(values):
    return statistics.median(values)


def spread(values):
    return max(values) - min(values)


def failed(run):
    return run.get("ms") is None


def verdict(base, cand):
    """(ok, lines) from two lists of runs {"ms": game-thread ms per blank or None, "worst": fps (advisory), "cpu": %, "hash": str}."""
    lines = []
    bad = [("baseline", r) for r in base if failed(r)] + [("candidate", r) for r in cand if failed(r)]
    if bad:
        lines.append(f"FAIL: {len(bad)} run(s) with no result ({', '.join(who for who, _ in bad)})")
        return False, lines
    ok = True
    hashes = {r["hash"] for r in base + cand}
    if len(hashes) != 1:
        ok = False
        lines.append(f"FAIL: state hashes differ ({len(hashes)} different): not the same game")
    base_ms = [r["ms"] for r in base]
    cand_ms = [r["ms"] for r in cand]
    base_median, cand_median, base_spread = median(base_ms), median(cand_ms), spread(base_ms)
    lines.append(f"baseline game thread: median {base_median:.2f} ms per blank, spread {base_spread:.2f}")
    lines.append(f"candidate game thread: median {cand_median:.2f} ms per blank, spread {spread(cand_ms):.2f}")
    fps = [r["worst"] for r in base + cand]
    if all(w is not None for w in fps):
        lines.append(f"advisory worst 10 s window: baseline median {median([r['worst'] for r in base]):.1f} fps, "
                     f"candidate median {median([r['worst'] for r in cand]):.1f} fps (not part of the verdict)")
    if cand_median > base_median + base_spread:
        ok = False
        lines.append(f"FAIL: candidate is slower: {cand_median:.2f} > {base_median:.2f} + {base_spread:.2f} ms per blank")
    if ok:
        lines.append("PASS: state hashes equal, candidate game thread not slower than the baseline's spread")
    return ok, lines


def one_run(binary, user_dir, module, args):
    """Run perf_fight once; the run dict, with worst None when it produced no result."""
    command = [sys.executable, "-I", str(PERF), "--binary", binary, "--renderer", args.renderer,
               "--sound", args.sound, "--json"]
    if user_dir:
        command += ["--user-dir", user_dir]
    if module:
        command += ["--game-module", module]
    try:
        done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=GAME_TIMEOUT)
        data = json.loads(done.stdout)
    except (subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        return {"ms": None, "worst": None, "cpu": None, "hash": None, "note": type(error).__name__}
    cpu = data["cpu_percent_of_core"].get(data["busiest_thread"])
    ms = data["cpu_ms_per_blank"].get(data["busiest_thread"]) if data["reached_fight_end"] else None
    return {"ms": ms, "worst": data["worst_10s_fps"], "cpu": cpu, "hash": data["state_sha256"], "note": data["log"]}


def show(label, index, run):
    cpu = "n/a" if run["cpu"] is None else f"{run['cpu']:.1f}%"
    worst = "n/a" if run["worst"] is None else f"{run['worst']:.1f}"
    ms = "n/a" if run["ms"] is None else f"{run['ms']:.2f}"
    digest = (run["hash"] or "n/a")[:16]
    print(f"{label:<9} run {index}: game thread {ms} ms per blank ({cpu})  worst 10 s window {worst} fps  hash {digest}  ({run['note']})",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--baseline", required=True, help="the last good build's executable")
    parser.add_argument("--candidate", required=True, help="the build to hand over")
    parser.add_argument("--runs", type=int, default=3, help="runs per build, alternating (default 3)")
    parser.add_argument("--renderer", default="vulkan")
    parser.add_argument("--sound", choices=("on", "off"), default="on")
    for side in ("baseline", "candidate"):
        parser.add_argument(f"--{side}-user-dir", default=None, help=f"prepared user folder of the {side}")
        parser.add_argument(f"--{side}-module", default=None, help=f"game module (libdon_game.so) of the {side}")
    args = parser.parse_args()

    base, cand = [], []
    for i in range(1, args.runs + 1):
        base.append(one_run(args.baseline, args.baseline_user_dir, args.baseline_module, args))
        show("baseline", i, base[-1])
        cand.append(one_run(args.candidate, args.candidate_user_dir, args.candidate_module, args))
        show("candidate", i, cand[-1])
    ok, lines = verdict(base, cand)
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
