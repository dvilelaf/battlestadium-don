"""Tests of tools/verify/wav_stats.py (T-303 step 3) with synthetic WAVs written here.

The WAVs are a tone (RMS about 0.35, no silence, no clipping), silence (all samples silent) and a
clipped square wave (nearly every sample saturated).  The command line is run as a subprocess so
that its exit code is checked the way T-304 uses it.
"""
import math
import struct
import subprocess
import sys
import wave
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import wav_stats  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "verify" / "wav_stats.py"
RATE = 48000


def write_wav(path, left, right=None, rate=RATE, channels=2, sampwidth=2):
    """Writes interleaved stereo (or mono) 16-bit samples; `left`/`right` hold -1..1 floats."""
    if right is None:
        right = left
    rows = (left, right)[:channels]
    samples = []
    for i in range(len(left)):
        for channel in rows:
            samples.append(round(channel[i] * 32767))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(sampwidth)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    return path


def tone(seconds, frequency=440.0, amplitude=0.5):
    n = int(seconds * RATE)
    return [amplitude * math.sin(2.0 * math.pi * frequency * i / RATE) for i in range(n)]


def run_cli(*args):
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), *[str(a) for a in args]],
        capture_output=True,
        text=True,
        timeout=120,
    )


def figure(stdout, name):
    for line in stdout.splitlines():
        if line.startswith(name + ": "):
            return float(line.split()[1])
    raise AssertionError(f"no {name} line in:\n{stdout}")


def test_tone_figures(tmp_path):
    path = write_wav(tmp_path / "tone.wav", tone(1.0))
    stats = wav_stats.analyze(path)
    assert stats.frames == RATE
    assert stats.channels == 2
    assert stats.bits == 16
    assert stats.duration == pytest.approx(1.0, abs=1e-3)
    # A sine of amplitude 0.5 has RMS 0.5 / sqrt(2) = 0.3536.
    assert stats.rms == pytest.approx(0.3536, abs=0.01)
    # Only the samples right at the zero crossings round to zero: a tone is not silent.
    assert stats.silent < 0.01
    assert stats.saturated == 0.0


def test_silence_figures(tmp_path):
    path = write_wav(tmp_path / "silence.wav", [0.0] * RATE)
    stats = wav_stats.analyze(path)
    assert stats.rms == 0.0
    assert stats.silent == 1.0
    assert stats.saturated == 0.0


def test_clipped_figures(tmp_path):
    # A full-scale square wave: every sample saturates.
    square = [1.0 if (i // 240) % 2 == 0 else -1.0 for i in range(RATE)]
    path = write_wav(tmp_path / "clipped.wav", square)
    stats = wav_stats.analyze(path)
    assert stats.rms > 0.9
    assert stats.saturated > 0.99
    assert stats.silent == 0.0


def test_cli_reports_and_passes(tmp_path):
    path = write_wav(tmp_path / "tone.wav", tone(0.25))
    result = run_cli(path, "--max-silence", 0.5, "--max-clip", 0.001)
    assert result.returncode == 0, result.stdout + result.stderr
    assert figure(result.stdout, "duration") == pytest.approx(0.25, abs=1e-3)
    assert figure(result.stdout, "rms") == pytest.approx(0.3536, abs=0.01)
    assert figure(result.stdout, "silent") < 0.01
    assert figure(result.stdout, "saturated") == 0.0
    assert "limit max-silence 0.500000: ok" in result.stdout
    assert "limit max-clip 0.001000: ok" in result.stdout


def test_cli_exit_1_on_silence(tmp_path):
    path = write_wav(tmp_path / "silence.wav", [0.0] * RATE)
    result = run_cli(path, "--max-silence", 0.5)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "FAIL: silent fraction 1.000000 exceeds 0.500000" in result.stdout
    assert "limit max-silence 0.500000: exceeded" in result.stdout


def test_cli_exit_1_on_clipping(tmp_path):
    square = [1.0 if (i // 240) % 2 == 0 else -1.0 for i in range(RATE)]
    path = write_wav(tmp_path / "clipped.wav", square)
    result = run_cli(path, "--max-clip", 0.001)
    assert result.returncode == 1, result.stdout + result.stderr
    assert "saturated fraction" in result.stdout


def test_cli_no_limit_always_exits_0(tmp_path):
    path = write_wav(tmp_path / "silence.wav", [0.0] * RATE)
    result = run_cli(path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not [line for line in result.stdout.splitlines() if line.startswith("limit")]


def test_cli_missing_file(tmp_path):
    result = run_cli(tmp_path / "does-not-exist.wav")
    assert result.returncode == 1
    assert result.stderr.startswith("error: ")


def test_truncated_data_is_read_as_far_as_it_goes(tmp_path):
    """A WAV whose size fields claim more than the file holds: the samples that are there count."""
    path = write_wav(tmp_path / "tone.wav", tone(0.5))
    data = path.read_bytes()
    path.write_bytes(data[: 20000])  # half of the header + data
    stats = wav_stats.analyze(path)
    assert 0 < stats.frames < int(0.5 * RATE)
    assert stats.rms > 0.1
