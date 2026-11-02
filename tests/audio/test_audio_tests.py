"""Runs every test executable of tests/audio/CMakeLists.txt found in the build folder.

The folder is $DON_AUDIO_TESTS_DIR (default build/audio-tests, relative to the repository root). Build it
with:

    cmake -S tests/audio -B build/audio-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DDON_AUDIO_SDL=ON
    cmake --build build/audio-tests -j2

Each executable exits with 0 if all its checks passed and prints one line per failed check on stderr.
test_spu2_glue needs the real SPU2 library (build/spu2/libdon_spu2.so); it is skipped, with the reason,
when the library is not there. The executables built in the container link SDL3 from there: when the host
cannot run one, it is run inside the container (the same thing tests/spu2/test_spu2_selftest.py does).

test_spu2_glue also writes a WAV; tools/verify/wav_stats.py is run on it here and its figures are checked.
Skipped, with the reason, when the build folder or the executables do not exist.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = Path(os.environ.get("DON_AUDIO_TESTS_DIR", "build/audio-tests"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
SUFFIX = ".exe" if sys.platform == "win32" else ""
WAV_STATS = ROOT / "tools" / "verify" / "wav_stats.py"
WAV_DIR = ROOT / "work" / "audio_tests"  # inside the repository so the container sees it too
CONTAINER_RUN = ROOT / "tools" / "container" / "run.sh"


def spu2_library():
    """The SPU2 library of build/spu2, or None."""
    folder = ROOT / "build" / "spu2"
    for name in ("libdon_spu2.so", "libdon_spu2.dylib", "don_spu2.dll"):
        candidate = folder / name
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
    f"cmake -S tests/audio -B {BUILD_DIR} -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DDON_AUDIO_SDL=ON "
    f"&& cmake --build {BUILD_DIR} -j2 (or set DON_AUDIO_TESTS_DIR)"
)


def run(*command):
    """Runs the command on the host; if the executable cannot load there, inside the build container."""
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=600)
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        result = subprocess.run(
            ["bash", str(CONTAINER_RUN), *[str(part) for part in command]],
            capture_output=True, text=True, timeout=900,
        )
    return result


def figure(text, name):
    match = re.search(rf"^{name}: ([0-9.]+)", text, re.MULTILINE)
    assert match, f"no {name} line in:\n{text}"
    return float(match.group(1))


@pytest.mark.skipif(not TESTS, reason=BUILD_HINT)
@pytest.mark.parametrize("executable", TESTS or [None], ids=lambda p: p.name if p else "none")
def test_audio_executable(executable):
    args = []
    wav = None
    if executable.name.startswith("test_spu2_glue"):
        library = spu2_library()
        if library is None:
            pytest.skip("build/spu2 has no libdon_spu2; run: bash tools/pipeline/build_spu2.sh")
        WAV_DIR.mkdir(parents=True, exist_ok=True)
        wav = WAV_DIR / "spu2_glue.wav"
        wav.unlink(missing_ok=True)
        args = ["--spu2-dir", str(library.parent), "--wav", str(wav)]

    result = run(executable, *args)
    assert result.returncode == 0, f"{executable.name} exit={result.returncode}\n{result.stdout}\n{result.stderr}"

    if wav is not None:
        assert wav.is_file(), f"{executable.name} did not write {wav}\n{result.stdout}"
        stats = run(sys.executable, "-I", WAV_STATS, wav, "--max-silence", "0.5", "--max-clip", "0.001")
        assert stats.returncode == 0, f"wav_stats {wav}\n{stats.stdout}\n{stats.stderr}"
        assert figure(stats.stdout, "duration") == pytest.approx(1.0, abs=0.05)
        assert figure(stats.stdout, "rms") > 0.01
        assert figure(stats.stdout, "silent") < 0.5
        assert figure(stats.stdout, "saturated") == 0.0
