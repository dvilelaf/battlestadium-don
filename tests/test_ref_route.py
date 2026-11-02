"""Tests of tools/ref/drive.py route parsing with synthetic routes (no emulator needed)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "ref"))

import drive  # noqa: E402

ROUTE_FILE = Path(__file__).resolve().parent.parent / "tools" / "ref" / "route.txt"


def test_parses_every_command_and_ignores_comments():
    steps = drive.parse_route(
        "# comment\n"
        "\n"
        "press circle   # trailing comment\n"
        "press start 250\n"
        "wait 1.5\n"
        "wait_screen title\n"
        "wait_screen menu 30\n"
        "capture 01-title\n"
    )
    assert [s.cmd for s in steps] == ["press", "press", "wait", "wait_screen", "wait_screen", "capture"]
    assert steps[0].args == ("circle", drive.DEFAULT_HOLD_MS)
    assert steps[1].args == ("start", 250)
    assert steps[2].args == (1.5,)
    assert steps[3].args == ("title", drive.DEFAULT_WAIT_SCREEN_S)
    assert steps[4].args == ("menu", 30.0)
    assert steps[5].args == ("01-title", True)
    assert steps[0].line == 3


def test_capture_check_can_be_turned_off_for_one_capture():
    steps = drive.parse_route("capture 01-title\ncapture 02-menu nocheck\n")
    assert [s.args for s in steps] == [("01-title", True), ("02-menu", False)]
    with pytest.raises(drive.RouteError):
        drive.parse_route("capture 02-menu maybe")


def test_buttons_are_case_insensitive_and_all_have_keys():
    assert drive.button_key("CIRCLE") == drive.button_key("circle")
    for name in ("up", "down", "left", "right", "cross", "circle", "square", "triangle",
                 "start", "select", "l1", "r1", "l2", "r2"):
        assert drive.button_key(name)
    assert len(set(drive.BUTTON_KEYS.values())) == len(drive.BUTTON_KEYS)


@pytest.mark.parametrize(
    "text, fragment",
    [
        ("press", "line 1"),
        ("press banana", "unknown button"),
        ("press circle 0", "HOLD_MS"),
        ("wait", "usage"),
        ("wait abc", "number"),
        ("wait -1", "at least"),
        ("wait_screen", "usage"),
        ("wait_screen title 0", "at least"),
        ("capture", "usage"),
        ("capture 01-title nocheck extra", "usage"),
        ("capture 07-extra", "unknown capture"),
        ("jump 3", "unknown command"),
    ],
)
def test_rejects_mistakes_with_the_line_number(text, fragment):
    with pytest.raises(drive.RouteError) as e:
        drive.parse_route("# first line\n" + text if "line 1" not in fragment else text)
    assert fragment in str(e.value)
    assert "line " in str(e.value)


def test_box_parsing():
    assert drive.parse_box("0,0,0.2,1") == (0.0, 0.0, 0.2, 1.0)
    for bad in ("1,2,3", "a,b,c,d", "0.5,0,0.2,1", "0,0,1.5,1"):
        with pytest.raises(drive.RouteError):
            drive.parse_box(bad)


def test_the_route_file_covers_the_six_captures_in_order():
    steps = drive.parse_route(ROUTE_FILE.read_text())
    captured = [s.args[0] for s in steps if s.cmd == "capture"]
    assert captured == list(drive.CAPTURE_NAMES)
    # Only PS2 buttons: parsing already checked every press.
    assert all(s.cmd in {"press", "wait", "wait_screen", "capture"} for s in steps)


def _fake_capture(monkeypatch, tmp_path, verdicts):
    """Runs drive.capture with the emulator replaced by fakes; returns how many dumps were taken."""
    taken = []

    def take_dump(paths):
        gs = tmp_path / f"d{len(taken)}.gs.zst"
        png = tmp_path / f"d{len(taken)}.png"
        gs.write_bytes(b"x")
        png.write_bytes(b"x")
        taken.append(gs)
        return gs, png

    monkeypatch.setattr(drive, "take_dump", take_dump)
    monkeypatch.setattr(drive, "replay_check", lambda paths, gs: verdicts[len(taken) - 1])
    monkeypatch.setattr(drive, "hotkey", lambda paths, combo: None)
    monkeypatch.setattr(drive, "_wait_new", lambda *a, **k: tmp_path / "state.p2s")
    monkeypatch.setattr(drive, "name_capture", lambda paths, name, png, p2s, gs: None)
    monkeypatch.setattr(drive.time, "sleep", lambda s: None)
    monkeypatch.setattr(drive, "RETAKE_DELAYS", (0.1, 0.1, 0.1))
    paths = drive.Paths(tmp_path / "raw", tmp_path / "out")
    return taken, paths


def test_capture_retakes_a_poor_dump_and_keeps_the_first_good_one(monkeypatch, tmp_path):
    taken, paths = _fake_capture(monkeypatch, tmp_path, [(False, "poor"), (False, "poor"), (True, "ok")])
    drive.capture(paths, "06-results")
    assert len(taken) == 3
    assert not taken[0].exists() and not taken[1].exists() and taken[2].exists()


def test_capture_gives_up_after_the_last_retake(monkeypatch, tmp_path):
    taken, paths = _fake_capture(monkeypatch, tmp_path, [(False, "poor")] * 4)
    with pytest.raises(SystemExit):
        drive.capture(paths, "06-results")
    assert len(taken) == 4


def test_capture_skips_the_check_when_it_cannot_run_or_is_off(monkeypatch, tmp_path):
    taken, paths = _fake_capture(monkeypatch, tmp_path, [(None, "no gsreplay")])
    drive.capture(paths, "01-title")
    assert len(taken) == 1
    taken, paths = _fake_capture(monkeypatch, tmp_path, [(False, "poor")])
    drive.capture(paths, "01-title", check=False)
    assert len(taken) == 1
