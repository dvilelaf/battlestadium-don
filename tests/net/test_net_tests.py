"""Runs every test executable of tests/net/CMakeLists.txt found in the build folder.

The folder is $DON_NET_TESTS_DIR (default build/net-tests, relative to the repository root). Build it with:

    cmake -S tests/net -B build/net-tests -DDON_HOST_PAD_INCLUDE_DIR=<folder that holds runtime/ps2_host_pad.h>
    cmake --build build/net-tests -j2

Each executable exits with 0 if all its checks passed and prints one line per failed check on stderr.
Every test_net_* program is built from tests/net/test_net_*.cpp; the pieces of D14 add theirs.
Skipped, with the reason, when the folder or the executables do not exist.
"""
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = Path(os.environ.get("DON_NET_TESTS_DIR", "build/net-tests"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
SUFFIX = ".exe" if sys.platform == "win32" else ""


def find_tests():
    """Test executables: files named test_net_* directly in the build folder."""
    if not BUILD_DIR.is_dir():
        return []
    return sorted(
        path for path in BUILD_DIR.iterdir()
        if path.is_file() and path.name.startswith("test_net_") and path.name.endswith(SUFFIX)
        and os.access(path, os.X_OK) and path.suffix in ("", ".exe")
    )


TESTS = find_tests()


@pytest.mark.skipif(
    not TESTS,
    reason=f"no test_net_* executable in {BUILD_DIR}; build it: "
           f"cmake -S tests/net -B {BUILD_DIR} -DDON_HOST_PAD_INCLUDE_DIR=<dir with runtime/ps2_host_pad.h> "
           f"&& cmake --build {BUILD_DIR} -j2 (or set DON_NET_TESTS_DIR)",
)
@pytest.mark.parametrize("executable", TESTS or [None], ids=lambda p: p.name if p else "none")
def test_net_executable(executable):
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, f"{executable.name} exit={result.returncode}\n{result.stdout}\n{result.stderr}"
