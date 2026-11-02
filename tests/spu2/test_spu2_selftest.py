"""Runs build/spu2/spu2_selftest (libdon_spu2 through its C API, synthetic ADPCM sample, no game data).

The program prints one OK/FAIL line per check and exits with 0 only if all pass. It runs on the
host if the library loads there, otherwise inside the build container. Skipped when
`bash tools/pipeline/build_spu2.sh` has not been run.
"""
import subprocess
import struct
import wave
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SELFTEST = ROOT / "build" / "spu2" / "spu2_selftest"
OUT_DIR = ROOT / "work" / "spu2_tests"  # inside the repository so that the container sees it too

pytestmark = pytest.mark.skipif(
    not SELFTEST.is_file(),
    reason="build/spu2/spu2_selftest does not exist; run: bash tools/pipeline/build_spu2.sh",
)


def run_selftest(*args):
    """Runs the self-test on the host; if the host cannot load it, inside the build container."""
    cmd = [str(SELFTEST), *args]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        cmd = ["bash", str(ROOT / "tools" / "container" / "run.sh"), *cmd]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return result


def test_selftest_passes():
    result = run_selftest()
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert not [line for line in lines if line.startswith("FAIL")], result.stdout
    assert sum(line.startswith("OK") for line in lines) >= 20
    assert lines[-1].startswith("PASS"), result.stdout


def test_selftest_writes_a_wav():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    wav_path = OUT_DIR / "tone.wav"
    wav_path.unlink(missing_ok=True)
    result = run_selftest("--wav", str(wav_path))
    assert result.returncode == 0, result.stdout + result.stderr
    with wave.open(str(wav_path), "rb") as w:
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (2, 2, 48000)
        frames = w.getnframes()
        data = w.readframes(frames)
    assert abs(frames - 48000) <= 1
    peak = max(abs(v) for v in struct.unpack("<%dh" % (len(data) // 2), data))
    assert 1600 < peak <= 32767


def test_selftest_rejects_bad_arguments():
    result = run_selftest("--nonsense")
    assert result.returncode == 2
