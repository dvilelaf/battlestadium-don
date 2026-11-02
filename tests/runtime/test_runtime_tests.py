"""Runs every test executable of tests/runtime/CMakeLists.txt found in the build folder.

The folder is $DON_RT_TESTS_DIR (default build/rt-tests, relative to the repository root). Build it with:

    cmake -S tests/runtime -B build/rt-tests -DPS2RECOMP_DIR=third_party/PS2Recomp
    cmake --build build/rt-tests -j6

Each executable exits with 0 if all its checks passed and prints one line per failed check on stderr.
Skipped, with the reason, when the folder or the executables do not exist.
"""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = Path(os.environ.get("DON_RT_TESTS_DIR", "build/rt-tests"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
SUFFIX = ".exe" if sys.platform == "win32" else ""


def find_tests():
    """Test executables: files named *_test directly in the build folder."""
    if not BUILD_DIR.is_dir():
        return []
    return sorted(
        path for path in BUILD_DIR.iterdir()
        if path.is_file() and path.name.endswith("_test" + SUFFIX) and os.access(path, os.X_OK)
    )


TESTS = find_tests()


@pytest.mark.skipif(
    not TESTS,
    reason=f"no *_test executable in {BUILD_DIR}; build it: "
           f"cmake -S tests/runtime -B {BUILD_DIR} && cmake --build {BUILD_DIR} -j6 "
           "(or set DON_RT_TESTS_DIR)",
)
@pytest.mark.parametrize("executable", TESTS or [None], ids=lambda p: p.name if p else "none")
def test_runtime_executable(executable):
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, f"{executable.name} exit={result.returncode}\n{result.stdout}\n{result.stderr}"
