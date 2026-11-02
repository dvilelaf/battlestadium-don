import sys
from pathlib import Path

# python -I does not add the script folder to sys.path; the tool lives in tools/run/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "run"))

import fight_compare  # noqa: E402


def runs(ms, digest="h1", fps=50.0):
    """Fake runs: game thread ms per blank (None = no result); fps is advisory only."""
    return [{"ms": m, "worst": fps, "cpu": 40.0, "hash": digest} for m in ms]


def test_candidate_inside_the_baseline_spread_passes():
    # baseline 11.05-11.75 (spread 0.70, median 11.30); candidate median 11.9 is within one spread above it
    ok, lines = fight_compare.verdict(runs([11.3, 11.05, 11.75]), runs([11.9, 11.2, 11.95]))
    assert ok
    assert any(line.startswith("PASS") for line in lines)


def test_candidate_faster_than_baseline_passes():
    ok, _ = fight_compare.verdict(runs([12.5, 12.4, 12.6]), runs([10.9, 11.0, 10.8]))
    assert ok


def test_candidate_slower_than_baseline_spread_fails():
    ok, lines = fight_compare.verdict(runs([10.9, 11.0, 10.95]), runs([12.5, 12.6, 12.4]))
    assert not ok
    assert any(line.startswith("FAIL") and "slower" in line for line in lines)


def test_fps_is_advisory_it_never_decides():
    # the candidate is much slower in fps (a noisy machine) but the game thread is the same: PASS
    ok, lines = fight_compare.verdict(runs([11.0, 11.1, 11.2], fps=26.4), runs([11.0, 11.1, 11.2], fps=20.7))
    assert ok
    assert any("advisory" in line for line in lines)


def test_different_state_hashes_fail_even_when_faster():
    ok, lines = fight_compare.verdict(runs([12.0, 12.1, 12.2], "a"), runs([10.0, 10.1, 10.2], "b"))
    assert not ok
    assert any(line.startswith("FAIL") and "hash" in line for line in lines)


def test_hashes_differing_between_runs_of_one_build_fail():
    mixed = runs([11.0, 11.0], "a") + runs([11.0], "b")
    ok, _ = fight_compare.verdict(mixed, runs([11.0, 11.0, 11.0], "a"))
    assert not ok


def test_a_run_without_game_thread_time_fails():
    ok, lines = fight_compare.verdict(runs([11.0, 11.0]), runs([11.0, None]))
    assert not ok
    assert any(line.startswith("FAIL") and "no result" in line for line in lines)


def test_median_and_spread():
    assert fight_compare.median([3.0, 1.0, 2.0]) == 2.0
    assert fight_compare.median([1.0, 2.0, 3.0, 4.0]) == 2.5
    assert fight_compare.spread([34.7, 31.8, 34.1]) == 34.7 - 31.8
