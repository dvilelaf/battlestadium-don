"""T-742: package_windows.py --from-builds lays out fake pieces and writes the zip (no build, no container)."""
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "pipeline" / "package_windows.py"
MACROS = REPO / "third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h"

pytestmark = pytest.mark.skipif(not MACROS.is_file(), reason="needs the PS2Recomp runtime header (third_party)")

PROGRAM = "Battle Stadium DON.exe"
EXPECTED_TOP = {PROGRAM, "don_gs.dll", "don_vu.dll", "don_spu2.dll", "SDL3.dll", "resources", "bin", "toolchain", "sdk",
                "config", "licenses", "LICENSE", "LEGAL.md", "THIRD_PARTY_NOTICES.md", "SOURCE.txt", "README.txt"}


def fake_builds(root: Path) -> Path:
    b = root / "builds"
    (b / "libs" / "resources").mkdir(parents=True)
    (b / "libs" / "don.exe").write_bytes(b"fake don")
    (b / "ps2_recomp.exe").write_bytes(b"fake recomp")
    for name in ("don_gs.dll", "don_vu.dll", "don_spu2.dll", "SDL3.dll"):
        (b / "libs" / name).write_bytes(b"fake " + name.encode())
    (b / "libs" / "resources" / "x.txt").write_text("resource")
    (b / "libs" / "BUILT_FROM.txt").write_text("commit x\n")
    for part in ("bin/clang++.exe", "bin/lld-link.exe"):
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


def run_script(builds: Path, out: Path):
    return subprocess.run([sys.executable, "-I", str(SCRIPT), "--from-builds", str(builds), "--out", str(out),
                           "--version", "9.9.9"], capture_output=True, text=True)


@pytest.fixture(scope="module")
def package(tmp_path_factory):
    root = tmp_path_factory.mktemp("t742")
    builds = fake_builds(root)
    out = root / "out"
    res = run_script(builds, out)
    assert res.returncode == 0, res.stdout + res.stderr
    archive = out / "don-windows-9.9.9.zip"
    ex = root / "extracted"
    ex.mkdir()
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
        z.extractall(ex)
    return res.stdout, archive, ex / "don-windows-9.9.9", names


def test_layout_is_exact(package):
    _, _, top, names = package
    assert {p.name for p in top.iterdir()} == EXPECTED_TOP
    for part in ("bin/ps2_recomp.exe", "toolchain/bin/clang++.exe", "toolchain/bin/lld-link.exe",
                 "sdk/inc/don_guest_macros.h", "sdk/game_module/module_entry.cpp", "config/disc_manifest.toml"):
        assert (top / part).is_file(), part
    assert not (top / "BUILT_FROM.txt").exists() and not (top / "don.exe").exists()
    assert all(n.startswith("don-windows-9.9.9/") for n in names)


def test_notices_present(package):
    _, _, top, _ = package
    lic = {p.name for p in (top / "licenses").iterdir()}
    assert {"LLVM-LICENSE.TXT", "PS2Recomp-LICENSE", "PCSX2-COPYING.GPLv3", "SDL3-LICENSE.txt", "LIBRARIES.txt"} <= lic
    assert "SDL3.dll" in (top / "licenses" / "LIBRARIES.txt").read_text()
    notices = (top / "THIRD_PARTY_NOTICES.md").read_text()
    for word in ("LLVM", "PCSX2", "PS2Recomp", "SDL", "win64", "Visual C++"):
        assert word in notices
    assert "GNU GENERAL PUBLIC LICENSE" in (top / "LICENSE").read_text().upper()


def test_source_and_readme(package):
    _, _, top, _ = package
    commit = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    src = (top / "SOURCE.txt").read_bytes().decode()
    assert commit in src and "https://github.com/dvilelaf/battlestadium-don" in src and "<source-url>" not in src
    assert "@" not in src and "\r\n" in src
    readme = (top / "README.txt").read_bytes().decode()
    assert "Battle Stadium DON.exe" in readme and "disc image" in readme and "\r\n" in readme


def test_prints_sha_and_size(package):
    stdout, archive, _, _ = package
    assert hashlib.sha256(archive.read_bytes()).hexdigest() in stdout
    assert f"{archive.stat().st_size} bytes" in stdout


def test_zip_is_reproducible(package, tmp_path):
    _, archive, _, _ = package
    again = run_script(archive.parents[1] / "builds", tmp_path / "again")
    assert again.returncode == 0, again.stdout + again.stderr
    assert (tmp_path / "again" / archive.name).read_bytes() == archive.read_bytes()


def test_public_check_passes(package):
    _, _, top, _ = package
    res = subprocess.run([sys.executable, "-I", str(REPO / "tools/verify/public_check.py"), "--root", str(top)],
                         capture_output=True, text=True, cwd=REPO)
    assert res.returncode == 0, res.stdout + res.stderr


def test_every_dll_needs_its_licence_text(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "jpeg62.dll").write_bytes(b"fake")
    res = run_script(b, tmp_path / "o")
    assert res.returncode != 0 and "libjpeg-turbo" in res.stderr + res.stdout and "jpeg62.dll" in res.stderr + res.stdout
    (b / "licenses" / "libjpeg-turbo-LICENSE.md").write_text("bsd")
    res = run_script(b, tmp_path / "o")
    assert res.returncode == 0, res.stdout + res.stderr


def test_microsoft_runtime_dll_needs_the_redistributable_terms(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "vcruntime140.dll").write_bytes(b"fake")
    res = run_script(b, tmp_path / "o")
    assert res.returncode != 0 and "Microsoft-VCRedist" in res.stderr + res.stdout
    (b / "licenses" / "Microsoft-VCRedist-REDIST.txt").write_text("terms")
    assert run_script(b, tmp_path / "o").returncode == 0


def test_unknown_dll_fails_naming_it(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "mystery.dll").write_bytes(b"fake")
    res = run_script(b, tmp_path / "o")
    assert res.returncode != 0 and "mystery.dll" in res.stderr + res.stdout


def test_missing_windows_toolchain_part_fails(tmp_path):
    b = fake_builds(tmp_path)
    (b / "toolchain" / "bin" / "lld-link.exe").unlink()
    res = run_script(b, tmp_path / "o")
    assert res.returncode != 0 and "lld-link.exe" in res.stderr + res.stdout


def test_game_data_in_the_libs_stops_the_packaging(tmp_path):
    b = fake_builds(tmp_path)
    (b / "libs" / "resources" / "DATA.AFS").write_bytes(b"x")
    res = run_script(b, tmp_path / "o")
    assert res.returncode != 0 and "game data" in res.stderr + res.stdout


def load_package_windows():
    import importlib.util
    spec = importlib.util.spec_from_file_location("package_windows_under_test", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(SCRIPT.parent))
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.path.remove(str(SCRIPT.parent))
    return mod


def test_collect_dependency_licenses_reads_zip_and_tar_archives(tmp_path, monkeypatch):
    import io
    import tarfile
    pw = load_package_windows()
    src, dest = tmp_path / "src", tmp_path / "dest"
    src.mkdir()
    dest.mkdir()
    with zipfile.ZipFile(src / "one.zip", "w") as z:
        z.writestr("one-1.0/LICENSE", "zip licence")
        z.writestr("one-1.0/other.c", "x")
    with tarfile.open(src / "two.tar.gz", "w:gz") as t:
        for name, data in (("two-2.0/docs/FTL.TXT", b"tar licence"), ("two-2.0/COPYING", b"tar copying")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    monkeypatch.setattr(pw, "DEPENDENCY_LICENSES", {"One": ("one.zip", ("LICENSE",)),
                                                    "Two": ("two.tar.gz", ("COPYING", "docs/FTL.TXT"))})
    pw.collect_dependency_licenses(src, dest)
    assert {p.name: p.read_bytes() for p in dest.iterdir()} == {
        "One-LICENSE": b"zip licence", "Two-COPYING": b"tar copying", "Two-FTL.TXT": b"tar licence"}
    (src / "two.tar.gz").unlink()
    with pytest.raises(SystemExit):
        pw.collect_dependency_licenses(src, dest)
