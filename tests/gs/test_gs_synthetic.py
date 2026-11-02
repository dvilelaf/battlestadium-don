"""Tests of libdon_gs through build/gs/gsreplay with the synthetic scene (no game data).

The scene is built by src/gsbridge/gsreplay.cpp: a 640x448 frame cleared to BACKGROUND with a
rectangle of RECTANGLE colour at RECT (left, top, right, bottom; right and bottom exclusive).
Skipped when `bash tools/pipeline/build_gs.sh` has not been run.
"""
import ctypes.util
import glob
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
GSREPLAY = ROOT / "build" / "gs" / "gsreplay"
OUT_DIR = ROOT / "work" / "gs_tests"  # inside the repository so that the container sees it too

# The same constants as in gsreplay.cpp.
SIZE = (640, 448)
BACKGROUND = (0x20, 0x40, 0x80)
RECTANGLE = (0xE0, 0xC0, 0x10)
RECT = (100, 50, 300, 200)

pytestmark = pytest.mark.skipif(
    not GSREPLAY.is_file(),
    reason="build/gs/gsreplay does not exist; run: bash tools/pipeline/build_gs.sh",
)


def has_vulkan_device():
    """True when a Vulkan loader and at least one driver description are present on the host."""
    if not ctypes.util.find_library("vulkan"):
        return False
    if os.environ.get("VK_ICD_FILENAMES"):
        return True
    dirs = ["/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d"]
    return any(glob.glob(d + "/*.json") for d in dirs)


def run_gsreplay(args, name):
    """Runs gsreplay on the host; if the host cannot load it, inside the build container."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    png = OUT_DIR / name
    png.unlink(missing_ok=True)
    cmd = [str(GSREPLAY), "--synthetic", str(png), *args]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        cmd = ["bash", str(ROOT / "tools" / "container" / "run.sh"), *cmd]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return result, png


def check_picture(png):
    image = Image.open(png).convert("RGB")
    assert image.size == SIZE
    left, top, right, bottom = RECT
    assert image.getpixel((10, 10)) == BACKGROUND
    assert image.getpixel((SIZE[0] - 1, SIZE[1] - 1)) == BACKGROUND
    assert image.getpixel((200, 100)) == RECTANGLE
    # The edges of the rectangle fall on exact pixels.
    assert image.getpixel((left, top)) == RECTANGLE
    assert image.getpixel((right - 1, bottom - 1)) == RECTANGLE
    assert image.getpixel((left - 1, top)) == BACKGROUND
    assert image.getpixel((right, bottom - 1)) == BACKGROUND
    assert image.getpixel((left, bottom)) == BACKGROUND


def test_software_synthetic():
    result, png = run_gsreplay(["--renderer", "software"], "software.png")
    assert result.returncode == 0, result.stderr
    check_picture(png)


def test_software_state_roundtrip_and_reopen():
    result, png = run_gsreplay(
        ["--renderer", "software", "--state-roundtrip", "--reopen"], "software_state.png"
    )
    assert result.returncode == 0, result.stderr
    assert "state-roundtrip: size=" in result.stdout and " ok" in result.stdout
    assert "reopen:" in result.stdout and "ok" in result.stdout.split("reopen:")[1]
    check_picture(png)


@pytest.mark.skipif(not has_vulkan_device(), reason="no Vulkan loader or driver on this host")
def test_vulkan_synthetic():
    result, png = run_gsreplay(["--renderer", "vulkan", "--state-roundtrip", "--reopen"], "vulkan.png")
    assert result.returncode == 0, result.stderr
    check_picture(png)


@pytest.mark.parametrize(
    "args",
    [
        ["--renderer", "metal"],
        ["--width", "100"],  # not a multiple of 64
        ["--bogus"],
    ],
)
def test_bad_command_line(args):
    result, _ = run_gsreplay(args, "bad.png")
    assert result.returncode == 2
    assert "usage:" in result.stderr


# ---- GS dump replay (--dump) ---------------------------------------------------------------------
# The synthetic scene is written out as a .gs file by the hidden option --write-dump, then replayed
# with --dump. No game data: the dump is made by gsreplay itself, in PCSX2's format
# (docs/knowledge/15-gs-dumps.md).


def run_raw(args):
    """Runs gsreplay with exactly these arguments (host first, then the build container)."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [str(GSREPLAY), *map(str, args)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 and "error while loading shared libraries" in result.stderr:
        cmd = ["bash", str(ROOT / "tools" / "container" / "run.sh"), *cmd]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return result


@pytest.fixture(scope="module")
def synthetic_dump():
    """The scene as a dump written with the software renderer, and the picture --synthetic gave."""
    dump = OUT_DIR / "scene.gs"
    png = OUT_DIR / "scene_synthetic.png"
    dump.unlink(missing_ok=True)
    result = run_raw(["--synthetic", png, "--renderer", "software", "--write-dump", dump])
    assert result.returncode == 0, result.stderr
    assert dump.is_file()
    return dump, png


@pytest.mark.parametrize("renderer", ["software", pytest.param("vulkan", marks=pytest.mark.skipif(
    not has_vulkan_device(), reason="no Vulkan loader or driver on this host"))])
def test_dump_replay_equals_synthetic(synthetic_dump, renderer):
    dump, _ = synthetic_dump
    direct = OUT_DIR / f"scene_direct_{renderer}.png"
    replayed = OUT_DIR / f"scene_replayed_{renderer}.png"
    for path in (direct, replayed):
        path.unlink(missing_ok=True)
    result = run_raw(["--synthetic", direct, "--renderer", renderer])
    assert result.returncode == 0, result.stderr
    result = run_raw(["--dump", dump, replayed, "--renderer", renderer])
    assert result.returncode == 0, result.stderr
    assert direct.read_bytes() == replayed.read_bytes()
    check_picture(replayed)
    # The summary line: packets by type, GIF bytes per path, vertical syncs.
    assert "transfer=4 vsync=1 read_fifo=1 regs=1" in result.stdout
    assert "fifo_qwords=5" in result.stdout
    assert "replayed_vsyncs=1" in result.stdout
    assert "serial=SYNTH-00000" in result.stdout


def test_dump_frame_option(synthetic_dump):
    dump, _ = synthetic_dump
    png = OUT_DIR / "scene_frame1.png"
    png.unlink(missing_ok=True)
    result = run_raw(["--dump", dump, png, "--renderer", "software", "--frame", 1])
    assert result.returncode == 0, result.stderr
    check_picture(png)
    result = run_raw(["--dump", dump, OUT_DIR / "scene_frame2.png", "--renderer", "software", "--frame", 2])
    assert result.returncode == 1
    assert "only 1 vertical syncs" in result.stderr


@pytest.mark.skipif(not shutil.which("zstd"), reason="the zstd tool is not installed")
def test_dump_zstd_compressed(synthetic_dump):
    dump, _ = synthetic_dump
    packed = OUT_DIR / "scene.gs.zst"
    packed.unlink(missing_ok=True)
    subprocess.run(["zstd", "-q", "-f", "-o", str(packed), str(dump)], check=True)
    png = OUT_DIR / "scene_zst.png"
    png.unlink(missing_ok=True)
    result = run_raw(["--dump", packed, png, "--renderer", "software"])
    assert result.returncode == 0, result.stderr
    check_picture(png)
    # A cut Zstandard stream is an error, not a crash.
    cut = OUT_DIR / "scene_cut.gs.zst"
    cut.write_bytes(packed.read_bytes()[: packed.stat().st_size // 2])
    result = run_raw(["--dump", cut, OUT_DIR / "never.png", "--renderer", "software"])
    assert result.returncode == 1
    assert "truncated" in result.stderr


def corrupt_cases(data):
    """(name, bytes, text expected in the error) for malformed versions of a valid dump."""
    state_end = 8 + (36 + 11) + int.from_bytes(data[12:16], "little")  # header block 47 bytes
    regs_end = state_end + 0x2000
    return [
        ("empty", b"", "too short"),
        ("header_only", data[:6], "too short"),
        ("header_cut", data[:30], "header block"),
        ("state_cut", data[: state_end - 1000], "state block"),
        ("regs_cut", data[: regs_end - 1], "privileged-register block"),
        ("packet_header_cut", data[: regs_end + 3], "truncated"),
        ("packet_payload_cut", data[: regs_end + 100], "truncated"),
        ("bad_packet_id", data[:regs_end] + b"\x09", "unknown packet type 9"),
        ("bad_path", data[:regs_end] + b"\x00\x07\x10\x00\x00\x00" + bytes(16), "unknown transfer path"),
        ("bad_length", data[:regs_end] + b"\x00\x03\x11\x00\x00\x00" + bytes(17), "not a multiple of 16"),
        ("no_vsync", data[:regs_end], "no vertical sync"),
    ]


def test_dump_malformed_files_fail_cleanly(synthetic_dump):
    dump, _ = synthetic_dump
    data = dump.read_bytes()
    for name, content, expected in corrupt_cases(data):
        path = OUT_DIR / f"bad_{name}.gs"
        path.write_bytes(content)
        result = run_raw(["--dump", path, OUT_DIR / "never.png", "--renderer", "software"])
        assert result.returncode == 1, (name, result.returncode, result.stderr)
        assert expected in result.stderr, (name, result.stderr)
    missing = run_raw(["--dump", OUT_DIR / "does_not_exist.gs", OUT_DIR / "never.png"])
    assert missing.returncode == 1 and "cannot open" in missing.stderr


def test_dump_usage_errors():
    for args in (["--dump", "a.gs"], ["--dump", "a.gs", "b.png", "--synthetic", "c.png"],
                 ["--synthetic", "a.png", "--frame", "1"], ["--dump", "a.gs", "b.png", "--frame", "0"]):
        result = run_raw(args)
        assert result.returncode == 2 and "usage:" in result.stderr, args
