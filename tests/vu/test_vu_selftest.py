"""Runs build/vu/vu_selftest (libdon_vu through its C API, hand-assembled microcode, no game data).

The program prints one OK/FAIL line per check and exits with 0 only if all pass. It runs every case
on microVU and on the PCSX2 interpreter and compares them. It runs on the host if the library
loads there, otherwise inside the build container. Skipped when
`bash tools/pipeline/build_vu.sh` has not been run.
"""
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SELFTEST = ROOT / "build" / "vu" / "vu_selftest"
LIB = ROOT / "build" / "vu" / "libdon_vu.so"

pytestmark = pytest.mark.skipif(
    not SELFTEST.is_file(),
    reason="build/vu/vu_selftest does not exist; run: bash tools/pipeline/build_vu.sh",
)


def run(cmd, timeout=300):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def run_selftest():
    result = run([str(SELFTEST)])
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        result = run(["bash", str(ROOT / "tools" / "container" / "run.sh"), str(SELFTEST)], 600)
    return result


def test_selftest_passes():
    result = run_selftest()
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert not [line for line in lines if line.startswith("FAIL")], result.stdout
    assert sum(line.startswith("OK") for line in lines) >= 50
    assert lines[-1].startswith("PASS"), result.stdout


def test_both_engines_and_comparison_ran():
    out = run_selftest().stdout
    assert "[microvu]" in out and "[interp ]" in out
    assert out.count("OK   compare ") >= 5


def test_only_don_vu_symbols_are_exported():
    result = run(["nm", "-D", "--defined-only", str(LIB)])
    if result.returncode != 0:
        pytest.skip("nm is not available")
    names = [line.split()[-1] for line in result.stdout.splitlines() if line.strip()]
    assert names and all(n.startswith("don_vu") for n in names), names
