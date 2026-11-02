"""Tests of tools/verify/wav_copies.py with synthetic WAVs written in a temp folder.

The reference is six seconds of stereo noise at 48 kHz. Three game recordings are built from it:
one copy at 2.0 s; the copy plus 0.6 times the copy delayed by 40 ms; and the copy with the right
channel delayed by 3 samples. The search is run on an excerpt at 1.0 s, which is found at 3.0 s
in the first game, at 1.0 s in the other two.
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import wav_copies  # noqa: E402

RATE = 48000
REF_SECONDS = 6
REF_START = 1.0


def write_wav(path, stereo):
    """Writes int16 samples of shape (frames, 2)."""
    data = np.clip(np.round(stereo), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())


@pytest.fixture
def reference():
    rng = np.random.default_rng(1)
    return rng.uniform(-3000, 3000, size=(REF_SECONDS * RATE, 2))


@pytest.fixture
def ref_file(tmp_path, reference):
    path = tmp_path / "ref.wav"
    write_wav(path, reference)
    return path


def run(ref_file, game_file, *extra):
    """Runs the command line with --json and returns the parsed result."""
    import contextlib
    import io

    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = wav_copies.main([str(ref_file), str(game_file), "--ref-start", str(REF_START),
                                "--json", *extra])
    assert code == 0
    return json.loads(out.getvalue())


def test_one_copy_gives_one_strong_peak(tmp_path, reference, ref_file):
    silence = np.zeros((2 * RATE, 2))
    game_file = tmp_path / "one.wav"
    write_wav(game_file, np.concatenate([silence, reference]))

    result = run(ref_file, game_file)["main"]
    first, second = result["peaks"][:2]
    assert round(first["game_s"] * RATE) == 3 * RATE
    assert first["height"] > 0.99
    assert second["height"] < 0.3 * first["height"]


def test_copy_with_delayed_echo_gives_two_peaks_40_ms_apart(tmp_path, reference, ref_file):
    delay = round(0.04 * RATE)
    game = np.zeros((REF_SECONDS * RATE + delay, 2))
    game[: len(reference)] += reference
    game[delay : delay + len(reference)] += 0.6 * reference
    game_file = tmp_path / "echo.wav"
    write_wav(game_file, game)

    result = run(ref_file, game_file)["main"]
    assert result["distance_ms"] == pytest.approx(40.0, abs=0.1)
    assert result["ratio"] == pytest.approx(0.6, abs=0.05)
    assert round(result["peaks"][0]["game_s"] * RATE) == REF_START * RATE


def test_echo_gives_the_same_distance_on_every_excerpt(tmp_path, reference, ref_file):
    delay = round(0.04 * RATE)
    game = np.zeros((REF_SECONDS * RATE + delay, 2))
    game[: len(reference)] += reference
    game[delay : delay + len(reference)] += 0.6 * reference
    game_file = tmp_path / "echo.wav"
    write_wav(game_file, game)

    excerpts = run(ref_file, game_file, "--excerpts", "5")["excerpts"]
    assert len(excerpts) == 5
    for item in excerpts:
        assert item["distance_ms"] == pytest.approx(40.0, abs=0.1)


def test_three_sample_channel_offset_still_finds_the_copy(tmp_path, reference, ref_file):
    shifted = reference.copy()
    shifted[3:, 1] = reference[:-3, 1]
    shifted[:3, 1] = 0
    game_file = tmp_path / "offset.wav"
    write_wav(game_file, shifted)

    result = run(ref_file, game_file)["main"]
    assert round(result["peaks"][0]["game_s"] * RATE) == REF_START * RATE
    assert result["peaks"][0]["height"] > 0.4
