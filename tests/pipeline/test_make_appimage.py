"""make_appimage.py: the AppDir holds the archive's files plus AppRun/.desktop/icon; a wrong tool is refused."""
import hashlib
import io
import sys
import tarfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "pipeline"))
import make_appimage  # noqa: E402


def fake_archive(root: Path, version: str = "abc1234") -> Path:
    archive = root / f"don-linux-{version}.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for name, data in ((f"don-linux-{version}/don", b"fake don"), (f"don-linux-{version}/README.txt", b"readme")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(data))
    return archive


def fake_tool(root: Path) -> tuple[Path, Path]:
    """A stand-in for appimagetool: writes its last argument; and a pin file holding its hash."""
    tool = root / "appimagetool"
    tool.write_text('#!/bin/sh\nfor a; do last="$a"; done\necho fake > "$last"\n')
    tool.chmod(0o755)
    pin = root / "appimagetool.sha256"
    pin.write_text(f"{hashlib.sha256(tool.read_bytes()).hexdigest()}  appimagetool\n")
    return tool, pin


def test_appdir_has_archive_files_and_launcher(tmp_path):
    appdir = make_appimage.make_appdir(fake_archive(tmp_path), tmp_path / "out" / "AppDir")
    assert (appdir / "don").read_bytes() == b"fake don"
    assert (appdir / "README.txt").is_file()
    run = (appdir / "AppRun").read_text()
    assert run.startswith("#!/bin/sh") and 'exec "$HERE/don" "$@"' in run
    assert (appdir / "AppRun").stat().st_mode & 0o111
    assert "Icon=battlestadium-don" in (appdir / "battlestadium-don.desktop").read_text()
    assert (appdir / "battlestadium-don.png").read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
    assert (appdir / ".DirIcon").is_symlink()


def test_make_names_the_appimage_after_the_version(tmp_path, capsys):
    tool, pin = fake_tool(tmp_path)
    dest = make_appimage.make(fake_archive(tmp_path, "5d2b93e"), tmp_path / "out", tool, pin)
    assert dest.name == "BattleStadiumDON-5d2b93e-x86_64.AppImage" and dest.is_file()
    assert "sha256" in capsys.readouterr().out
    assert not (tmp_path / "out" / "appimage-5d2b93e").exists()


def test_tool_not_matching_the_pin_is_refused(tmp_path):
    tool, pin = fake_tool(tmp_path)
    pin.write_text("0" * 64 + "  appimagetool\n")
    with pytest.raises(SystemExit, match="is not the pinned"):
        make_appimage.make(fake_archive(tmp_path), tmp_path / "out", tool, pin)


def test_archive_name_is_checked(tmp_path):
    with pytest.raises(SystemExit, match="don-linux-<version>"):
        make_appimage.version_of(tmp_path / "other.tar.gz")
