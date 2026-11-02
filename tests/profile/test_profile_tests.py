"""Tests the profiler of src/host against the measuring program of tests/profile/ (T-203).

The program (tests/profile/profile_probe.cpp, built from the CMakeLists next to this file) samples
itself: a worker thread spends about 1.4 s of its processor time in one function and 0.6 s in
another (a 70/30 split), and the main thread sleeps through it. This test runs it twice -- once
with DON_PROFILE unset, once with it set -- and checks:

  * unset: no profile file is written, and the measured work takes no longer than with it set;
  * set: the file exists, the number of samples is within 20 % of the thread's processor time
    times the sampling rate, at least 90 % of the samples are in the two functions, and their
    shares are within 10 percentage points of the 70/30 split the program sized its work to;
  * the sleeping main thread got no sample (every sample is inside the program's own module);
  * tools/verify/profile_summary.py runs on the file and names both functions.

The build folder is $DON_PROFILE_TESTS_DIR (default build/profile-tests); the tests are skipped,
with the reason, when the program is not there:

    cmake -S tests/profile -B build/profile-tests && cmake --build build/profile-tests -j2
"""
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools" / "verify"))

import profile_summary as ps  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_DIR = Path(os.environ.get("DON_PROFILE_TESTS_DIR", "build/profile-tests"))
if not BUILD_DIR.is_absolute():
    BUILD_DIR = ROOT / BUILD_DIR
SUFFIX = ".exe" if sys.platform == "win32" else ""
PROBE = BUILD_DIR / f"profile_probe{SUFFIX}"
SUMMARY = ROOT / "tools" / "verify" / "profile_summary.py"
BUILD_HINT = (
    f"no {PROBE.name} in {BUILD_DIR}; build it: cmake -S tests/profile -B {BUILD_DIR} "
    f"&& cmake --build {BUILD_DIR} -j2 (or set DON_PROFILE_TESTS_DIR)"
)

pytestmark = pytest.mark.skipif(not PROBE.is_file(), reason=BUILD_HINT)

# The probe sizes its two phases to these proportions of time (a_share_target in its output).
A_TARGET = 0.7
B_TARGET = 0.3


def figure(text, name):
    """The float of a `name: <float>` line of the probe's output."""
    match = re.search(rf"^{re.escape(name)}: ?([0-9.]+)", text, re.MULTILINE)
    assert match, f"no {name} line in:\n{text}"
    return float(match.group(1))


def integer(text, name):
    """The integer of a `name: <int>` line of the probe's output."""
    match = re.search(rf"^{re.escape(name)}: ?([0-9]+)", text, re.MULTILINE)
    assert match, f"no {name} line in:\n{text}"
    return int(match.group(1))


def run_probe(profile: Path | None, kind: str, work: tuple[int, int] | None = None,
              cwd: Path | None = None) -> dict:
    """Runs the probe with DON_PROFILE set (to `profile`) or removed, and reads its output."""
    environment = dict(os.environ)
    if profile is None:
        environment.pop("DON_PROFILE", None)
    else:
        environment["DON_PROFILE"] = str(profile)
    arguments = [str(PROBE)]
    if work is not None:
        arguments += [str(work[0]), str(work[1])]
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=300, env=environment, cwd=cwd)
    assert result.returncode == 0, f"profile_probe ({kind}) exit={result.returncode}\n{result.stdout}\n{result.stderr}"
    return {
        "stdout": result.stdout,
        "stderr": result.stderr,
        "work_ms": figure(result.stdout, "work_ms"),
        "cpu_ms": figure(result.stdout, "cpu_ms"),
        "wall_ms": figure(result.stdout, "wall_ms"),
        "a_target": figure(result.stdout, "a_share_target"),
        "work": (integer(result.stdout, "busy_a_iterations"), integer(result.stdout, "busy_b_iterations")),
    }


@pytest.fixture(scope="module")
def runs(tmp_path_factory):
    """Two runs of each mode, without the profiler first (it calibrates the work) and then with it.

    Every later run is given the first run's iteration counts, so all of them do exactly the same
    amount of work and the difference between the modes in `cpu_ms` (and in `work_ms`) is what the
    sampler costs. Two runs per mode (off, on, off, on) and the smaller of each pair remove the
    machine's own drift: on this host the same work varies by several percent between one process
    and the next, which is more than the sampler costs.
    """
    folder_off = tmp_path_factory.mktemp("profile-off")
    first_off = run_probe(None, "off", cwd=folder_off)
    work = first_off["work"]

    folder_on = tmp_path_factory.mktemp("profile-on")
    profile = folder_on / "probe.txt"
    first_on = run_probe(profile, "on", work=work)
    if not profile.is_file():
        if "not available on this platform" in first_on["stderr"]:
            pytest.skip(f"the sampler is not available on this platform: {first_on['stderr'].strip()}")
        pytest.fail(f"no profile was written\n{first_on['stdout']}\n{first_on['stderr']}")

    second_off = run_probe(None, "off", work=work)
    second_on = run_probe(profile, "on", work=work)

    return {
        "profile": profile,
        "work": work,
        "off": {**first_off, "folder": folder_off,
                "work_ms": min(first_off["work_ms"], second_off["work_ms"]),
                "cpu_ms": min(first_off["cpu_ms"], second_off["cpu_ms"])},
        "on": {**first_on, "profile": profile,
               "work_ms": min(first_on["work_ms"], second_on["work_ms"]),
               "cpu_ms": min(first_on["cpu_ms"], second_on["cpu_ms"])},
        "off_runs": [first_off["work_ms"], second_off["work_ms"]],
        "on_runs": [first_on["work_ms"], second_on["work_ms"]],
        "off_cpu_runs": [first_off["cpu_ms"], second_off["cpu_ms"]],
        "on_cpu_runs": [first_on["cpu_ms"], second_on["cpu_ms"]],
    }


def resolved(profile_path: Path):
    """The summary of the profile of the probe, with the symbols read from the probe itself.

    The segment source is the real one: the probe is linked with lld in the container build
    (build/profile-tests-lld), whose code segment has p_vaddr - p_offset = 0x1000, so the file offset a
    sample is recorded at is NOT the virtual address the symbol table uses. Without it this test would
    name whatever lies 4 KiB before each sample (see tools/verify/profile_summary.py).
    """
    profile = ps.read_profile(profile_path)
    summary = ps.analyze(profile, ps.NmSymbolSource(), segments=ps.ElfSegmentSource())
    assert summary.with_symbol == summary.samples, (
        f"nm resolved {summary.with_symbol} of {summary.samples} samples; notes: {summary.notes}"
    )
    return summary


def test_no_profile_file_is_written_when_the_variable_is_unset(runs):
    # The run happened with its working directory inside `folder` and wrote nothing there.
    assert list(runs["off"]["folder"].iterdir()) == []
    assert "profile" not in runs["off"]["stderr"]
    assert runs["off"]["work_ms"] > 1000.0  # the work really happened


def test_the_run_is_not_slower_without_the_profiler(runs):
    # Every run did exactly the same number of iterations, so the difference between the two modes
    # is what the sampler costs. The processor time is the figure that does not move when the
    # machine preempts the thread (this host runs other work while the tests run); the wall time is
    # printed too, for the report. Both values are the smaller of the two runs of each mode.
    print(f"cpu_ms with={runs['on_cpu_runs']} without={runs['off_cpu_runs']} work={runs['work']}")
    print(f"work_ms with={runs['on_runs']} without={runs['off_runs']}")
    assert runs["off"]["cpu_ms"] <= runs["on"]["cpu_ms"] * 1.05 + 30.0


def test_the_profile_has_the_expected_number_of_samples(runs):
    profile = ps.read_profile(runs["on"]["profile"])
    assert profile.thread == "profile-probe"
    assert profile.samples_lost == 0
    # About two seconds of work, sized by a calibration that the machine's load can stretch (the
    # calibration takes the smallest of several trials, so this is a sanity band, not a check of
    # the calibration).
    assert 0.5 < profile.cpu_us / 1e6 < 8.0
    # The sampler uses the thread's processor clock for both figures, so this is a tight one: one
    # sample per millisecond of thread time.
    expected = profile.rate_hz * profile.cpu_us / 1e6
    assert profile.samples == pytest.approx(expected, rel=0.2)


def test_the_samples_are_the_two_functions_in_the_known_proportion(runs):
    summary = resolved(runs["on"]["profile"])
    total = summary.samples
    shares = {}
    for entry in summary.symbols:
        for name in ("busy_a", "busy_b"):
            if f"::{name}(" in entry.name:
                shares[name] = shares.get(name, 0) + entry.samples
    assert set(shares) == {"busy_a", "busy_b"}, [entry.name for entry in summary.symbols[:5]]

    assert sum(shares.values()) >= 0.9 * total
    assert shares["busy_a"] / total == pytest.approx(A_TARGET, abs=0.10)
    assert shares["busy_b"] / total == pytest.approx(B_TARGET, abs=0.10)


def test_only_the_sampled_thread_was_sampled(runs):
    # The main thread sleeps: had the sampler taken its samples from that thread, they would be in
    # the C library (nanosleep) or in its own waiting code, not in the probe's module.
    summary = resolved(runs["on"]["profile"])
    assert summary.in_module == summary.samples
    modules = {entry.module.basename for entry in summary.symbols if entry.module is not None}
    assert modules == {PROBE.name}, modules
    for entry in summary.symbols:
        assert "sleep" not in entry.name.lower(), entry.name


def test_profile_summary_runs_on_the_probe_profile(runs):
    result = subprocess.run([sys.executable, "-I", str(SUMMARY), str(runs["on"]["profile"]), "--top", "5"],
                            capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stderr
    assert "profile-probe" in result.stdout
    assert "busy_a" in result.stdout and "busy_b" in result.stdout
    assert "total" in result.stdout
    assert "100.0%" in result.stdout
    assert "not on this machine" not in result.stdout  # the probe's module was found

    # The probe's own functions are not part of the port, so they land in "unknown" (the bucket
    # rules are about the port); they are still the top symbols.
    top = [line for line in result.stdout.splitlines() if "busy_a" in line or "busy_b" in line]
    assert len(top) == 2, result.stdout
