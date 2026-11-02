import sys
from pathlib import Path

# python -I does not add the script folder to sys.path; the tool lives in tools/run/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "run"))

import perf_fight  # noqa: E402


def real_builds(procs, excluded=()):
    return perf_fight.real_build_pids(procs, excluded)


def test_ninja_and_build_script_are_builds():
    procs = [
        (100, ["ninja", "-C", "build/x"]),
        (101, ["bash", "tools/pipeline/build.sh", "linux-dev"]),
        (102, ["/usr/bin/python3", "-I", "tools/pipeline/build_gs.py"]),
    ]
    assert real_builds(procs) == {100, 101, 102}


def test_shells_editors_and_greps_that_only_mention_builds_are_not():
    procs = [
        (200, ["bash", "-c", "echo build.sh"]),
        (201, ["grep", "ninja", "log"]),
        (202, ["vim", "tools/pipeline/build.sh"]),
        (203, ["bash", "-c", "uv run python -I tools/run/perf_fight.py"]),
    ]
    assert real_builds(procs) == set()


def test_build_scripts_with_a_dash_c_option_are_builds():
    # "-c" of a build script's own arguments is not the interpreter's "-c" (that one runs a string).
    procs = [
        (500, ["bash", "tools/pipeline/build.sh", "-c", "release"]),
        (501, ["python", "-I", "tools/pipeline/package_linux.py", "-c", "cfg"]),
    ]
    assert real_builds(procs) == {500, 501}


def test_uv_child_python_running_a_package_script_is_a_build():
    # What `uv run --no-project python -I tools/pipeline/package_linux.py` leaves running.
    procs = [(600, ["python", "-I", "tools/pipeline/package_linux.py"])]
    assert real_builds(procs) == {600}


def test_containerised_ninja_seen_from_the_host_is_a_build():
    procs = [(700, ["/usr/bin/ninja", "-C", "build/x"])]
    assert real_builds(procs) == {700}


def test_excluded_pids_are_never_builds():
    procs = [(300, ["ninja"]), (301, ["bash", "tools/pipeline/build.sh"])]
    assert real_builds(procs, excluded={300}) == {301}


def test_empty_argv_is_not_a_build():
    # Kernel threads and processes that exit meanwhile have an empty cmdline.
    assert real_builds([(400, []), (401, [])]) == set()


def test_game_module_is_passed_to_the_game_before_the_elf():
    command = perf_fight.game_command("don", "software", "off", 200, "/tmp/libdon_game.so")
    assert command[-3:] == ["--game-module", "/tmp/libdon_game.so", perf_fight.ELF]


def test_no_game_module_option_without_a_module():
    command = perf_fight.game_command("don", "software", "off", 200)
    assert "--game-module" not in command
    assert command[-1] == perf_fight.ELF


def test_fight_end_is_read_from_the_exit_line_frames_when_the_trace_stops_short():
    # The state trace prints every 60 blanks: a run that ends at blank 7800 has its last trace line at 7740.
    assert perf_fight.fight_reached(last_blank=7740, frames=7800)
    assert not perf_fight.fight_reached(last_blank=7740, frames=7700)


def test_fight_end_falls_back_to_the_last_trace_line_without_an_exit_line():
    assert perf_fight.fight_reached(last_blank=7800, frames=None)
    assert not perf_fight.fight_reached(last_blank=4860, frames=None)


def test_binary_outside_the_clone_is_shown_by_its_absolute_path():
    # The measured binaries live in the main checkout, not in this clone: relative_to would raise.
    outside = Path("/elsewhere/build/don")
    assert perf_fight.shown_path(outside) == "/elsewhere/build/don"


def test_binary_inside_the_clone_is_shown_relative():
    inside = perf_fight.ROOT / "build" / "linux-dev" / "don"
    assert perf_fight.shown_path(inside) == "build/linux-dev/don"
