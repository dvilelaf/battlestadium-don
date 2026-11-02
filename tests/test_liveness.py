"""Tests of tools/verify/liveness.py with synthetic logs (they do not need the game)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import liveness

FOREVER = 10**9


def trace(vsync_to, pauses=()):
    """A log with a vsync line every 60 vsyncs up to `vsync_to`, and a frame line at each vsync
    where the frame loop runs. The frame loop does not run for the vsyncs in (start, end] of each
    pair in `pauses`."""
    lines = []
    frame = 0
    for v in range(60, vsync_to + 1, 60):
        lines.append(f"[don] vsync {v}")
        if not any(start < v <= end for start, end in pauses):
            frame += 60
            lines.append(f"[don] frame {frame}")
    return "\n".join(lines) + "\n"


def write(tmp_path, text, name="run.log"):
    path = tmp_path / name
    path.write_text(text)
    return path


def test_healthy_run_passes(tmp_path, capsys):
    log = write(tmp_path, trace(3000))
    assert liveness.main([str(log)]) == 0
    out = capsys.readouterr().out
    assert out.startswith("[liveness]")
    assert "frame=3000 vsync=3000" in out
    assert "largest gap 60 vsyncs" in out and ": ok" in out


def test_stopped_run_fails_and_names_the_stop(tmp_path, capsys):
    log = write(tmp_path, trace(3000, pauses=((600, FOREVER),)))
    assert liveness.main([str(log)]) == 1
    out = capsys.readouterr().out.splitlines()
    assert out[0].startswith("[liveness]") and ": FAIL" in out[0]
    assert out[1] == "[liveness] the frame loop did not run from vsync 600 to vsync 3000"


def test_short_pause_in_the_middle_passes(tmp_path):
    # The frame loop stops for vsyncs 660..780 and resumes at 840: a gap of 240 vsyncs, under the default 300.
    result = liveness.analyze(write(tmp_path, trace(3000, pauses=((600, 780),))))
    assert (result.largest_gap, result.gap_from, result.gap_to) == (240, 600, 840)
    assert liveness.main([str(tmp_path / "run.log")]) == 0


def test_long_pause_that_resumes_fails_and_names_the_pause(tmp_path, capsys):
    log = write(tmp_path, trace(3000, pauses=((600, 1800),)))
    result = liveness.analyze(log)
    assert (result.largest_gap, result.gap_from, result.gap_to) == (1260, 600, 1860)
    assert liveness.main([str(log)]) == 1
    out = capsys.readouterr().out
    assert "from vsync 600 to vsync 1860" in out


def test_no_frame_line_exits_2(tmp_path, capsys):
    log = write(tmp_path, "\n".join(f"[don] vsync {v}" for v in range(60, 1201, 60)) + "\n")
    assert liveness.main([str(log)]) == 2
    out = capsys.readouterr().out
    assert out.startswith("[liveness]") and "no frame line" in out


def test_no_vsync_line_exits_2(tmp_path, capsys):
    log = write(tmp_path, "[don] frame 60\n[don] frame 120\n")
    assert liveness.main([str(log)]) == 2
    assert "no vsync line" in capsys.readouterr().out


def test_unreadable_file_exits_2(tmp_path, capsys):
    assert liveness.main([str(tmp_path / "missing.log")]) == 2
    assert capsys.readouterr().out.startswith("[liveness] cannot read")


def test_last_line_cut_in_the_middle_is_not_read(tmp_path):
    # The stopped run ends with "[don] vsync 1" cut from "[don] vsync 1800": read as 1, the tail
    # would be 0 and the stop would pass. It must not be read at all.
    text = trace(3000, pauses=((600, FOREVER),)) + "[don] vsync 1"
    result = liveness.analyze(write(tmp_path, text))
    assert result.last_vsync == 3000
    assert (result.largest_gap, result.gap_from, result.gap_to) == (2400, 600, 3000)
    assert liveness.main([str(tmp_path / "run.log")]) == 1


def test_cut_last_line_of_a_healthy_run_passes(tmp_path):
    text = trace(3000) + "[don] frame 18"
    result = liveness.analyze(write(tmp_path, text))
    assert result.last_frame == 3000 and result.largest_gap == 60


def test_max_gap_option(tmp_path, capsys):
    log = write(tmp_path, trace(3000, pauses=((600, 900),)))
    assert liveness.main([str(log), "--max-gap", "360"]) == 0
    assert liveness.main([str(log), "--max-gap", "359"]) == 1
    assert "limit 359" in capsys.readouterr().out
    assert liveness.main([str(log)]) == 1  # the default is 300: a pause of 360 is over it


def test_lines_run_together_are_still_counted(tmp_path):
    text = "Starting execution[don] vsync 60[don] frame 60\n[don] vsync 120\n"
    result = liveness.analyze(write(tmp_path, text))
    assert (result.last_frame, result.last_vsync, result.advance_vsync) == (60, 120, 60)
