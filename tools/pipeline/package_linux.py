#!/usr/bin/env python3
"""Builds the Linux download from the COMMITTED HEAD of this repository: build/player/don-linux-<version>.tar.gz.

Why: build/linux-dev/don is rebuilt all day from a working tree that other people are editing (and from the shared
runtime tree third_party/PS2Recomp with unfinished patches). The player folder changes only when this script is run.
It sits beside package_windows.sh and the build_*.sh scripts, and is Python so that a Windows twin can share it.

    uv run --no-project python -I tools/pipeline/package_linux.py [--jobs N] [--skip-build] [--version V] [--out DIR]
    uv run --no-project python -I tools/pipeline/package_linux.py --from-builds DIR [--version V] [--out DIR]
    ... --appimage   also makes BattleStadiumDON-<version>-x86_64.AppImage from the archive (make_appimage.py)

What it does, all under the folder of the checkout that holds this script (called ROOT below):
  1. work/player-src: a detached git worktree of HEAD (never the working tree), with symlinks to the shared
     third_party/ and generated/ (neither is in git).
  2. work/player-ps2recomp: its own PS2Recomp tree: a clone of third_party/PS2Recomp's objects at the commit pinned in
     tools/setup/fetch_ps2recomp.sh, with patches/ps2recomp/series applied in order (the same steps as that script,
     on another folder; the shared tree is only read). It is made again only when the pin, the series or a patch changes.
  3. build/player-build/{game,gs,spu2,vu}: the game with the linux-dev preset's options (read from CMakePresets.json) and
     -DDON_RUNTIME_DIR=<that tree>; the three libraries with the options of build_gs.sh, build_spu2.sh, build_vu.sh,
     from third_party/pcsx2 (read only: its patches/pcsx2 series must be applied, it is checked). Build folders persist,
     so the next run only rebuilds what changed. Everything runs inside the build container through the MAIN
     checkout's tools/container/run.sh (its lock: one container at a time).
  4. build/player/don-linux/: don, libdon_gs.so, libdon_vu.so, libdon_spu2.so, resources/, and every library the
     container's ldd resolves into /opt/deps (SDL3, libjpeg, libwebp ...), all with run path $ORIGIN, plus
     BUILT_FROM.txt (the commit). It checks with ldd that nothing resolves outside the folder or the system.
  5. The download (T-696): the folder of step 4 is the --libs of tools/module/stage_install.py, which lays out the
     staged install (src/host/prepare_cli.h: don, libs, bin/ps2_recomp, toolchain/, sdk/, config/), adds LICENSE,
     LEGAL.md, THIRD_PARTY_NOTICES.md, licenses/, SOURCE.txt (the commit) and README.txt (packaging/linux/), and writes
     the tar.gz, printing its SHA-256 and size. `don` is built with -DDON_LINK_GENERATED=OFF: no game code inside.
     The run path $ORIGIN of every ELF is checked with readelf (check_runpath), beside the ldd check.
--from-builds DIR packages already built pieces without building (and without the ldd and run path checks, which need
real ELF files): DIR/ps2_recomp, DIR/libs/ (as step 4 leaves it, with don), DIR/toolchain/ (as fetch_toolchain.py
leaves it), DIR/licenses/ (PS2Recomp-LICENSE, PCSX2-COPYING.GPLv3, and <project>-<file> for each shipped library, e.g. SDL3-LICENSE.txt) and DIR/ps2_runtime_macros.h.
The folder holds the program only, nothing of the game. DON_JOBS (default 4) sets the compile jobs.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "module"))
sys.path.insert(0, str(ROOT / "tools" / "pipeline"))
import stage_install  # noqa: E402
from package_checks import cached_var, check_path_maps  # noqa: E402
import make_appimage  # noqa: E402
JOBS_DEFAULT = int(os.environ.get("DON_JOBS", "4"))


def run(cmd, **kwargs):
    kwargs.setdefault("check", True)
    return subprocess.run([str(c) for c in cmd], **kwargs)


def out(cmd, **kwargs) -> str:
    return run(cmd, capture_output=True, text=True, **kwargs).stdout.strip()


def log(msg: str) -> None:
    print(f"[package_linux] {msg}", flush=True)


def main_checkout() -> Path:
    """The checkout that owns the container runner and the shared third_party/: where .git lives."""
    common = Path(out(["git", "-C", ROOT, "rev-parse", "--path-format=absolute", "--git-common-dir"]))
    return common.parent


def path_maps(main: Path) -> str:
    """-ffile-prefix-map options that keep this machine's folder names out of the binaries (__FILE__, assert text,
    template names, debug info): the checkout, the shared main checkout and the home folder become fixed names.
    Read by cmake from CFLAGS/CXXFLAGS at the FIRST configure of a build folder only."""
    shared = (main / "third_party").resolve().parent   # third_party/ is a link to another checkout in a worktree
    pairs = [(ROOT, "/don/src"), (main, "/don/main"), (shared, "/don/shared"), (Path.home(), "/home/user")]
    return " ".join(f"-ffile-prefix-map={a}={b}" for a, b in pairs)


def container(main: Path, script: str, *args, jobs: int):
    """One container run through the main checkout's runner (the shared lock); `script` is a bash -c body."""
    env = dict(os.environ, DON_JOBS=str(jobs))
    maps = path_maps(main)
    script = f'export CFLAGS="${{CFLAGS:-}} {maps}" CXXFLAGS="${{CXXFLAGS:-}} {maps}"; {script}'
    cmd = ["bash", main / "tools/container/run.sh", "bash", "-c", script, "_", *args]
    run(cmd, env=env, cwd=main)


def prepare_source(main: Path, commit: str, name: str = "player-src") -> Path:
    src = ROOT / "work" / name
    if not src.exists():
        src.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "-C", ROOT, "worktree", "add", "--detach", src, commit])
    else:
        run(["git", "-C", src, "checkout", "-q", "--detach", "--force", commit])
        run(["git", "-C", src, "clean", "-fdq", "-e", "third_party", "-e", "generated"])
    # third_party and generated are not in git: the committed tree is built against the shared ones.
    for name in ("third_party", "generated"):
        target = (main / name).resolve()
        link = src / name
        if link.is_symlink() or link.exists():
            if link.is_symlink() and link.resolve() == target:
                continue
            link.unlink() if link.is_symlink() else shutil.rmtree(link)
        link.symlink_to(target)
    return src


def fetch_pin(src: Path) -> str:
    text = (src / "tools/setup/fetch_ps2recomp.sh").read_text()
    match = re.search(r"^PIN=([0-9a-f]{40})$", text, re.M)
    if not match:
        sys.exit("cannot read PIN from tools/setup/fetch_ps2recomp.sh")
    return match.group(1)


def prepare_runtime(main: Path, src: Path, folder: str = "player-ps2recomp") -> tuple[Path, str]:
    """A PS2Recomp tree of its own at the pinned commit with the committed series applied."""
    pin = fetch_pin(src)
    patches = src / "patches/ps2recomp"
    names = [n for n in (patches / "series").read_text().splitlines() if n.strip()]
    digest = hashlib.sha256()
    digest.update(pin.encode())
    for name in names:
        digest.update(name.encode())
        digest.update((patches / name).read_bytes())
    stamp = digest.hexdigest()
    tree = ROOT / "work" / folder
    stamp_file = tree / ".player-stamp"
    if stamp_file.is_file() and stamp_file.read_text().strip() == stamp:
        log(f"PS2Recomp tree up to date ({len(names)} patches)")
        return tree, stamp
    if tree.exists():
        shutil.rmtree(tree)
    shared = (main / "third_party/PS2Recomp").resolve()
    log(f"making the PS2Recomp tree at {pin[:12]} with {len(names)} patches")
    run(["git", "clone", "-q", "--local", "--no-hardlinks", "--no-checkout", shared, tree])
    run(["git", "-C", tree, "checkout", "-q", pin])
    for name in names:
        patch = patches / name
        if run(["git", "-C", tree, "apply", "--check", patch], check=False, capture_output=True).returncode != 0:
            sys.exit(f"DOES NOT APPLY: {name}")
        run(["git", "-C", tree, "apply", patch])
    stamp_file.write_text(stamp + "\n")
    return tree, stamp


def check_pcsx2(main: Path, src: Path) -> Path:
    tree = (main / "third_party/pcsx2").resolve()
    for name in [n for n in (src / "patches/pcsx2/series").read_text().splitlines() if n.strip()]:
        patch = src / "patches/pcsx2" / name
        if "clang-cl" in name:  # the Windows cross-build's own fixes: the shared Linux tree does not carry them
            continue
        done = run(["git", "-C", tree, "apply", "--reverse", "--check", patch], check=False, capture_output=True)
        if done.returncode != 0:
            sys.exit(f"third_party/pcsx2 does not have patch {name} applied (bash tools/setup/fetch_pcsx2.sh)")
    return tree


def preset_flags(src: Path, name: str) -> list[str]:
    presets = json.loads((src / "CMakePresets.json").read_text())["configurePresets"]
    preset = next(p for p in presets if p["name"] == name)
    if "inherits" in preset:
        sys.exit("preset with inherits is not handled")
    return [f"-D{k}={v}" for k, v in preset["cacheVariables"].items()]


def needs_configure(build_dir: Path, runtime: Path, flags: list[str] | None = None) -> bool:
    """No build files yet, made against another PS2Recomp tree, or a preset value (-DNAME=value) changed since."""
    if not (build_dir / "build.ninja").is_file() or cached_var(build_dir, "DON_RUNTIME_DIR") != str(runtime):
        return True
    for flag in flags or []:
        name, _, value = flag[2:].partition("=")
        cached = cached_var(build_dir, name)
        if cached is not None and cached != value:
            return True
    return False


def configure_game_flags(src: Path) -> list[str]:
    """The preset's options with the generated code not linked: the download holds no game code (T-696)."""
    flags = [f for f in preset_flags(src, "linux-dev") if not f.startswith("-DDON_LINK_GENERATED=")]
    return [*flags, "-DDON_LINK_GENERATED=OFF"]


def configure_game_cmd(src: Path, game: Path, runtime: Path) -> list[str]:
    """The cmake command that configures `don` for the download (never a preset: --preset would link the game)."""
    return ["cmake", "-S", str(src), "-B", str(game), "-G", "Ninja", *configure_game_flags(src), f"-DDON_RUNTIME_DIR={runtime}"]


def build(main: Path, src: Path, runtime: Path, pcsx2: Path, jobs: int) -> dict[str, Path]:
    base = ROOT / "build" / "player-build"
    game, gs, spu2, vu = (base / n for n in ("game-nolink", "gs", "spu2", "vu"))
    check_path_maps([game, gs, spu2, vu, base / "recomp"], path_maps(main).split())
    flags = configure_game_flags(src)
    # Configure only when needed: Ninja re-runs CMake by itself when a CMake file changes.
    if needs_configure(game, runtime, flags):
        container(main, 'exec "$@"', *configure_game_cmd(src, game, runtime), jobs=jobs)
    container(main, 'cmake --build "$1" -j "$2" --target don', game, str(jobs), jobs=jobs)

    common = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld", "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"]
    if not (gs / "build.ninja").is_file():
        container(main, 'cmake -S "$1" -B "$2" -G Ninja "${@:3}"', pcsx2, gs, *common,
                  "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld", "-DENABLE_QT_UI=OFF", "-DENABLE_TESTS=OFF", "-DUSE_VULKAN=ON",
                  "-DUSE_OPENGL=OFF", "-DX11_API=ON", "-DWAYLAND_API=ON", "-DDISABLE_ADVANCE_SIMD=ON", "-DENABLE_SETCAP=OFF",
                  f"-DDON_GS_BRIDGE_SOURCE_DIR={src / 'src/gsbridge'}", jobs=jobs)
    container(main, 'cmake --build "$1" -j "$2" --target don_gs', gs, str(jobs), jobs=jobs)
    for name, folder, bridge in (("spu2", spu2, "spu2bridge"), ("vu", vu, "vubridge")):
        if not (folder / "build.ninja").is_file():
            container(main, 'cmake -S "$1" -B "$2" -G Ninja "${@:3}"', src / "src" / bridge, folder, *common,
                      f"-DDON_PCSX2_SOURCE_DIR={pcsx2}", jobs=jobs)
        container(main, 'cmake --build "$1" -j "$2" --target "$3"', folder, str(jobs), f"don_{name}", jobs=jobs)
    recomp = base / "recomp"
    if not (recomp / "build.ninja").is_file():
        container(main, 'cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                  '-DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF',   # as build_tools.sh
                  runtime, recomp, jobs=jobs)
    container(main, 'cmake --build "$1" -j "$2" --target ps2_recomp', recomp, str(jobs), jobs=jobs)
    return {"game": game, "gs": gs, "spu2": spu2, "vu": vu, "recomp": recomp}


ASSEMBLE = r'''
set -euo pipefail
dest=$1; game=$2; gs=$3; spu2=$4; vu=$5
rm -rf "$dest.new"; mkdir -p "$dest.new"
cp "$game/don" "$dest.new/don"
cp "$gs/don-gs-bridge/libdon_gs.so" "$spu2/libdon_spu2.so" "$vu/libdon_vu.so" "$dest.new/"
cp -r "$gs/don-gs-bridge/resources" "$dest.new/resources"
# What the image's /opt/deps provides and the host does not: each library the loaded files need, through each other.
for round in 1 2 3; do
  for f in "$dest.new"/don "$dest.new"/*.so "$dest.new"/*.so.*; do
    [ -e "$f" ] || continue
    ldd "$f" | awk '/=> \/opt\/deps\// { print $1, $3 }' | while read -r name path; do
      [ -e "$dest.new/$name" ] || cp -L "$path" "$dest.new/$name"
    done
  done
done
[ -e "$dest.new/libSDL3.so.0" ] || cp -L /opt/deps/lib/libSDL3.so.0.4.12 "$dest.new/libSDL3.so.0"
# libshaderc is not linked: PCSX2 loads it by name with dlopen when it compiles its shaders.
cp -L /opt/deps/lib/libshaderc_shared.so.1 "$dest.new/libshaderc_shared.so.1"
for f in "$dest.new"/don "$dest.new"/*.so "$dest.new"/*.so.*; do
  patchelf --set-rpath '$ORIGIN' "$f"
done
'''


def assemble(main: Path, dirs: dict[str, Path], commit: str, runtime_stamp: str, jobs: int) -> Path:
    dest = ROOT / "build" / "player" / "don-linux"
    dest.parent.mkdir(parents=True, exist_ok=True)
    container(main, ASSEMBLE, dest, dirs["game"], dirs["gs"], dirs["spu2"], dirs["vu"], jobs=jobs)
    new = Path(f"{dest}.new")
    subject = out(["git", "-C", ROOT, "log", "-1", "--format=%s", commit])
    (new / "BUILT_FROM.txt").write_text(
        f"commit {commit}\nsubject {subject}\nbuilt {datetime.datetime.now().astimezone().isoformat(timespec='seconds')}\n"
        f"ps2recomp-series-stamp {runtime_stamp}\n")
    # Replaced as a whole and in one step: a player that is running keeps the old inodes (journal 018).
    old = Path(f"{dest}.old")
    if old.exists():
        shutil.rmtree(old)
    if dest.exists():
        dest.rename(old)
    new.rename(dest)
    if old.exists():
        shutil.rmtree(old)
    return dest


def check_ldd(dest: Path) -> None:
    """Nothing may resolve into a build folder or third_party; anything else must be in the folder or the system."""
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    bad = []
    for f in [dest / "don", *sorted(dest.glob("*.so")), *sorted(dest.glob("*.so.*"))]:
        listing = out(["ldd", f], env=env)
        for line in listing.splitlines():
            if "not found" in line:
                bad.append(f"{f.name}: {line.strip()}")
                continue
            match = re.search(r"=> (/\S+)", line)
            if not match:
                continue
            target = Path(match.group(1)).resolve()
            inside = dest.resolve() in target.parents
            system = str(target).startswith(("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/"))
            if not (inside or system):
                bad.append(f"{f.name}: {line.strip()}")
    if bad:
        sys.exit("ldd: libraries outside the folder and the system:\n  " + "\n  ".join(bad))
    log(f"ldd clean: {len(list(dest.glob('*.so*')))} libraries, none outside the folder or /lib, /usr/lib")


def check_runpath(dest: Path) -> None:
    """Every ELF file of the folder must carry the run path $ORIGIN (readelf -d), or `don` would not find its libraries."""
    bad = []
    for f in sorted(p for p in dest.iterdir() if p.is_file() and p.read_bytes()[:4] == b"\x7fELF"):
        dynamic = out(["readelf", "-d", f])
        if not re.search(r"\((RUNPATH|RPATH)\).*\[\$ORIGIN\]", dynamic):
            bad.append(f.name)
    if bad:
        sys.exit("run path $ORIGIN missing in: " + ", ".join(bad))
    log("run path $ORIGIN present in every ELF file of the folder")


COPY_LICENSES = r'''
dest=$1; shift
for project in "$@"; do
  for dir in /opt/deps/share/licenses/$project /opt/deps/share/doc/$project; do
    [ -d "$dir" ] || continue
    find "$dir" -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'README.ijg' \) |
      while read -r f; do cp -L "$f" "$dest/$project-$(basename "$f")"; done
  done
done
'''


def collect_licenses(main: Path, src: Path, runtime: Path, pcsx2: Path, libs: Path) -> Path:
    """licenses/ sources of a real build: PS2Recomp and PCSX2 from their trees, and for each shipped library its
    project's licence files from the build image's /opt/deps/share (stage_install.add_documents fails naming a
    library whose text is not there)."""
    dest = ROOT / "build" / "player-build" / "licenses"
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    shutil.copy2(runtime / "LICENSE", dest / "PS2Recomp-LICENSE")
    shutil.copy2(pcsx2 / "COPYING.GPLv3", dest / "PCSX2-COPYING.GPLv3")
    projects = sorted({v for f in libs.glob("*.so*") for k, v in stage_install.LIBRARY_PROJECTS.items() if f.name.startswith(k)})
    container(main, COPY_LICENSES, dest, *projects, jobs=1)
    return dest


def package(stage: Path, args: argparse.Namespace, libs: Path, recomp: Path, toolchain: Path,
            licenses: Path, macros: Path | None, commit: str) -> Path:
    parsed = argparse.Namespace(out=stage, don=libs / "don", libs=libs, recomp=recomp, toolchain=toolchain, repo=ROOT,
                                macros_source=macros)
    stage_install.stage(parsed)
    stage_install.add_documents(stage, ROOT, commit, args.version, licenses)
    archive = args.out / f"don-linux-{args.version}.tar.gz"
    digest, size = stage_install.write_archive(stage, archive)
    print(f"{archive}\nsha256 {digest}\n{size} bytes")
    if args.appimage:
        make_appimage.make(archive, args.out)
    return archive


def main_() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--jobs", type=int, default=JOBS_DEFAULT)
    parser.add_argument("--skip-build", action="store_true", help="only assemble from the existing build folders")
    parser.add_argument("--from-builds", type=Path, help="package already built pieces (no build); see the module doc")
    parser.add_argument("--version", help="default: git describe --tags --always of HEAD")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "player", help="folder of the .tar.gz")
    parser.add_argument("--appimage", action="store_true", help="also make the AppImage (tools/pipeline/make_appimage.py)")
    args = parser.parse_args()
    commit = out(["git", "-C", ROOT, "rev-parse", "HEAD"])
    args.version = args.version or out(["git", "-C", ROOT, "describe", "--tags", "--always"])
    stage = ROOT / "build" / "player-stage" / f"don-linux-{args.version}"
    if args.from_builds:
        b = args.from_builds
        package(stage, args, b / "libs", b / "ps2_recomp", b / "toolchain", b / "licenses",
                b / "ps2_runtime_macros.h" if (b / "ps2_runtime_macros.h").is_file() else None, commit)
        return 0
    jobs = min(args.jobs, 6)
    main = main_checkout()
    start = datetime.datetime.now()
    log(f"HEAD {commit[:12]} {out(['git', '-C', ROOT, 'log', '-1', '--format=%s', commit])[:90]}")
    dirty = out(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"])
    if dirty:
        log("note: the working tree has uncommitted changes; they are NOT in the player (only the commit is)")
    src = prepare_source(main, commit)
    os.environ["DON_BUILD_COMMIT"] = commit   # the build id (src/host/CMakeLists.txt); every container run inherits it
    runtime, stamp = prepare_runtime(main, src)
    pcsx2 = check_pcsx2(main, src)
    dirs = build(main, src, runtime, pcsx2, jobs) if not args.skip_build else {
        k: ROOT / "build" / "player-build" / n
        for k, n in (("game", "game-nolink"), ("gs", "gs"), ("spu2", "spu2"), ("vu", "vu"), ("recomp", "recomp"))}
    dest = assemble(main, dirs, commit, stamp, jobs)
    check_ldd(dest)
    check_runpath(dest)
    licenses = collect_licenses(main, src, runtime, pcsx2, dest)
    toolchain = ROOT / "build" / "toolchain" / "linux"
    if not (toolchain / "bin" / "clang++").exists():   # the pinned compiler the player's PC builds the module with
        log("fetching the pinned toolchain (tools/pipeline/fetch_toolchain.py)")
        run([sys.executable, "-I", ROOT / "tools/pipeline/fetch_toolchain.py"])
    archive = package(stage, args, dest, dirs["recomp"] / "ps2xRecomp" / "ps2_recomp", toolchain, licenses,
                      runtime / "ps2xRuntime" / "include" / "ps2_runtime_macros.h", commit)
    secs = int((datetime.datetime.now() - start).total_seconds())
    log(f"built {archive} ({archive.stat().st_size // 1048576} MB) from commit {commit} in {secs // 60}m{secs % 60:02d}s")
    return 0


if __name__ == "__main__":
    sys.exit(main_())
