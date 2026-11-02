"""T-696: package_linux.py --from-builds lays out fake pieces and writes the archive (no build, no container)."""
import hashlib
import importlib.util
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "pipeline" / "package_linux.py"
MACROS = REPO / "third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h"

pytestmark = pytest.mark.skipif(not MACROS.is_file(), reason="needs the PS2Recomp runtime header (third_party)")

EXPECTED_TOP = {"don", "libdon_gs.so", "libdon_vu.so", "libdon_spu2.so", "libSDL3.so.0", "resources", "bin", "toolchain",
                "sdk", "config", "licenses", "LICENSE", "LEGAL.md", "THIRD_PARTY_NOTICES.md", "SOURCE.txt", "README.txt"}


def fake_builds(root: Path) -> Path:
    b = root / "builds"
    (b / "libs" / "resources").mkdir(parents=True)
    (b / "libs" / "don").write_bytes(b"fake don")
    (b / "ps2_recomp").write_bytes(b"fake recomp")
    for name in ("libdon_gs.so", "libdon_vu.so", "libdon_spu2.so", "libSDL3.so.0"):
        (b / "libs" / name).write_bytes(b"fake " + name.encode())
    (b / "libs" / "resources" / "x.txt").write_text("resource")
    (b / "libs" / "BUILT_FROM.txt").write_text("commit x\n")
    for part in ("bin/clang++", "bin/ld.lld"):
        (b / "toolchain" / part).parent.mkdir(parents=True, exist_ok=True)
        (b / "toolchain" / part).write_bytes(b"fake")
    (b / "toolchain" / "TOOLCHAIN.sha256").write_text("0" * 64 + "\n")
    (b / "toolchain" / "LICENSE.TXT").write_text("LLVM licence text")
    (b / "licenses").mkdir()
    (b / "licenses" / "PS2Recomp-LICENSE").write_text("ps2recomp")
    (b / "licenses" / "PCSX2-COPYING.GPLv3").write_text("pcsx2")
    (b / "licenses" / "SDL3-LICENSE.txt").write_text("sdl")
    (b / "ps2_runtime_macros.h").write_bytes(MACROS.read_bytes())
    return b


@pytest.fixture(scope="module")
def package(tmp_path_factory):
    root = tmp_path_factory.mktemp("t696")
    builds = fake_builds(root)
    out = root / "out"
    res = subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(builds), "--out", str(out),
                          "--version", "9.9.9"], capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    archive = out / "don-linux-9.9.9.tar.gz"
    ex = root / "extracted"
    ex.mkdir()
    with tarfile.open(archive) as t:
        names = t.getnames()
        t.extractall(ex, filter="data")
    return res.stdout, archive, ex / "don-linux-9.9.9", names


def test_layout_is_exact(package):
    _, _, top, names = package
    assert {p.name for p in top.iterdir()} == EXPECTED_TOP
    print("\n".join(sorted(n for n in names if n.count("/") <= 2)))
    for part in ("bin/ps2_recomp", "toolchain/bin/clang++", "toolchain/bin/ld.lld", "sdk/inc/don_guest_macros.h",
                 "sdk/game_module/module_entry.cpp", "config/disc_manifest.toml"):
        assert (top / part).is_file(), part
    assert not (top / "BUILT_FROM.txt").exists()


def test_notices_present(package):
    _, _, top, _ = package
    lic = {p.name for p in (top / "licenses").iterdir()}
    assert {"LLVM-LICENSE.TXT", "PS2Recomp-LICENSE", "PCSX2-COPYING.GPLv3", "SDL3-LICENSE.txt", "LIBRARIES.txt"} <= lic
    assert "libSDL3.so.0" in (top / "licenses" / "LIBRARIES.txt").read_text()
    notices = (top / "THIRD_PARTY_NOTICES.md").read_text()
    for word in ("LLVM", "PCSX2", "PS2Recomp", "SDL"):
        assert word in notices
    assert "GNU GENERAL PUBLIC LICENSE" in (top / "LICENSE").read_text().upper()


def test_source_and_readme(package):
    _, _, top, _ = package
    commit = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    src = (top / "SOURCE.txt").read_text()
    assert commit in src and "https://github.com/dvilelaf/battlestadium-don" in src and "<source-url>" not in src
    assert "@" not in src
    assert "./don" in (top / "README.txt").read_text()


def test_prints_sha_and_size(package):
    stdout, archive, _, _ = package
    assert hashlib.sha256(archive.read_bytes()).hexdigest() in stdout
    assert f"{archive.stat().st_size} bytes" in stdout


def test_public_check_passes(package):
    _, _, top, _ = package
    res = subprocess.run([sys.executable, "-I", str(REPO / "tools/verify/public_check.py"), "--root", str(top)],
                         capture_output=True, text=True, cwd=REPO)
    assert res.returncode == 0, res.stdout + res.stderr


def load_script():
    spec = importlib.util.spec_from_file_location("package_linux", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_configure_is_unlinked_and_never_a_preset():
    script = load_script()
    cmd = script.configure_game_cmd(REPO, Path("/b/game-nolink"), Path("/r"))
    assert "-DDON_LINK_GENERATED=OFF" in cmd
    assert not [c for c in cmd if c.startswith("-DDON_LINK_GENERATED=") and c != "-DDON_LINK_GENERATED=OFF"]
    assert not any("preset" in c for c in cmd) and "-DDON_RUNTIME_DIR=/r" in cmd
    assert "-DCMAKE_BUILD_TYPE=Release" in cmd or any(c.startswith("-DCMAKE_BUILD_TYPE") for c in cmd)


def test_every_library_needs_its_licence_text(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "libjpeg.so.8").write_bytes(b"fake")
    res = subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(b), "--out", str(tmp_path / "o")],
                         capture_output=True, text=True)
    assert res.returncode != 0 and "libjpeg-turbo" in res.stderr + res.stdout and "libjpeg.so.8" in res.stderr + res.stdout
    (b / "licenses" / "libjpeg-turbo-LICENSE.md").write_text("bsd")
    res = subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(b), "--out", str(tmp_path / "o")],
                         capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr


def test_unknown_library_fails_naming_it(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "libmystery.so.1").write_bytes(b"fake")
    res = subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(b), "--out", str(tmp_path / "o")],
                         capture_output=True, text=True)
    assert res.returncode != 0 and "libmystery.so.1" in res.stderr + res.stdout


def test_missing_licence_fails(tmp_path):
    b = fake_builds(tmp_path)
    (b / "licenses" / "PCSX2-COPYING.GPLv3").unlink()
    res = subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(b), "--out", str(tmp_path / "o")],
                         capture_output=True, text=True)
    assert res.returncode != 0 and "PCSX2" in res.stderr + res.stdout


def test_build_image_keeps_the_licence_of_every_shipped_library():
    """build-deps.sh must copy each shipped library's licence into /opt/deps/share/licenses/<project>/ (T-698)."""
    spec = importlib.util.spec_from_file_location("stage_install", REPO / "tools" / "module" / "stage_install.py")
    stage_install = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage_install)
    script = (REPO / "tools" / "container" / "build-deps.sh").read_text()
    for project in sorted(set(stage_install.LIBRARY_PROJECTS.values())):
        assert f"keep_license {project} " in script, project
