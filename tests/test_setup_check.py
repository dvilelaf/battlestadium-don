"""Tests of tools/run/setup_check.py: its log classifier, its verdicts and its summary (no game, no program run)."""
import re
import signal
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "run"))
import setup_check  # noqa: E402
import setup_check_build  # noqa: E402
import setup_check_paths  # noqa: E402

CLEAN = "[don] window title: x\n[don] setup screen: welcome\n[don] setup screen: checking-error E-PICK-3\n"


def test_clean_log_has_no_bad_line():
    assert setup_check.classify_log(CLEAN) == []


def test_every_pattern_is_found():
    for line in ("==1==ERROR: AddressSanitizer: heap-use-after-free", "a.cpp:1:2: runtime error: shift", "Assertion failed",
                 "terminate called, abort", "FATAL: no", "[don] cannot open: error 5"):
        assert len(setup_check.classify_log(line)) == 1, line


def test_a_screen_trace_line_is_not_an_error():
    assert setup_check.classify_log("[don] setup screen: preparing-error") == []


def test_one_line_is_reported_once():
    assert len(setup_check.classify_log("fatal error: assertion abort")) == 1


def test_screens_of_keeps_order():
    assert setup_check.screens_of(CLEAN) == ["welcome", "checking-error E-PICK-3"]


def test_judge_pass():
    ok, seen = setup_check.judge("p", 0, False, CLEAN, ["welcome", "checking-error E-PICK-3"], 2)
    assert ok and seen == "screens welcome > checking-error E-PICK-3"


def test_a_wrong_error_code_is_a_fail():
    ok, seen = setup_check.judge("p", 0, False, CLEAN, ["welcome", "checking-error E-DISC-3"], 2)
    assert not ok and "expected ['welcome', 'checking-error E-DISC-3']" in seen


def test_every_error_screen_expects_its_code():
    for p in setup_check_paths.PATHS:
        for screen in p.expected or []:
            if screen.split()[0].endswith("-error"):
                assert re.fullmatch(r"\S+-error E-[A-Z]+-\d+", screen), (p.name, screen)


def test_judge_crash_exit_hang_and_log():
    assert "SIGSEGV" in setup_check.judge("p", -signal.SIGSEGV, False, CLEAN, None, 2)[1]
    assert "exit code 3" in setup_check.judge("p", 3, False, CLEAN, None, 2)[1]
    assert "hung" in setup_check.judge("p", None, True, CLEAN, None, 2)[1]
    ok, seen = setup_check.judge("p", 0, False, CLEAN + "runtime error: x\n", None, 2)
    assert not ok and "UndefinedBehaviorSanitizer" in seen


def test_judge_wrong_screens_and_missing_shots():
    ok, seen = setup_check.judge("p", 0, False, CLEAN, ["welcome"], 2)
    assert not ok and "expected ['welcome']" in seen
    ok, seen = setup_check.judge("p", 0, False, CLEAN, None, 1)
    assert not ok and "1 screenshots for 2 screens" in seen


def test_summary_line():
    assert setup_check.summary_line(21, 1, 263.4) == "setup_check: 21 passed, 1 failed (263 s)"


def test_auto_hold_follows_the_keys():
    p = setup_check_paths.Path_("x", "down,down,confirm", hold=0.0)
    assert p.hold == 3 * setup_check_paths.SECONDS_PER_KEY + setup_check_paths.HOLD_EXTRA


def test_path_names_are_unique():
    names = [p.name for p in setup_check_paths.PATHS]
    assert len(names) == len(set(names))


def test_t709_paths_exist_and_drops_have_no_comma():
    by_name = {p.name: p for p in setup_check_paths.PATHS}
    for name in ("stop-while-preparing", "drop-disc", "drop-not-disc", "choose-game-folder"):
        assert name in by_name, name
    assert "now:" in by_name["stop-while-preparing"].script
    for p in setup_check_paths.PATHS:
        for key in p.script.split(","):
            if key.startswith(("drop:", "now:drop:")):
                assert key.count(":") >= 1 and "," not in key.split("drop:", 1)[1]


def test_game_folder_home_holds_only_a_system_cnf(tmp_path):
    iso = tmp_path / "x.iso"
    iso.write_bytes(b"")
    (tmp_path / "b").mkdir()
    home = setup_check.make_home(tmp_path / "b", "gamefolder", iso)
    docs = sorted(p.name for p in (home / "Documents").iterdir())
    assert docs == ["g-game"]
    assert [p.name for p in (home / "Documents" / "g-game").iterdir()] == ["SYSTEM.CNF"]


def test_t713_sanitizer_is_the_default(monkeypatch):
    monkeypatch.delenv("DON_CHECK_SANITIZE", raising=False)
    monkeypatch.delenv("DON_CHECK_BUILD_DIR", raising=False)
    assert setup_check_build.sanitize() == "address,undefined"
    assert setup_check_build.build_dir().name == "check-asan"
    env = setup_check_build.sanitizer_env()
    assert "halt_on_error=1" in env["ASAN_OPTIONS"] and "abort_on_error=0" in env["ASAN_OPTIONS"]
    assert "print_stacktrace=1" in env["UBSAN_OPTIONS"] and "halt_on_error=1" in env["UBSAN_OPTIONS"]


def test_t713_plain_binary_when_the_list_is_empty(monkeypatch):
    monkeypatch.setenv("DON_CHECK_SANITIZE", "")
    assert setup_check_build.sanitizer_env() == {}


def test_t713_large_listing_path_confirms_a_folder_row(tmp_path):
    p = {q.name: q for q in setup_check_paths.PATHS}["browse-5000-files-enter-folder"]
    assert p.home == "folders" and p.script.count("confirm") == 5   # browse, Documents, e-many, then a folder row twice
    iso = tmp_path / "x.iso"
    iso.write_bytes(b"")
    (tmp_path / "b").mkdir()
    home = setup_check.make_home(tmp_path / "b", "folders", iso)
    many = home / "Documents" / "e-many"
    assert (many / "a-sub").is_dir() and len(list(many.iterdir())) == 5001
    setup_check.fix_modes(tmp_path)
