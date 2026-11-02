"""Tests of the host program (T-110 part 2) with the synthetic guest of host_synthetic_test.cpp.

    bash tools/pipeline/build_host.sh          # build/host/host_synthetic_test (needs build/gs from build_gs.sh)
    uv run --no-project --with pillow --with pytest python -I -m pytest tests/host -q

The folder with the executable is $DON_HOST_TESTS_DIR (default build/host). Everything is skipped, with the
reason, when the executable or the GS library is missing. The windowed test needs xvfb-run and ImageMagick's
`import` and runs only on a virtual display with Mesa's software Vulkan driver, never on the user's display.
Screenshots go to work/shots/t110_pytest/.

The guest sets up the display only through the runtime's sceGs* handlers (no direct write of a privileged
register), and sets the scene's own SCISSOR_2/ALPHA_1 after the display set-up, because sceGsResetGraph
resets the GS and a GS reset clears the drawing registers.

What is checked:
  * exit codes: 0, 2 (argument error), 3 (no GS library), 4 (the guest throws);
  * both renderers, headless: at least 5 PNGs per run, with the scene's colours at known positions, the moving
    box moving, and about 60 vertical blanks per second; that scene includes the two registers a GS reset
    clears and the old sceGsResetGraph stand-in packet used to overwrite, SCISSOR_2 (the context-2 box) and
    ALPHA_1 (the blended box);
  * a run that sets SCISSOR_2 and ALPHA_1 again before every frame produces the same bytes as one that sets
    them once, so the reset left them alone;
  * a windowed run on Xvfb, with a screen grab;
  * T-303: sound is on by default (--wav with --sound off is an argument error, a missing library is one line, --sound on with an empty
    --spu2-dir exits 3), and --sound on --wav <file> writes a valid WAV of about the run's length, all
    silence (the synthetic guest makes no sound);
  * T-205: DON_VU1=microvu and DON_VU1=pcsx2interp leave the picture byte-identical to the
    DON_VU1=runtime run, and with DON_VU1 unset and an empty --vu-dir the run still goes on with one
    [don-vu] warning (the synthetic guest uses no vector unit) and their summary line reports zero microprograms.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
BUILD = Path(os.environ.get("DON_HOST_TESTS_DIR", "build/host"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
EXE = BUILD / ("host_synthetic_test" + (".exe" if sys.platform == "win32" else ""))
PACING_EXE = BUILD / ("pacing_stats_test" + (".exe" if sys.platform == "win32" else ""))
GS_DIR = ROOT / "build" / "gs"
GS_LIB = GS_DIR / "libdon_gs.so"
SPU2_LIB = ROOT / "build" / "spu2" / "libdon_spu2.so"
VU_LIB = ROOT / "build" / "vu" / "libdon_vu.so"
WAV_STATS = ROOT / "tools" / "verify" / "wav_stats.py"
SHOTS = ROOT / "work" / "shots" / "t110_pytest"
LVP = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"

# T-524: never touch the owner's real ~/.local/share/don (cards, log, cache): every run here writes to
# a throwaway user folder. Inherited by all subprocesses (run() copies os.environ).
os.environ.setdefault("DON_USER_DIR", tempfile.mkdtemp(prefix="don-host-test-"))

pytestmark = pytest.mark.skipif(
    not EXE.is_file() or not GS_LIB.is_file(),
    reason=f"needs {EXE} (bash tools/pipeline/build_host.sh) and {GS_LIB} (bash tools/pipeline/build_gs.sh)",
)

# The scene (640x448), see tests/host/host_synthetic_test.cpp.
BG = (0x20, 0x40, 0x80)
POINTS = {
    "background": ((5, 5), BG),
    "red box": ((90, 80), (0xE0, 0x20, 0x20)),
    "green box": ((250, 80), (0x20, 0xE0, 0x20)),
    "blue box": ((410, 80), (0x20, 0x20, 0xE0)),
    "context-2 box": ((300, 190), (0xC8, 0x64, 0x32)),
    # (Cs - Cd) * As + Cd with As = 0x30 / 0x80 = 0.375
    "blended box": ((300, 330), tuple(round(d + (255 - d) * 0.375) for d in BG)),
    "yellow box row": ((5, 410), BG),
}
YELLOW = (0xFF, 0xFF, 0x00)


def run(args, env=None, timeout=120):
    full_env = dict(os.environ)
    for name in ("DON_TEST_FAIL", "DON_TEST_PERFRAME", "DON_GS_DIR", "DON_HOST_LOG", "DON_SPU2_DIR",
                 "DON_AUDIO_LOG", "DON_VU1", "DON_VU_LOG", "DON_VU_DIR"):
        full_env.pop(name, None)
    full_env.update(env or {})
    if "--sound" not in args:  # sound is on by default; the tests that are not about it ask for silence
        args = [*args, "--sound", "off"]
    return subprocess.run([str(EXE), *args], capture_output=True, text=True, timeout=timeout, env=full_env,
                          cwd=ROOT)


def close(a, b, tolerance=3):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def shot_files(folder):
    return sorted(Path(folder).glob("*.png"))


def pixel(image, x, y):
    """Pixel at scene coordinates (x, y) of a picture of any size (the scene is 640x448)."""
    sx = image.width / 640
    sy = image.height / 448
    return image.getpixel((min(image.width - 1, int(x * sx)), min(image.height - 1, int(y * sy))))


def yellow_left_edge(image):
    row = int(410 * image.height / 448)
    for x in range(image.width):
        if close(image.getpixel((x, row)), YELLOW):
            return x
    return None


def figure(text, name):
    match = re.search(rf"^{name}: ([0-9.]+)", text, re.MULTILINE)
    assert match, f"no {name} line in:\n{text}"
    return float(match.group(1))


def headless(name, extra_env=None, renderer="vulkan", seconds=4, every=30, extra_args=()):
    folder = SHOTS / name
    shutil.rmtree(folder, ignore_errors=True)
    result = run(["--headless", "--renderer", renderer, "--seconds", str(seconds), "--shots", str(folder),
                  "--shot-every", str(every), *extra_args],
                 env=dict({"DON_HOST_LOG": "1"}, **(extra_env or {})))
    return result, folder


def test_exit_code_2_on_bad_argument():
    result = run(["--does-not-exist"])
    assert result.returncode == 2, result.stderr
    assert "[don]" in result.stderr


def test_fullscreen_option_parses():
    """T-522: --fullscreen is a valid flag (no value); the usage line names it."""
    result = run(["--help"])
    assert result.returncode == 0, result.stderr
    assert "--fullscreen" in result.stderr, result.stderr

    # --fullscreen is accepted by the parser (a headless run ignores it and goes on).
    result = run(["--headless", "--seconds", "2", "--fullscreen"])
    assert result.returncode != 2, result.stderr
    assert "try --help" not in result.stderr


def test_net_option_argument_errors():
    """T-504: each bad net option exits 2 with a message naming the option."""
    cases = [
        (["--net-host", "0"], "--net-host"),
        (["--net-host", "70000"], "--net-host"),
        (["--net-host", "abc"], "--net-host"),
        (["--net-host"], "--net-host"),                          # missing value
        (["--net-host", "7877", "--net-join", "h:7877"], "--net-host"),
        (["--net-players", "4"], "--net-players"),               # without --net-host
        (["--net-delay", "2"], "--net-delay"),                   # without --net-host
        (["--net-host", "7877", "--net-players", "5"], "--net-players"),
        (["--net-host", "7877", "--net-delay", "0"], "--net-delay"),
        (["--net-name", "1234567890123456"], "--net-name"),      # 16 characters
    ]
    for args, name in cases:
        result = run(args)
        assert result.returncode == 2, (args, result.stderr)
        assert name in result.stderr, (args, result.stderr)
        assert "try --help" in result.stderr


def test_net_option_combination_errors():
    result = run(["--sound", "off", "--net-host", "7877"])
    assert result.returncode == 2 and "--sound off" in result.stderr, result.stderr
    result = run(["--players", "2", "--net-host", "7877", "--sound", "on"])
    assert result.returncode == 2 and "--players" in result.stderr, result.stderr


def test_net_host_options_are_accepted():
    """A valid --net-host line is accepted by the parser: not exit 2 and no argument-error message."""
    result = run(["--net-host", "7877", "--net-players", "4", "--net-delay", "2", "--sound", "on",
                  "--headless", "--seconds", "2"])
    assert result.returncode != 2, result.stderr
    assert "try --help" not in result.stderr


def test_exit_code_3_without_the_gs_library(tmp_path):
    result = run(["--headless", "--seconds", "2", "--gs-dir", str(tmp_path)])
    assert result.returncode == 3, result.stderr
    assert "cannot find the GS library" in result.stderr
    result = run(["--headless", "--seconds", "2"], env={"DON_GS_DIR": str(tmp_path)})
    assert result.returncode == 3, result.stderr


def test_exit_code_4_when_the_guest_throws():
    result = run(["--headless", "--seconds", "20"], env={"DON_TEST_FAIL": "20"})
    assert result.returncode == 4, result.stderr
    assert "the runtime ended with an error" in result.stderr


@pytest.mark.parametrize("renderer", ["vulkan", "software"])
def test_headless_scene_colours_and_rate(renderer):
    result, folder = headless(f"headless_{renderer}", renderer=renderer, seconds=4)
    assert result.returncode == 0, result.stderr
    shots = shot_files(folder)
    assert len(shots) >= 5, f"{len(shots)} screenshots"
    edges = []
    for path in shots:
        image = Image.open(path).convert("RGB")
        assert image.size == (640, 448), image.size
        for name, ((x, y), expected) in POINTS.items():
            got = pixel(image, x, y)
            assert close(got, expected), f"{path.name}: {name} at ({x},{y}) is {got}, expected {expected}"
        edge = yellow_left_edge(image)
        assert edge is not None, f"{path.name}: no yellow box"
        edges.append(edge)
    assert len(set(edges)) == len(edges), f"the moving box does not move: {edges}"
    match = re.search(r"\[don\] GS: (\d+) vblanks", result.stderr)
    assert match, result.stderr
    vblanks = int(match.group(1))
    assert 4 * 60 * 0.9 <= vblanks <= 4 * 60 * 1.1, f"{vblanks} vblanks in 4 s"


def test_input_script_pads_shots_and_quit(tmp_path):
    """T-112: a script drives the pad through the host and the guest shows what it read.

    The guest reads port 0 slot 0 with the runtime's replaced scePad* handlers (scePadGetState must say
    "stable" first) and paints two indicators: red while CIRCLE is held, green while LEFT is held (see
    tests/host/host_synthetic_test.cpp). The script taps CIRCLE, then LEFT, taking a shot inside each tap,
    and ends with quit. Two runs must give byte-identical shots: the frames are the frames the guest
    received, not wall-clock time.
    """
    script = tmp_path / "answer.txt"
    script.write_text(
        "# T-112 test: shot, CIRCLE (shot inside the tap), LEFT (shot inside the tap), quit.\n"
        "10  shot before\n"
        "30  tap CIRCLE 30\n"
        "45  shot circle\n"
        "70  tap LEFT 30\n"
        "85  shot left\n"
        "130 quit\n"
    )
    folders = []
    for attempt in ("a", "b"):
        folder = SHOTS / f"script_{attempt}"
        shutil.rmtree(folder, ignore_errors=True)
        result = run(["--headless", "--renderer", "software", "--seconds", "20", "--shots", str(folder),
                      "--input-script", str(script)], timeout=120)
        assert result.returncode == 0, result.stderr
        folders.append(folder)

    for folder in folders:
        for name in ("before", "circle", "left"):
            assert (folder / f"{name}.png").is_file(), f"{name}.png is missing in {folder}"
    before = Image.open(folders[0] / "before.png").convert("RGB")
    circle = Image.open(folders[0] / "circle.png").convert("RGB")
    left = Image.open(folders[0] / "left.png").convert("RGB")
    assert pixel(before, 30, 220) == (0, 0, 0), "CIRCLE was held before the script tapped it"
    assert pixel(before, 100, 220) == (0, 0, 0), "LEFT was held before the script tapped it"
    assert pixel(circle, 30, 220) == (0xFF, 0, 0), "CIRCLE is not seen by the guest during its tap"
    assert pixel(circle, 100, 220) == (0, 0, 0), "LEFT is held during the CIRCLE tap"
    assert pixel(left, 30, 220) == (0, 0, 0), "CIRCLE is still held during the LEFT tap"
    assert pixel(left, 100, 220) == (0, 0xFF, 0), "LEFT is not seen by the guest during its tap"
    # Repeatable: the same script gives the same pictures, byte for byte.
    for name in ("before", "circle", "left"):
        assert (folders[0] / f"{name}.png").read_bytes() == (folders[1] / f"{name}.png").read_bytes(), name


def test_input_script_error_reports_the_line_and_exits_2(tmp_path):
    script = tmp_path / "broken.txt"
    script.write_text("10 shot before\n30 tap NOTABUTTON 4\n40 tap LEFT 4\n")
    folder = SHOTS / "script_broken"
    shutil.rmtree(folder, ignore_errors=True)
    result = run(["--headless", "--seconds", "5", "--shots", str(folder), "--input-script", str(script)])
    assert result.returncode == 2, result.stderr
    assert "line 2" in result.stderr, result.stderr
    assert "NOTABUTTON" in result.stderr, result.stderr
    assert not folder.exists() or not list(folder.glob("*.png")), "a broken script started the run"


def test_input_script_quit_ends_the_run(tmp_path):
    script = tmp_path / "quick.txt"
    script.write_text("20 shot early\n30 quit\n")
    folder = SHOTS / "script_quit"
    shutil.rmtree(folder, ignore_errors=True)
    # No --seconds: only quit can end this run, and it must do it with 0.
    result = run(["--headless", "--renderer", "software", "--shots", str(folder), "--input-script", str(script)],
                 env={"DON_HOST_LOG": "1"}, timeout=60)
    assert result.returncode == 0, result.stderr
    assert (folder / "early.png").is_file(), result.stderr
    match = re.search(r"\[don\] GS: (\d+) vblanks", result.stderr)
    assert match, result.stderr
    vblanks = int(match.group(1))
    assert 30 <= vblanks <= 120, f"quit did not end the run at its frame ({vblanks} vblanks)"


def test_the_reset_leaves_scissor2_and_alpha1_alone():
    """SCISSOR_2 and ALPHA_1 (the context-2 box and the blended box) survive the display set-up.

    sceGsResetGraph used to send a stand-in packet whose entries 0x41 and 0x42 landed on those two real GS
    registers; the host program filtered them out. The handler writes nothing to the GS through the GIF now,
    so a run that sets the two registers once after the display set-up and one that sets them again before
    every frame must produce the same bytes.
    """
    once, once_dir = headless("state_once", seconds=2)
    every, every_dir = headless("state_every_frame", {"DON_TEST_PERFRAME": "1"}, seconds=2)
    assert once.returncode == 0, once.stderr
    assert every.returncode == 0, every.stderr
    name = "frame_000060.png"
    for folder in (once_dir, every_dir):
        image = Image.open(folder / name).convert("RGB")
        assert close(pixel(image, 300, 190), POINTS["context-2 box"][1]), (
            f"{folder.name}: the context-2 box is not drawn (SCISSOR_2 clipped it)")
        assert close(pixel(image, 300, 330), POINTS["blended box"][1]), (
            f"{folder.name}: the blended box shows the wrong equation (ALPHA_1 was overwritten)")
    assert (every_dir / name).read_bytes() == (once_dir / name).read_bytes()


@pytest.mark.skipif(not shutil.which("xvfb-run") or not shutil.which("import"),
                    reason="needs xvfb-run and ImageMagick's import")
@pytest.mark.skipif(not Path(LVP).is_file(), reason=f"needs Mesa's software Vulkan driver ({LVP})")
@pytest.mark.parametrize("renderer", ["software", "vulkan"])
def test_windowed_on_xvfb(renderer, tmp_path):
    folder = SHOTS / f"windowed_{renderer}"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    grab = folder / "screen.png"
    script = tmp_path / "windowed.sh"
    # T-517: the start screen opens before the game window, so a run without one of its skip flags waits for a
    # human. A windowed test is automated: an (empty) input script skips the screen and lets the scene render.
    noop_input = tmp_path / "noop.input"
    noop_input.write_text("# no commands: run until --seconds ends it\n")
    script.write_text(
        "#!/usr/bin/env bash\n"
        f'"{EXE}" --renderer {renderer} --seconds 8 --shots "{folder}" --shot-every 60'
        f' --input-script "{noop_input}" &\n'
        "pid=$!\n"
        "sleep 5\n"
        f'import -window root "{grab}"\n'
        "wait $pid\n"
        "exit $?\n"
    )
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.update({"VK_ICD_FILENAMES": LVP, "SDL_VIDEODRIVER": "x11", "DON_HOST_LOG": "1"})
    result = subprocess.run(["xvfb-run", "-a", "-s", "-screen 0 1400x1000x24", "bash", str(script)],
                            capture_output=True, text=True, timeout=180, env=env)
    assert result.returncode == 0, result.stderr
    assert "window)" in result.stdout + result.stderr
    screen = Image.open(grab).convert("RGB")
    colours = {screen.getpixel((x, y)) for x in range(0, screen.width, 7) for y in range(0, screen.height, 7)}
    for expected in ((0xE0, 0x20, 0x20), (0x20, 0xE0, 0x20), (0x20, 0x20, 0xE0), (0xC8, 0x64, 0x32), YELLOW):
        assert any(close(c, expected, 6) for c in colours), f"colour {expected} not on the screen grab"
    shots = list(folder.glob("frame_*.png"))
    assert len(shots) >= 5, f"{len(shots)} screenshots"


# ---------------------------------------------------------------------------------------------
# T-303, sound. On by default since 2026-10-14; `run` adds --sound off unless the test gives --sound.
# ---------------------------------------------------------------------------------------------


def test_wav_with_sound_off_is_an_argument_error(tmp_path):
    result = run(["--headless", "--seconds", "2", "--sound", "off", "--wav", str(tmp_path / "x.wav")])
    assert result.returncode == 2, result.stderr
    assert "--wav needs sound" in result.stderr, result.stderr
    assert not (tmp_path / "x.wav").exists()

    result = run(["--headless", "--seconds", "2", "--sound", "off", "--wav", str(tmp_path / "x.wav")])
    assert result.returncode == 2, result.stderr

    result = run(["--headless", "--seconds", "2", "--sound", "maybe"])
    assert result.returncode == 2, result.stderr
    assert "--sound must be on or off" in result.stderr, result.stderr


def test_default_sound_without_the_spu2_library_goes_on_silent(tmp_path):
    empty = tmp_path / "no-library"
    empty.mkdir()
    result = subprocess.run([str(EXE), "--headless", "--seconds", "2", "--spu2-dir", str(empty)],
                            capture_output=True, text=True, timeout=120, cwd=ROOT)
    assert result.returncode == 0, result.stderr
    assert "[don] no SPU2 library: running without sound" in result.stderr, result.stderr


def test_sound_on_without_the_spu2_library_exits_3(tmp_path):
    empty = tmp_path / "no-library"
    empty.mkdir()
    result = run(["--headless", "--seconds", "2", "--sound", "on", "--spu2-dir", str(empty)])
    assert result.returncode == 3, result.stderr
    assert "[don-spu2]" in result.stderr and "cannot find the SPU2 library" in result.stderr, result.stderr


@pytest.mark.skipif(not SPU2_LIB.is_file(), reason=f"needs {SPU2_LIB} (bash tools/pipeline/build_spu2.sh)")
def test_sound_on_writes_a_wav_of_silence_of_the_run_length(tmp_path):
    """The synthetic guest makes no sound: the WAV must be a valid, silent recording of the run."""
    wav = tmp_path / "sound.wav"
    result = run(["--headless", "--renderer", "software", "--seconds", "3", "--sound", "on",
                  "--wav", str(wav)], env={"DON_AUDIO_LOG": "1"}, timeout=180)
    assert result.returncode == 0, result.stderr
    assert "SPU2 library" not in result.stderr  # that line needs DON_HOST_LOG, which is off here
    assert wav.is_file(), f"no WAV in {tmp_path}"

    stats = subprocess.run([sys.executable, "-I", str(WAV_STATS), str(wav)], capture_output=True, text=True,
                           timeout=120)
    assert stats.returncode == 0, stats.stdout + stats.stderr
    assert figure(stats.stdout, "silent") == 1.0, stats.stdout  # not a single sample above the threshold
    assert figure(stats.stdout, "saturated") == 0.0, stats.stdout
    duration = figure(stats.stdout, "duration")
    assert 2.0 <= duration <= 4.5, f"{duration} s of audio for a 3 s run"

    # The output side reports what it wrote, and no device was opened (headless).
    summary = re.search(r"\[don-spu2\] audio: (\d+) frames \(([0-9.]+) s\), device (\w+)", result.stderr)
    assert summary, result.stderr
    assert summary.group(3) == "closed", summary.group(0)
    assert abs(float(summary.group(2)) - duration) < 0.5, (summary.group(0), duration)


def test_sound_off_loads_nothing_of_the_sound_path():
    """A run with sound off is today's run: no SPU2 line at all, with DON_AUDIO_LOG and DON_HOST_LOG on."""
    result = run(["--headless", "--renderer", "software", "--seconds", "2"],
                 env={"DON_AUDIO_LOG": "1", "DON_HOST_LOG": "1"}, timeout=120)
    assert result.returncode == 0, result.stderr
    assert "don-spu2" not in result.stderr, result.stderr


# ---------------------------------------------------------------------------------------------
# T-205, the host VU1. The synthetic guest uses no vector unit, so the picture must not change.
# ---------------------------------------------------------------------------------------------


@pytest.mark.skipif(not VU_LIB.is_file(), reason=f"needs {VU_LIB} (bash tools/pipeline/build_vu.sh)")
@pytest.mark.parametrize("selection", ["microvu", "pcsx2interp"])
def test_vu1_selection_leaves_the_picture_identical(selection):
    # The baseline is the runtime's own interpreter: unset now means microVU (T-205).
    default, default_dir = headless("vu1_default", renderer="software", seconds=3, every=30,
                                    extra_env={"DON_VU1": "runtime"})
    assert default.returncode == 0, default.stderr

    run_result, folder = headless(f"vu1_{selection}", renderer="software", seconds=3, every=30,
                                  extra_env={"DON_VU1": selection, "DON_VU_LOG": "1"})
    assert run_result.returncode == 0, run_result.stderr

    # The summary line of the one session, with no microprogram at all (this guest never starts one).
    match = re.search(r"\[don-vu\] VU1 \((\w+)\): (\d+) programs started, (\d+) continuations", run_result.stderr)
    assert match, run_result.stderr
    assert match.group(1) == selection
    assert match.group(2) == "0" and match.group(3) == "0", match.group(0)

    names = [path.name for path in shot_files(default_dir)]
    assert len(names) >= 5, names
    for name in names:
        assert (folder / name).is_file(), f"{name} is missing with DON_VU1={selection}"
        assert (folder / name).read_bytes() == (default_dir / name).read_bytes(), (
            f"{name} differs with DON_VU1={selection}")


def test_vu1_unset_without_library_warns_and_runs(tmp_path):
    """DON_VU1 unset means microVU; with no library in --vu-dir the run goes on on the runtime's interpreter."""
    empty = tmp_path / "empty_vu"
    empty.mkdir()
    result = run(["--headless", "--renderer", "software", "--seconds", "2", "--vu-dir", str(empty)])
    assert result.returncode == 0, result.stderr
    assert result.stderr.count("[don-vu] warning: no VU1 library") == 1, result.stderr


# ---------------------------------------------------------------------------------------------
# T-206: the floating-point mode of the game thread, read from the guest's own arithmetic.
# ---------------------------------------------------------------------------------------------


def test_fp_mode_of_the_game_thread():
    """The guest computes five operations and paints the low byte of each result as a pixel.

    host_synthetic_test.cpp's fp_mode_bytes() and fma_byte() compute, with the thread's own mode:
    multiply 255.0f * 0.00197001174f, add 1.0f + 8.9e-8f, divide 1.0f / 3.0f, multiply 1e-38f * 0.5f
    (a denormal), and a*b+c chosen so that fusion changes the low byte. The pixels are 1x1 sprites at
    y = 440, x = 10, 30, 50, 70, 90, greyscale (r = g = b = the low byte). The console's mode is round
    toward zero with FTZ and DAZ (the PS2's, and PCSX2's for the EE and both VUs); the host's is round
    to nearest with denormals kept. The last byte must be 0x00 in both: the project's -ffp-contract=off.
    """
    expected = {
        "console": (0x34, 0x00, 0xAA, 0x00, 0x00),  # 0x3F009A34 0x3F800000 0x3EAAAAAA 0x00000000
        "host": (0x35, 0x01, 0xAB, 0xF7, 0x00),     # 0x3F009A35 0x3F800001 0x3EAAAAAB 0x003671F7
    }
    for mode, want in expected.items():
        folder = SHOTS / f"fp_mode_{mode}"
        shutil.rmtree(folder, ignore_errors=True)
        result = run(["--headless", "--renderer", "software", "--seconds", "2", "--shots", str(folder)],
                     env={"DON_FP_MODE": mode}, timeout=120)
        assert result.returncode == 0, result.stderr
        shots = shot_files(folder)
        assert shots, result.stderr
        image = Image.open(shots[-1]).convert("RGB")
        got = tuple(image.getpixel((10 + 20 * i, 440))[0] for i in range(5))
        assert got == want, f"DON_FP_MODE={mode}: {got} != {want}"


# ---------------------------------------------------------------------------------------------
# T-305: the arithmetic of DON_PACING_LOG's summary (header only, no game, no GS library).
# ---------------------------------------------------------------------------------------------


@pytest.mark.skipif(not PACING_EXE.is_file(),
                    reason=f"needs {PACING_EXE} (bash tools/pipeline/build_host.sh)")
def test_pacing_stats_arithmetic():
    result = subprocess.run([str(PACING_EXE)], capture_output=True, text=True, timeout=60, cwd=ROOT)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "pacing_stats_test: all checks passed" in result.stdout, result.stdout


def test_default_search_is_next_to_the_program_then_gs_folder_then_the_sibling_gs(tmp_path):
    # The layout that is shipped (docs/BUILDING.md): <program folder>, <program folder>/gs, <program folder>/../gs.
    # A program copied to a folder with no library reports exactly those places, in that order.
    app = tmp_path / "app"
    app.mkdir()
    target = app / EXE.name
    shutil.copy2(EXE, target)
    for library in EXE.parent.glob("libSDL3*"):  # the window library is found next to the program
        shutil.copy2(library, app / library.name, follow_symlinks=True)
    full_env = {k: v for k, v in os.environ.items() if k not in ("DON_GS_DIR", "DON_VU_DIR", "DON_SPU2_DIR")}
    result = subprocess.run([str(target), "--headless", "--seconds", "2", "--sound", "off"], capture_output=True,
                            text=True, timeout=60, env=full_env, cwd=tmp_path)
    assert result.returncode == 3, result.stderr
    lines = [line.strip() for line in result.stderr.splitlines() if "not found" in line]
    assert len(lines) == 3, result.stderr
    folders = [Path(line.rsplit(": not found", 1)[0]).parent for line in lines]
    assert folders[0].resolve() == app.resolve()
    assert folders[1].resolve() == (app / "gs").resolve()
    assert folders[2].resolve() == (tmp_path / "gs").resolve()
