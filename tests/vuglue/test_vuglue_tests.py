"""Runs every test executable of tests/vuglue/CMakeLists.txt found in the build folder.

The folder is $DON_VUGLUE_TESTS_DIR (default build/vuglue-tests, relative to the repository root).
Build it with:

    cmake -S tests/vuglue -B build/vuglue-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
    cmake --build build/vuglue-tests -j2

Each executable exits with 0 if all its checks passed and prints one line per failed check on stderr.
test_vu_glue needs the real VU1 library (build/vu/libdon_vu.so), which it loads at run time through
src/host/vu_glue.cpp; it is skipped, with the reason, when the library is not there (run
bash tools/pipeline/build_vu.sh). The executables built in the container link SDL3 from there: when the
host cannot run one, it is run inside the container (as tests/audio and tests/vu do).

test_vu_glue is run a second time with DON_VU_LOG=1: that is the only mode in which the glue puts a
counting trampoline in front of the runtime's XGKICK sink, and the summary line of the run is checked
in its stderr output.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = Path(os.environ.get("DON_VUGLUE_TESTS_DIR", "build/vuglue-tests"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
SUFFIX = ".exe" if sys.platform == "win32" else ""
CONTAINER_RUN = ROOT / "tools" / "container" / "run.sh"
LIBRARY_DIR = ROOT / "build" / "vu"
LIBRARY_NAMES = ("libdon_vu.so", "libdon_vu.dylib", "don_vu.dll")


def vu_library():
    """The VU1 library of build/vu, or None."""
    for name in LIBRARY_NAMES:
        candidate = LIBRARY_DIR / name
        if candidate.is_file():
            return candidate
    return None


def find_tests():
    """Test executables: files named test_* directly in the build folder."""
    if not BUILD_DIR.is_dir():
        return []
    return sorted(
        path for path in BUILD_DIR.iterdir()
        if path.is_file() and path.name.startswith("test_") and path.name.endswith(SUFFIX)
        and os.access(path, os.X_OK) and path.suffix in ("", ".exe")
    )


TESTS = find_tests()
BUILD_HINT = (
    f"no test_* executable in {BUILD_DIR}; build it: "
    f"cmake -S tests/vuglue -B {BUILD_DIR} -G Ninja -DCMAKE_CXX_COMPILER=clang++ "
    f"&& cmake --build {BUILD_DIR} -j2 (or set DON_VUGLUE_TESTS_DIR)"
)
LIBRARY_HINT = "build/vu has no libdon_vu library; run: bash tools/pipeline/build_vu.sh"


def run(*command, env=None):
    """Runs the command on the host; if the executable cannot load there, inside the build container."""
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=600,
                            env=full_env)
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        prefix = [f"{name}={value}" for name, value in (env or {}).items()]
        result = subprocess.run(
            ["bash", str(CONTAINER_RUN), "env", *prefix, *[str(part) for part in command]],
            capture_output=True, text=True, timeout=900,
        )
    return result


@pytest.mark.skipif(not TESTS, reason=BUILD_HINT)
@pytest.mark.parametrize("executable", TESTS or [None], ids=lambda p: p.name if p else "none")
def test_vuglue_executable(executable):
    args = []
    if executable.name.startswith("test_vu_glue"):
        library = vu_library()
        if library is None:
            pytest.skip(LIBRARY_HINT)
        args = ["--vu-dir", str(library.parent)]

    result = run(executable, *args)
    assert result.returncode == 0, f"{executable.name} exit={result.returncode}\n{result.stdout}\n{result.stderr}"
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    assert lines and lines[-1].endswith(": all checks passed"), result.stdout
    assert "CHECK failed" not in result.stderr and "CHECK_EQ failed" not in result.stderr, result.stderr


@pytest.mark.skipif(not TESTS, reason=BUILD_HINT)
def test_vu_glue_log_summary():
    """DON_VU_LOG=1: the counting sink and the one summary line at thread_end."""
    executable = BUILD_DIR / ("test_vu_glue" + SUFFIX)
    if not executable.is_file():
        pytest.skip(BUILD_HINT)
    library = vu_library()
    if library is None:
        pytest.skip(LIBRARY_HINT)

    result = run(executable, "--vu-dir", str(library.parent), env={"DON_VU_LOG": "1"})
    assert result.returncode == 0, f"exit={result.returncode}\n{result.stdout}\n{result.stderr}"

    summaries = [line for line in result.stderr.splitlines() if "VU1 (" in line]
    assert len(summaries) == 2, result.stderr  # one per engine
    for engine in ("microvu", "pcsx2interp"):
        matching = [line for line in summaries if f"VU1 ({engine})" in line]
        assert len(matching) == 1, result.stderr
        match = re.search(
            r"\[don-vu\] VU1 \((\w+)\): (\d+) programs started, (\d+) continuations, (\d+) cycles, "
            r"(\d+) XGKICK packets, (\d+) bytes$",
            matching[0],
        )
        assert match, matching[0]
        started, continuations, cycles, packets, byte_count = (int(match.group(i)) for i in range(2, 7))
        assert started >= 6, matching[0]  # the test runs 6 programs with a start
        assert continuations >= 5, matching[0]  # the sliced run and the 200-pass loop
        assert cycles > 0, matching[0]
        assert packets >= 3 and byte_count == 32 * packets, matching[0]  # two packets of 32 bytes + one
