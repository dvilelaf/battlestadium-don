#!/usr/bin/env python3
"""Makes the two personal test bundles (Linux AppImage and Windows zip) in build/test-bundles/.

Why: the owner tests the port on his own machines, and both bundles have to be made the same way every time. The
Windows package alone is not enough (it never ships the game or the Microsoft runtime) and the Linux player folder
alone asks for the game path in argument; a bundle is therefore the program, the runtime DLLs it needs and the owner's
game folder, put together by hand otherwise. No saves: the program keeps them in the player's user folder (decision
D15), and a saves folder in a bundle would be copied there at its first start. It is Python and sits beside package_linux.py and
package_windows.py, so it can be read and changed by anyone.

    uv run --no-project python -I tools/pipeline/make_test_bundles.py [options]

  --game PATH      the game folder (default: DON_VARIANTS_DIR or <data>/don/variants, then en-jpvoice). Its symlinks
                   are followed (the variant points at the disc). mc0, mc1 and .make_variant_dir.json are left out:
                   the memory cards of the folder are a run's own, and the marker file is the maker's.
  --vcredist PATH  the folder with the Microsoft runtime DLLs the Windows bundle must carry (default:
                   work/vcredist/dlls). They have to be x64, as the port is; each one's PE header is checked.
  --output PATH    where the bundles go (default: build/test-bundles). Only build/ may be written inside the
                   checkout: anything else in the git tree is refused.

What it makes under the output folder:

  appdir/                          the AppDir the AppImage is made from
    AppRun, don.desktop, don.png   written here (AppRun: the game, then "$@")
    usr/lib/don/                   build/player/don-linux/ plus libXrandr.so.2 and libXi.so.6 (SDL3 dlopens them)
    usr/share/don/game/            the game that was given
  BattleStadiumDON-test-x86_64.AppImage
  don-windows-test/                build/windows-package/don-windows/ plus the runtime DLLs (also in gs/, vu/,
                                   spu2/), game\\ and a README-TEST.txt for the bundle
  BattleStadiumDON-test-windows.zip
  README-TEST.txt, don.png         the AppImage's own readme, and the icon kept between runs (the icon is not in
                                   the repository: it is looked for there first)

WARNING: both bundles contain the game (its data files). They are for the owner's own
machines: they are never shared, uploaded, sent or added to git (build/ is ignored and the game never goes in it).
At the end each bundle is verified: the AppImage is run headless for 10 s with a temporary HOME and its run is
judged by tools/run/classify_log.py (it must be a normal vsync loop, as in tools/run/smoke.sh), and 7z tests the
zip. Either one fails, and the script stops saying which bundle is not made (rule 2 of AGENTS.md).
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
# The run of the AppImage is judged the way a smoke run is (tools/run/smoke.sh): the same classifier.
_spec = importlib.util.spec_from_file_location("classify_log", HERE.parent / "run/classify_log.py")
CL = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(CL)  # type: ignore[union-attr]
VERIFY_SECONDS = 10
GAME_ELF = "SLPS_256.75"
EXE_NAME = "Battle Stadium DON.exe"          # T-519: the name the Windows package now carries
APPIMAGE_NAME = "BattleStadiumDON-test-x86_64.AppImage"
ZIP_NAME = "BattleStadiumDON-test-windows.zip"
WIN_FOLDER = "don-windows-test"
ICON_NAME = "don.png"
# The game folder of a variant holds these; neither belongs in a bundle (see --game above).
GAME_SKIP = frozenset({"mc0", "mc1", ".make_variant_dir.json"})
VCREDIST_DLLS = ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
# SDL3 loads the X11 libraries by dlopen, so ldd does not list them: the AppImage has to carry them.
X11_LIBS = ("libXrandr.so.2", "libXi.so.6")
IMAGE_FILE_MACHINE_AMD64 = 0x8664

APPRUN = """#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
# T-524: pass only the ELF path. The program itself decides the user folder (memory cards, GS cache,
# log, net.ini) from $XDG_DATA_HOME/don (or DON_USER_DIR) and migrates an old don-test save at start.
exec "$HERE/usr/lib/don/don" "$HERE/usr/share/don/game/SLPS_256.75" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Battle Stadium D.O.N (test)
Exec=AppRun
Icon=don
Categories=Game;
Terminal=false
"""

LINUX_README = """Battle Stadium D.O.N - PERSONAL TEST COPY (Linux, x86-64)
=========================================================

This AppImage CONTAINS THE GAME (the English data, with the original
disc files). It is a personal test copy for its owner. Do not share it,
upload it or send it to anyone.

How to run
----------

1. Make it executable once:

       chmod +x BattleStadiumDON-test-x86_64.AppImage

2. Double-click it in the file manager, or run it from a terminal:

       ./BattleStadiumDON-test-x86_64.AppImage

   If there is no Vulkan driver on the machine, use the software renderer:

       ./BattleStadiumDON-test-x86_64.AppImage --renderer software

3. FUSE note: an AppImage normally mounts itself with FUSE. If the
   system has no FUSE (the AppImage fails with a message about FUSE or
   "libfuse"), extract and run it without FUSE:

       ./BattleStadiumDON-test-x86_64.AppImage --appimage-extract-and-run

Saves
-----

Everything the program keeps is in one folder, ~/.local/share/don (the same
folder for every way of starting the port):

    memcards/    the memory cards (mc0, mc1)
    gs-cache/    the shader and graphics cache
    net.ini      the start screen's settings
    don.log      what the program did: every start is added at the end,
                 under its date and time. Send it with a report of a problem.

A new AppImage keeps all of it. An older test AppImage kept the saves in
~/.local/share/don-test/saves: the first start copies them over.

Keyboard
--------

    Arrows        move
    L             Circle (confirm)
    K             Cross (cancel / jump)
    J             Square (attack)
    I             Triangle
    1 / 3         L2 / R2 (guard)
    Q / E         L1 / R1
    Enter         Start
    Esc           quit

Needs
-----

- A 64-bit Linux system with glibc 2.35 or newer (Ubuntu 22.04, Debian 12,
  Fedora 36 or later and their equivalents).
- A Vulkan driver and loader for the default renderer (for example
  mesa-vulkan-drivers and libvulkan1 on Debian/Ubuntu). Without one, use
  --renderer software.
- An audio server (PipeWire or PulseAudio) for sound.
- No other installation is needed: the AppImage carries its own libraries.
"""

WINDOWS_README = """Battle Stadium D.O.N - PERSONAL TEST COPY (Windows, x64)
========================================================

THIS FOLDER CONTAINS THE GAME (the game\\ folder holds its data files, copied
from your own disc). DO NOT SHARE IT. Do not upload it or send it to anyone.

Nothing needs to be installed. The Microsoft Visual C++ runtime (msvcp140.dll,
vcruntime140.dll, vcruntime140_1.dll) is included next to the program and in
gs\\, vu\\ and spu2\\, as Microsoft allows for redistribution with an
application. A graphics card with a Vulkan driver is needed (any recent NVIDIA,
AMD or Intel driver).

1. Start
--------
Unzip the folder anywhere, then double-click "Battle Stadium DON.exe". It asks
nothing and no window of text opens. The game's files are in game\\, next to
the program.

Your saves, settings and log are not in this folder but in %APPDATA%\\don
(type it in the Explorer's address bar): memcards\\ (your memory cards),
gs-cache\\, net.ini and don.log (what the program did; every start is added at
the end, under its date and time: send it with a report). A new version
unzipped anywhere keeps them. An older version kept the saves in a saves\\
folder beside the program: unzip this one over the old folder (or copy that
saves\\ folder beside this program) and the first start copies them over.

Windows will very likely show a blue box the first time: "Windows protected your
PC". That is only because this program is not signed with a paid certificate;
click "More info" and then "Run anyway".

If the graphics fail (black window, a crash at start), open a command prompt in
this folder and run:

   "Battle Stadium DON.exe" --renderer software

Other options: --sound off, --upscale 2, --players 4, --seconds 60.

2. Keys
-------
   Arrow keys      move (D-pad)
   L               Circle: confirm
   K               Cross: cancel / jump
   J               Square: attack
   I               Triangle
   1 / 3           L2 / R2: guard
   Q / E           L1 / R1
   Enter           Start
   Esc             quit (or close the window)

3. Contents
-----------
   Battle Stadium DON.exe   the program
   *.dll            its libraries, and the three Microsoft runtime DLLs
   game\\            the game's data files (your copy; the one with
                    SLPS_256.75)
   gs\\ vu\\ spu2\\    the graphics, vector unit and sound libraries (the
                    Microsoft DLLs are inside these too)
   BUILT_FROM.txt   the commit this folder was built from
"""


def run(cmd, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run([str(c) for c in cmd], **kwargs)


def out(cmd, **kwargs) -> str:
    return run(cmd, capture_output=True, text=True, **kwargs).stdout.strip()


def log(msg: str) -> None:
    print(f"[make_test_bundles] {msg}", flush=True)


def check_output_path(dest: Path) -> Path:
    """Refuse to write inside the checkout except under build/: the bundles carry the game (rule 1)."""
    dest = dest.expanduser().resolve()
    top = Path(out(["git", "-C", ROOT, "rev-parse", "--show-toplevel"])).resolve()
    build = top / "build"
    if dest == top or top in dest.parents:
        if dest != build and build not in dest.parents:
            sys.exit(f"refusing to write {dest}: inside the checkout only build/ may hold the bundles")
    return dest


def data_dir() -> Path:
    """<data> as the launcher defines it: XDG_DATA_HOME or ~/.local/share, or %APPDATA% on Windows."""
    if os.name == "nt":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
    else:
        base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "don"


def default_game() -> Path:
    return Path(os.environ.get("DON_VARIANTS_DIR") or data_dir() / "variants") / "en-jpvoice"


def copy_tree(src: Path, dst: Path, skip_names=frozenset()) -> None:
    """Copy a folder following symlinks (a variant folder points at the disc and at its own files)."""
    dst.mkdir(parents=True, exist_ok=True)
    for entry in sorted(src.iterdir()):
        if entry.name in skip_names:
            continue
        target = dst / entry.name
        if entry.is_dir():                      # follows a symlink to a folder
            shutil.copytree(entry, target, symlinks=False, dirs_exist_ok=True)
        else:
            shutil.copy2(entry, target)         # follows a symlink to a file
    shutil.copystat(src, dst)


def system_library(name: str) -> Path:
    """Where the system keeps a soname (ldconfig -p); only the 64-bit one is taken."""
    for line in out(["ldconfig", "-p"]).splitlines():
        left, sep, right = line.partition("=>")
        words = left.split()
        if not sep or not words or words[0] != name or "x86-64" not in left:
            continue
        path = Path(right.strip())
        if path.is_file():
            return path
    sys.exit(f"{name} is not on this system (ldconfig -p); the AppImage needs it")


def pe_machine(path: Path) -> int:
    """The Machine field of a PE file, or 0 when it is not one."""
    head = path.read_bytes()[:0x1000]
    if head[:2] != b"MZ":
        return 0
    offset = int.from_bytes(head[0x3C:0x40], "little")
    if head[offset:offset + 4] != b"PE\0\0":
        return 0
    return int.from_bytes(head[offset + 4:offset + 6], "little")


def write_placeholder_png(path: Path, size: int = 256) -> None:
    """A plain blue square, only when no icon is found: the icon is not in the repository (no dependency needed)."""
    row = b"\x00" + bytes((0x1A, 0x36, 0x8F)) * size
    raw = zlib.compress(row * size, 9)

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def icon(bundle: Path) -> Path:
    """The icon kept beside the bundles (build/ is not in git): the one already there, else the appdir's, else a square."""
    kept = bundle / ICON_NAME
    if kept.is_file():
        return kept
    for old in (bundle / "appdir" / ICON_NAME, bundle / "appdir" / "usr/share/don" / ICON_NAME):
        if old.is_file():
            shutil.copy2(old, kept)
            return kept
    write_placeholder_png(kept)
    log(f"no icon found: wrote a plain placeholder {kept} (put your own {ICON_NAME} next to the bundles)")
    return kept


def make_appdir(bundle: Path, player: Path, game: Path, icon_path: Path) -> Path:
    appdir = bundle / "appdir"
    if appdir.exists():
        shutil.rmtree(appdir)
    if not (player / "don").is_file():
        sys.exit(f"missing {player / 'don'}: run tools/pipeline/package_linux.py first")
    lib = appdir / "usr/lib/don"
    shutil.copytree(player, lib, symlinks=False, dirs_exist_ok=True,
                    ignore=lambda _, names: [n for n in names if n == "data"])
    # data/ is what the program itself writes (PCSX2's cache, cards, logs) when run from this folder: not the program.
    if (player / "data").exists():
        log("not copied: build/player/don-linux/data/ (the program's own cache, made by running it)")
    for name in X11_LIBS:
        source = system_library(name)
        shutil.copy2(source, lib / name)
        log(f"added {name} from {source}")
    share = appdir / "usr/share/don"
    copy_tree(game, share / "game", GAME_SKIP)
    shutil.copy2(icon_path, share / ICON_NAME)
    shutil.copy2(icon_path, appdir / ICON_NAME)
    link = appdir / ".DirIcon"
    if link.exists() or link.is_symlink():
        link.unlink()
    link.symlink_to(ICON_NAME)
    (appdir / "AppRun").write_text(APPRUN)
    (appdir / "AppRun").chmod(0o755)
    (appdir / "don.desktop").write_text(DESKTOP)
    (bundle / "README-TEST.txt").write_text(LINUX_README)
    return appdir


def make_appimage(bundle: Path, appdir: Path) -> Path:
    tool = ROOT / "third_party/appimagetool/appimagetool-x86_64.AppImage"
    if not tool.is_file():
        sys.exit(f"missing {tool}: it is downloaded by tools/setup (see docs/knowledge/09-environment-and-tools.md)")
    dest = bundle / APPIMAGE_NAME
    if dest.exists():
        dest.unlink()
    env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")  # the tool is an AppImage itself: no FUSE
    run([tool, "-n", appdir, dest], env=env, cwd=bundle)
    dest.chmod(0o755)
    return dest


def make_windows(bundle: Path, package: Path, game: Path, vcredist: Path) -> Path:
    folder = bundle / WIN_FOLDER
    if folder.exists():
        shutil.rmtree(folder)
    if not (package / EXE_NAME).is_file():
        sys.exit(f"missing {package / EXE_NAME}: run tools/pipeline/package_windows.py first "
                 "(an older package carries don.exe and a start.bat)")
    shutil.copytree(package, folder, symlinks=False)
    dlls = []
    for name in VCREDIST_DLLS:
        dll = vcredist / name
        if not dll.is_file():
            sys.exit(f"missing {dll} (--vcredist)")
        machine = pe_machine(dll)
        if machine != IMAGE_FILE_MACHINE_AMD64:
            sys.exit(f"{dll} is not x64 (PE machine 0x{machine:04x}, want 0x8664)")
        dlls.append(dll)
    subfolders = [sub_dir for sub_dir in (folder / n for n in ("gs", "vu", "spu2")) if sub_dir.is_dir()]
    for dll in dlls:
        shutil.copy2(dll, folder / dll.name)
        for sub_dir in subfolders:              # each folder holds a DLL that needs the same runtime
            shutil.copy2(dll, sub_dir / dll.name)
    copy_tree(game, folder / "game", GAME_SKIP)
    readme = folder / "README-TEST.txt"
    readme.write_text(WINDOWS_README)
    readme.write_bytes(readme.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))  # Windows wants CRLF
    return folder


def make_zip(bundle: Path, folder: Path) -> Path:
    dest = bundle / ZIP_NAME
    if dest.exists():
        dest.unlink()
    # The game's data does not compress (Store, as the hand-made bundle did), and this keeps the zip quick to make.
    run(["7z", "a", "-tzip", "-mx=0", "-bso0", "-bsp0", dest.name, folder.name], cwd=bundle)
    return dest


def _no_core_dump():
    """A core dump would contain the game's memory: it must never be written (see tools/run/smoke.sh)."""
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except Exception:
        pass


def verify_appimage(bundle: Path, appimage: Path) -> str:
    """Run it headless for VERIFY_SECONDS with a temporary HOME, and judge the run like a smoke run."""
    home = Path(tempfile.mkdtemp(prefix="don-test-home-"))
    log_file = bundle / "appimage-verify.log"
    try:
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_DATA_HOME")}
        env["HOME"] = str(home)
        env["APPIMAGE_EXTRACT_AND_RUN"] = "1"   # no FUSE needed, as in the readme
        env["DON_TTY"] = "1"                    # the game's text and the frame trace, so the log can be judged
        env["DON_TRACE_FRAMES"] = "1"
        argv = [str(appimage), "--headless", "--renderer", "software", "--sound", "off",
                "--seconds", str(VERIFY_SECONDS)]
        log(f"running the AppImage headless for {VERIFY_SECONDS} s (HOME={home})")
        done = run(argv, capture_output=True, text=True, env=env, cwd=home, timeout=600,
                   check=False, preexec_fn=_no_core_dump)
        # The program's messages reach the pipes as well as its own <user>/don.log (a tee): the pipes are judged,
        # and the log has to exist, with this run's dated block (D15).
        text = done.stdout + done.stderr
        log_file.write_text(text)
        app_log = home / ".local/share/don/don.log"
        if not app_log.is_file() or "===== Battle Stadium D.O.N started" not in app_log.read_text(errors="replace"):
            sys.exit(f"the AppImage wrote no dated don.log in its user folder ({app_log})")
        saved = sorted(p for p in (home / ".local/share/don").rglob("*") if p.is_file())
        category, detail = CL.classify(text)
        if done.returncode != 0:
            sys.exit(f"the AppImage ended with {done.returncode} (want 0 after --seconds {VERIFY_SECONDS}); "
                     f"its output is in {log_file}")
        if category != "OK_VSYNC_LOOP":
            sys.exit(f"the AppImage run is not OK_VSYNC_LOOP but {category} {detail} (tools/run/classify_log.py); "
                     f"its output is in {log_file}")
        if not saved:
            sys.exit(f"the AppImage ran but wrote nothing under its temporary HOME ({home}); log {log_file}")
        return f"{category} {detail}, {len(saved)} files under its temporary HOME, log {log_file.name}"
    finally:
        shutil.rmtree(home, ignore_errors=True)


def verify_zip(zip_path: Path) -> str:
    log(f"testing {zip_path.name} with 7z ({zip_path.stat().st_size // 1048576} MB)")
    done = run(["7z", "t", zip_path], capture_output=True, text=True, check=False)
    text = done.stdout + done.stderr
    if done.returncode != 0 or "Everything is Ok" not in text:
        sys.exit("7z t failed:\n" + "\n".join(text.strip().splitlines()[-6:]))
    listing = out(["7z", "l", "-ba", zip_path])
    want = [f"{WIN_FOLDER}/game/{GAME_ELF}", f"{WIN_FOLDER}/{EXE_NAME}"]
    missing = [name for name in want if name not in listing]
    if missing:
        sys.exit(f"the zip does not hold {missing}")
    return "7z t: " + " ".join(text.strip().splitlines()[-1].split())


def human(size: int) -> str:
    return f"{size // 1048576} MB"


def main_() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--game", type=Path, default=default_game(),
                        help="the game folder (symlinks followed; mc0, mc1 and .make_variant_dir.json left out)")
    parser.add_argument("--vcredist", type=Path, default=ROOT / "work/vcredist/dlls",
                        help="the x64 Microsoft runtime DLLs the Windows bundle carries")
    parser.add_argument("--output", type=Path, default=ROOT / "build/test-bundles",
                        help="where the bundles go (only build/ may be written inside the checkout)")
    args = parser.parse_args()

    bundle = check_output_path(args.output)
    player = ROOT / "build/player/don-linux"
    package = ROOT / "build/windows-package/don-windows"
    log("WARNING: both bundles contain the game (its data files): for the owner's own "
        "machines only, never shared, uploaded or sent to anyone, and never added to git")
    for name, path in (("game", args.game), ("vcredist", args.vcredist),
                       ("player", player), ("Windows package", package)):
        if not path.is_dir():
            sys.exit(f"missing {name} folder: {path}")
    if not (args.game / GAME_ELF).is_file():
        sys.exit(f"{args.game} does not hold {GAME_ELF}: that is not a game folder")
    bundle.mkdir(parents=True, exist_ok=True)
    log(f"game {args.game}, output {bundle}")

    icon_path = icon(bundle)
    appdir = make_appdir(bundle, player, args.game, icon_path)
    game_size = sum(f.stat().st_size for f in (appdir / "usr/share/don/game").rglob("*") if f.is_file())
    log(f"appdir ready ({human(game_size)} of game), making the AppImage with appimagetool")
    appimage = make_appimage(bundle, appdir)
    log(f"{appimage.name}: {human(appimage.stat().st_size)}")
    log(f"the AppImage's run: {verify_appimage(bundle, appimage)}")

    folder = make_windows(bundle, package, args.game, args.vcredist)
    zip_path = make_zip(bundle, folder)
    log(f"{zip_path.name}: {human(zip_path.stat().st_size)} -> {verify_zip(zip_path)}")

    log("WARNING: both bundles hold the game: keep them on your own machines, do not share them")
    for path in sorted(bundle.iterdir()):
        size = path.stat().st_size if path.is_file() else sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
        print(f"{size // 1048576:>6} MB  {path.name}{'/' if path.is_dir() else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main_())
