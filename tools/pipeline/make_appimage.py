#!/usr/bin/env python3
"""Makes the single-file Linux download, BattleStadiumDON-<version>-x86_64.AppImage, from the Linux archive.

    uv run --no-project python -I tools/pipeline/make_appimage.py build/player/don-linux-<version>.tar.gz [--out DIR]
    (or: tools/pipeline/package_linux.py ... --appimage, which calls it after writing the archive)

The AppImage holds exactly the files of the archive (the program only, nothing of the game), plus:
  AppRun                          starts ./don with the given arguments
  battlestadium-don.desktop/.png  from packaging/linux/ (the icon is original artwork of this project, not the game's)
The AppImage is read-only when mounted; the program writes only to its user folder (~/.local/share/don or
DON_USER_DIR: the prepared game, saves, settings), so nothing else is needed.

The tool: appimagetool (https://github.com/AppImage/appimagetool, "continuous" release, build of git 854e19e), a
download kept in third_party/appimagetool/ and checked against tools/pipeline/appimagetool.sha256 before use.
It runs with APPIMAGE_EXTRACT_AND_RUN=1 (it is an AppImage itself; no FUSE needed).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "third_party" / "appimagetool" / "appimagetool-x86_64.AppImage"
PIN = ROOT / "tools" / "pipeline" / "appimagetool.sha256"
PACKAGING = ROOT / "packaging" / "linux"
APP_ID = "battlestadium-don"
APPRUN = '#!/bin/sh\nHERE="$(dirname "$(readlink -f "$0")")"\nexec "$HERE/don" "$@"\n'


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_tool(tool: Path, pin: Path = PIN) -> None:
    """Fails loudly when the tool is missing or is not the pinned download."""
    want = pin.read_text().split()[0]
    if not tool.is_file():
        sys.exit(f"missing {tool}: download appimagetool-x86_64.AppImage from "
                 f"https://github.com/AppImage/appimagetool/releases (SHA-256 must be {want}, {pin.name})")
    got = sha256(tool)
    if got != want:
        sys.exit(f"{tool}: SHA-256 {got} is not the pinned {want} ({pin})")


def version_of(archive: Path) -> str:
    name = archive.name
    if not (name.startswith("don-linux-") and name.endswith(".tar.gz")):
        sys.exit(f"{archive}: expected don-linux-<version>.tar.gz")
    return name.removeprefix("don-linux-").removesuffix(".tar.gz")


def make_appdir(archive: Path, appdir: Path) -> Path:
    """Unpacks the archive's top folder as appdir and adds AppRun, the .desktop file and the icon."""
    if appdir.exists():
        shutil.rmtree(appdir)
    work = appdir.parent / (appdir.name + ".unpack")
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter="data")
    tops = list(work.iterdir())
    if len(tops) != 1 or not (tops[0] / "don").is_file():
        sys.exit(f"{archive}: expected one top folder holding `don`")
    tops[0].rename(appdir)
    work.rmdir()
    (appdir / "AppRun").write_text(APPRUN)
    (appdir / "AppRun").chmod(0o755)
    shutil.copyfile(PACKAGING / f"{APP_ID}.desktop", appdir / f"{APP_ID}.desktop")
    shutil.copyfile(PACKAGING / f"{APP_ID}.png", appdir / f"{APP_ID}.png")
    (appdir / ".DirIcon").symlink_to(f"{APP_ID}.png")
    return appdir


def make(archive: Path, out: Path, tool: Path = TOOL, pin: Path = PIN) -> Path:
    check_tool(tool, pin)
    version = version_of(archive)
    out.mkdir(parents=True, exist_ok=True)
    appdir = make_appdir(archive, out / f"appimage-{version}" / "AppDir")
    dest = out / f"BattleStadiumDON-{version}-x86_64.AppImage"
    if dest.exists():
        dest.unlink()
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
    subprocess.run([str(tool), "-n", str(appdir), str(dest)], env=env, check=True,
                   stdout=subprocess.DEVNULL, cwd=appdir.parent)
    dest.chmod(0o755)
    shutil.rmtree(appdir.parent)
    print(f"{dest}\nsha256 {sha256(dest)}\n{dest.stat().st_size} bytes")
    return dest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("archive", type=Path, help="don-linux-<version>.tar.gz (tools/pipeline/package_linux.py)")
    parser.add_argument("--out", type=Path, help="folder of the AppImage (default: the archive's folder)")
    args = parser.parse_args()
    make(args.archive.resolve(), (args.out or args.archive.parent).resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
