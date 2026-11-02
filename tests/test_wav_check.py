"""Tests of tools/verify/wav_check.py with synthetic WAVs written in a temp folder.

Each WAV is one second per window at 48 kHz, three seconds long, with stereo noise (+-3000):
R delayed by exactly 3 samples, one click in the second window, a 20 ms run of zeros in the
second window, and a clean aligned one. The command line is run through main() with --json so
that the figures are checked the way a person reads them.
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import wav_check  # noqa: E402

RATE = 48000
SECONDS = 3


def write_wav(path, left, right=None):
    """Writes int16 samples; a right of None writes a mono file."""
    with wave.open(str(path), "wb") as w:
        if right is None:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(np.asarray(left, dtype="<i2").tobytes())
            return
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.stack([left, right], axis=1).astype("<i2").tobytes())


def noise(seed, count):
    return np.random.default_rng(seed).integers(-3000, 3001, count, dtype=np.int64)


def run_json(path, capsys):
    assert wav_check.main(["--json", str(path)]) == 0
    return json.loads(capsys.readouterr().out)


def test_delayed_right_channel_gives_lag_3_every_second(tmp_path, capsys):
    count = RATE * SECONDS
    left = noise(1, count)
    right = np.concatenate([noise(2, 3), left[:-3]])
    path = tmp_path / "delayed.wav"
    write_wav(path, left, right)

    result = run_json(path, capsys)

    assert [w["lag_samples"] for w in result["windows"]] == [3, 3, 3]
    assert [w["lag_ms"] for w in result["windows"]] == [pytest.approx(3000 / RATE, abs=1e-3)] * 3
    assert all(w["peak_corr"] > 0.9 for w in result["windows"])
    assert result["summary"]["clicks"] == 0
    assert result["summary"]["dropouts"] == 0


def test_single_inserted_click_is_counted_once_in_its_second(tmp_path, capsys):
    count = RATE * SECONDS
    left = noise(3, count)
    right = left.copy()
    left[RATE + RATE // 2] = 32000  # a spike in the middle of the second window
    path = tmp_path / "click.wav"
    write_wav(path, left, right)

    result = run_json(path, capsys)

    assert [w["clicks"] for w in result["windows"]] == [0, 1, 0]
    assert result["summary"]["clicks"] == 1
    assert all(w["lag_samples"] == 0 for w in result["windows"])


def test_20ms_run_of_zeros_is_one_dropout_in_its_second(tmp_path, capsys):
    count = RATE * SECONDS
    left = noise(4, count)
    right = left.copy()
    start = RATE + RATE // 4
    zeros = 20 * RATE // 1000  # 960 frames
    left[start : start + zeros] = 0
    right[start : start + zeros] = 0
    path = tmp_path / "dropout.wav"
    write_wav(path, left, right)

    result = run_json(path, capsys)

    assert [w["dropouts"] for w in result["windows"]] == [0, 1, 0]
    assert result["summary"]["dropouts"] == 1
    assert result["summary"]["clicks"] == 0


def test_clean_aligned_signal_has_no_lag_clicks_or_dropouts(tmp_path, capsys):
    count = RATE * SECONDS
    left = noise(5, count)
    path = tmp_path / "clean.wav"
    write_wav(path, left, left.copy())

    result = run_json(path, capsys)

    assert [w["lag_samples"] for w in result["windows"]] == [0, 0, 0]
    assert all(w["peak_corr"] > 0.99 for w in result["windows"])
    assert result["summary"]["clicks"] == 0
    assert result["summary"]["dropouts"] == 0


def test_mono_input_is_reported_and_not_measured(tmp_path, capsys):
    path = tmp_path / "mono.wav"
    write_wav(path, noise(6, RATE))

    assert wav_check.main([str(path)]) == 2
    assert "mono" in capsys.readouterr().err


def test_echo_120ms_at_half_amplitude_is_found_in_every_window_and_channel(tmp_path, capsys):
    count = RATE * SECONDS
    lag = 120 * RATE // 1000  # 5760 frames
    left = noise(8, count) + 0.5 * np.concatenate([np.zeros(lag, dtype=np.int64), noise(8, count)[:-lag]])
    right = noise(9, count) + 0.5 * np.concatenate([np.zeros(lag, dtype=np.int64), noise(9, count)[:-lag]])
    path = tmp_path / "echo.wav"
    write_wav(path, left.astype(np.int64), right.astype(np.int64))

    assert wav_check.main(["--json", "--echo", str(path)]) == 0
    echo = json.loads(capsys.readouterr().out)["echo"]

    rows = echo["windows"]
    assert [(r["second"], r["channel"]) for r in rows] == [(s, c) for s in range(SECONDS) for c in ("L", "R")]
    assert all(abs(r["lag_ms"] - 120.0) < 1.0 for r in rows)
    # Half amplitude gives about 0.35 here: 0.5 * (N - lag) / (1.25 * N) with N = RATE.
    assert all(0.3 < r["peak"] < 0.45 for r in rows)
    assert echo["summary"]["most_common_lag_ms"] == 120
    assert echo["summary"]["windows_above"] == 2 * SECONDS


def test_echo_finds_no_peak_above_0_3_in_clean_noise(tmp_path, capsys):
    count = RATE * SECONDS
    left = noise(10, count)
    right = noise(11, count)
    path = tmp_path / "clean_echo.wav"
    write_wav(path, left, right)

    assert wav_check.main(["--json", "--echo", str(path)]) == 0
    echo = json.loads(capsys.readouterr().out)["echo"]

    assert all(r["peak"] <= 0.3 for r in echo["windows"])
    assert echo["summary"]["most_common_lag_ms"] is None
    assert echo["summary"]["windows_above"] == 0


def test_echo_is_absent_without_the_flag_and_printed_as_a_table_with_it(tmp_path, capsys):
    path = tmp_path / "echo_table.wav"
    left = noise(12, RATE * SECONDS)
    write_wav(path, left, left.copy())

    assert wav_check.main(["--json", str(path)]) == 0
    assert "echo" not in json.loads(capsys.readouterr().out)

    assert wav_check.main(["--echo", str(path)]) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert lines[SECONDS + 2].startswith("echo: lags 10..500 ms")
    assert lines[SECONDS + 3].startswith("second  channel")
    assert lines[-1].startswith("echo summary:")


def test_table_output_has_one_row_per_second_and_a_summary(tmp_path, capsys):
    path = tmp_path / "clean_table.wav"
    left = noise(7, RATE * SECONDS)
    write_wav(path, left, left.copy())

    assert wav_check.main([str(path)]) == 0
    lines = capsys.readouterr().out.strip().splitlines()

    assert lines[0].startswith("second")
    assert len(lines) == 1 + SECONDS + 1
    assert lines[-1].startswith("summary: 3 windows")
