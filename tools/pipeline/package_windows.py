#!/usr/bin/env python3
"""Builds the Windows download from the COMMITTED HEAD of this repository: build/player/don-windows-<version>.zip.

The twin of package_linux.py, for the M6 layout (src/host/prepare_cli.h): "Battle Stadium DON.exe", its DLLs
(don_gs.dll, don_vu.dll, don_spu2.dll, SDL3.dll ...) and resources/ beside it, bin/ps2_recomp.exe, toolchain/ (the pinned
LLVM for Windows), sdk/, config/, plus LICENSE, LEGAL.md, THIRD_PARTY_NOTICES.md, licenses/, SOURCE.txt and README.txt
(packaging/windows/). tools/module/stage_install.py lays it out and writes the .zip, printing its SHA-256 and size.

    uv run --no-project python -I tools/pipeline/package_windows.py [--jobs N] [--skip-build] [--version V] [--out DIR]
    uv run --no-project python -I tools/pipeline/package_windows.py --from-builds DIR [--version V] [--out DIR]
    (bash tools/pipeline/package_windows.sh does the same)

--from-builds DIR packages already built pieces without building: DIR/libs/ (don.exe, the DLLs, resources/),
DIR/ps2_recomp.exe, DIR/toolchain/ (as `fetch_toolchain.py --platform windows` leaves it: build/toolchain/windows),
DIR/licenses/ (PS2Recomp-LICENSE, PCSX2-COPYING.GPLv3, and <project>-<file> for each shipped DLL, e.g.
SDL3-LICENSE.txt; the C/C++ runtime DLLs and their licence, Microsoft-VCRedist-license.rtf, are added from --vcredist,
which defaults to third_party/vcredist/x64 as tools/pipeline/fetch_vcredist.py leaves it, pinned in vcredist-windows.sha256;
which DLLs is computed from the import tables of the shipped PEs, and a package that lacks one is refused) and DIR/ps2_runtime_macros.h.

Without --from-builds the script runs the existing cross-build steps (build() below: a detached worktree of HEAD, a
PS2Recomp tree and a PCSX2 tree of their own, clang-cl + lld-link inside the Windows build container through the MAIN
checkout's tools/container/run_windows.sh and its lock) and then packages the result. The game is built WITHOUT the
generated code (-DDON_LINK_GENERATED=OFF): the download holds no game code. ps2_recomp.exe comes from
build/player-build-win/recomp (tools/pipeline/build_tools_windows.sh, T-738) and the compiler from
build/toolchain/windows; the real end-to-end package is the job of the next card.
The folder holds the program only, nothing of the game. DON_JOBS (default 4, at most 6) sets the compile jobs.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("package_linux", HERE / "package_linux.py")
L = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(L)  # type: ignore[union-attr]
ROOT, run, out = L.ROOT, L.run, L.out
sys.path.insert(0, str(ROOT / "tools" / "module"))
import stage_install  # noqa: E402
sys.path.insert(0, str(HERE))
import package_checks  # noqa: E402

# T-519: the name the player sees and double-clicks is stage_install.PLATFORMS["windows"].program; the build keeps
# the target's own name (don.exe).


def log(msg: str) -> None:
    print(f"[package_windows] {msg}", flush=True)


def container(main: Path, script: str, *args, jobs: int):
    """One run of the Windows build container through the main checkout's runner (the shared lock)."""
    env = dict(os.environ, DON_JOBS=str(jobs))
    run(["bash", main / "tools/container/run_windows.sh", "bash", "-c", script, "_", *args], env=env, cwd=main)


def prepare_pcsx2(main: Path, src: Path) -> tuple[Path, str]:
    text = (src / "tools/setup/fetch_pcsx2.sh").read_text()
    match = re.search(r"^PIN=([0-9a-f]{40})", text, re.M)
    if not match:
        sys.exit("cannot read PIN from tools/setup/fetch_pcsx2.sh")
    pin = match.group(1)
    patches = src / "patches/pcsx2"
    names = [n for n in (patches / "series").read_text().splitlines() if n.strip()]
    digest = hashlib.sha256(pin.encode())
    for name in names:
        digest.update(name.encode())
        digest.update((patches / name).read_bytes())
    stamp = digest.hexdigest()
    tree = ROOT / "work" / "player-pcsx2-win"
    stamp_file = tree / ".player-stamp"
    if stamp_file.is_file() and stamp_file.read_text().strip() == stamp:
        log(f"PCSX2 tree up to date ({len(names)} patches)")
        return tree, stamp
    if tree.exists():
        shutil.rmtree(tree)
    log(f"making the PCSX2 tree at {pin[:12]} with {len(names)} patches")
    run(["git", "clone", "-q", "--local", "--no-checkout", (main / "third_party/pcsx2").resolve(), tree])
    run(["git", "-C", tree, "checkout", "-q", pin])
    for name in names:
        patch = patches / name
        if run(["git", "-C", tree, "apply", "--check", patch], check=False, capture_output=True).returncode != 0:
            sys.exit(f"DOES NOT APPLY: {name}")
        run(["git", "-C", tree, "apply", patch])
    stamp_file.write_text(stamp + "\n")
    return tree, stamp


def windows_flags(src: Path, deps: Path, ffmpeg: Path) -> list[str]:
    presets = json.loads((src / "CMakePresets.json").read_text())["configurePresets"]
    preset = next(p for p in presets if p["name"] == "windows-release")
    flags = [f"-D{k}={str(v).replace('${sourceDir}', str(src))}" for k, v in preset["cacheVariables"].items()]
    # the download holds no game code: the module is built on the player's PC (T-696, M7)
    flags = [f for f in flags if not f.startswith(("-DCMAKE_PREFIX_PATH=", "-DDON_LINK_GENERATED="))]
    flags.append("-DDON_LINK_GENERATED=OFF")
    toolchain = src / "cmake/toolchain-windows-clangcl.cmake"
    # the videos: the static FFmpeg of tools/setup/build_ffmpeg_windows.sh (patch 0069 reads PS2X_FFMPEG_PREFIX)
    return [*flags, f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DCMAKE_PREFIX_PATH={deps}",
            "-DDON_ENABLE_FFMPEG=ON", f"-DPS2X_FFMPEG_PREFIX={ffmpeg}"]


# The dependency DLLs come from tools/setup/build_win_deps.sh, which builds them from the source archives it keeps in
# third_party/win-deps-src/ and installs no licence text (T-743): the texts are read from those archives.
# project (the name stage_install.WINDOWS_LIBRARY_PROJECTS expects) -> (archive, licence files inside the top folder)
DEPENDENCY_LICENSES = {
    "SDL3": ("sdl.zip", ("LICENSE.txt",)),
    "zlib": ("zlib132.zip", ("LICENSE",)),
    "libpng": ("lpng1658.zip", ("LICENSE",)),
    "libjpeg-turbo": ("jpeg.tar.gz", ("LICENSE.md", "README.ijg")),
    "lz4": ("lz4.zip", ("LICENSE",)),
    "zstd": ("zstd.zip", ("LICENSE", "COPYING")),
    "libwebp": ("webp.tar.gz", ("COPYING",)),
    "freetype": ("freetype.tar.gz", ("LICENSE.TXT", "docs/FTL.TXT", "docs/GPLv2.TXT")),
    "plutovg": ("plutovg.zip", ("LICENSE",)),
    "plutosvg": ("plutosvg.zip", ("LICENSE",)),
    "rapidyaml": ("ryml.zip", ("LICENSE.txt",)),
    "shaderc": ("shaderc.zip", ("LICENSE",)),
}


def collect_dependency_licenses(sources: Path, dest: Path) -> None:
    """Writes <project>-<file> into dest for every dependency of the Windows DLLs, from the archives in sources."""
    for project, (archive, members) in DEPENDENCY_LICENSES.items():
        path = sources / archive
        if not path.is_file():
            sys.exit(f"missing {path}: the licence text of {project} is read from it (tools/setup/build_win_deps.sh)")
        if archive.endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                top = z.namelist()[0].split("/")[0]
                blobs = {m: z.read(f"{top}/{m}") for m in members}
        else:
            with tarfile.open(path) as t:
                top = t.getnames()[0].split("/")[0]
                blobs = {m: t.extractfile(f"{top}/{m}").read() for m in members}
        for member, data in blobs.items():
            (dest / f"{project}-{Path(member).name}").write_bytes(data)


def build(main: Path, src: Path, runtime: Path, pcsx2: Path, deps: Path, jobs: int) -> dict[str, Path]:
    base = ROOT / "build" / "player-build-win"
    game, gs, spu2, vu = (base / n for n in ("game", "gs", "spu2", "vu"))
    toolchain = f"-DCMAKE_TOOLCHAIN_FILE={src / 'cmake/toolchain-windows-clangcl.cmake'}"
    prefix = f"-DCMAKE_PREFIX_PATH={deps}"
    ffmpeg = main / "third_party/ffmpeg/windows"
    if not any((ffmpeg / "lib").glob("*avcodec*")):
        sys.exit(f"missing the static FFmpeg in {ffmpeg}: run bash tools/setup/build_ffmpeg_windows.sh first (T-305)")
    flags = windows_flags(src, deps, ffmpeg)
    if L.needs_configure(game, runtime, flags):
        container(main, 'cmake -S "$1" -B "$2" -G Ninja "${@:4}" "-DDON_RUNTIME_DIR=$3"', src, game, runtime,
                  *flags, jobs=jobs)
    container(main, 'cmake --build "$1" --parallel "$2" --target don', game, str(jobs), jobs=jobs)
    for name, folder in (("spu2", spu2), ("vu", vu)):
        if not (folder / "build.ninja").is_file():
            container(main, 'cmake -S "$1" -B "$2" -G Ninja "${@:3}"', src / "src" / f"{name}bridge", folder,
                      "-DCMAKE_BUILD_TYPE=Release", toolchain, prefix, f"-DDON_PCSX2_SOURCE_DIR={pcsx2}", jobs=jobs)
        container(main, 'cmake --build "$1" --parallel "$2"', folder, str(jobs), jobs=jobs)
    if not (gs / "build.ninja").is_file():
        container(main, 'cmake -S "$1" -B "$2" -G Ninja "${@:3}"', pcsx2, gs, "-DCMAKE_BUILD_TYPE=Release", toolchain, prefix,
                  "-DENABLE_QT_UI=OFF", "-DENABLE_TESTS=OFF", "-DUSE_VULKAN=ON", "-DUSE_OPENGL=OFF", "-DDISABLE_ADVANCE_SIMD=ON",
                  f"-DDON_GS_BRIDGE_SOURCE_DIR={src / 'src/gsbridge'}", jobs=jobs)
    container(main, 'cmake --build "$1" --parallel "$2" --target don_gs', gs, str(jobs), jobs=jobs)
    return {"game": game, "gs": gs, "spu2": spu2, "vu": vu}


def assemble_libs(dirs: dict[str, Path], deps: Path, commit: str, stamps: str) -> Path:
    """build/player/don-windows-libs/: don.exe, the DLLs, resources/ and BUILT_FROM.txt (what --from-builds takes as DIR/libs)."""
    dest = ROOT / "build" / "player" / "don-windows-libs"
    bridge = dirs["gs"] / "don-gs-bridge"
    need = [dirs["game"] / "don.exe", bridge / "don_gs.dll", bridge / "resources", dirs["vu"] / "don_vu.dll",
            dirs["spu2"] / "don_spu2.dll", deps / "bin" / "SDL3.dll"]
    for path in need:
        if not path.exists():
            sys.exit(f"missing: {path}")
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    shutil.copy2(dirs["game"] / "don.exe", dest / "don.exe")
    for dll in sorted((deps / "bin").glob("*.dll")):  # the DLLs only: bin/ also holds libjpeg's command-line tools
        shutil.copy2(dll, dest)
    for part in (bridge / "don_gs.dll", dirs["vu"] / "don_vu.dll", dirs["spu2"] / "don_spu2.dll"):
        shutil.copy2(part, dest)
    shutil.copytree(bridge / "resources", dest / "resources")
    subject = out(["git", "-C", ROOT, "log", "-1", "--format=%s", commit])
    when = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    (dest / "BUILT_FROM.txt").write_text(f"commit {commit}\nsubject {subject}\nbuilt {when}\nstamps {stamps}\n")
    return dest


def check_no_game_data(stage: Path) -> None:
    bad = [p for p in stage.rglob("*") if p.is_file() and re.search(r"\.(afs|fpk|irx|iso)$|^SLPS_", p.name, re.I)]
    if bad:
        sys.exit(f"game data found in the package: {bad[:3]}")


VCREDIST_DIR = ROOT / "third_party" / "vcredist" / "x64"   # tools/pipeline/fetch_vcredist.py leaves it here
VCREDIST_LICENSE = "Microsoft-VCRedist-license.rtf"


def place_vc_runtime(stage: Path, vcredist: Path, licenses: Path) -> list[str]:
    """Copies, unmodified, the Visual C++ runtime DLLs the staged PEs import (and those DLLs' own imports) beside the
    PE that loads them, and the redistributable's licence text into licenses/. Returns the files placed (T-770).

    Which DLLs is computed from the import tables (package_checks.missing_vc_runtime), not listed here. The UCRT is
    Windows 10+'s own and is not shipped."""
    if package_checks.missing_vc_runtime(stage) and not (vcredist / "VERSION.txt").is_file():
        sys.exit(f"missing {vcredist}: run tools/pipeline/fetch_vcredist.py (the programs import the Visual C++ runtime)")
    placed: list[str] = []
    while True:
        missing = package_checks.missing_vc_runtime(stage)
        if not missing:
            break
        for _importer, name, folder in missing:
            source = next((f for f in vcredist.glob("*.dll") if f.name.lower() == name), None)
            if source is None:
                sys.exit(f"{name} is imported but is not in the pinned redistributable ({vcredist})")
            shutil.copy2(source, stage / folder / source.name)
            if f"{folder}/{source.name}" not in placed:
                placed.append(f"{folder}/{source.name}")
    if placed:
        shutil.copy2(vcredist / "license.rtf", licenses / VCREDIST_LICENSE)
    return placed


def package(stage: Path, args: argparse.Namespace, libs: Path, recomp: Path, toolchain: Path,
            licenses: Path, macros: Path | None, commit: str) -> Path:
    parsed = argparse.Namespace(out=stage, don=libs / "don.exe", libs=libs, recomp=recomp, toolchain=toolchain, repo=ROOT,
                                macros_source=macros, platform="windows")
    stage_install.stage(parsed)
    placed = place_vc_runtime(stage, args.vcredist, licenses)
    log(f"Visual C++ runtime DLLs placed ({len(placed)}): {' '.join(placed)}")
    stage_install.add_documents(stage, ROOT, commit, args.version, licenses, "windows")
    problems = package_checks.check_vc_runtime(stage)
    if problems:
        sys.exit("the package lacks Visual C++ runtime DLLs:\n  " + "\n  ".join(problems))
    check_no_game_data(stage)
    archive = args.out / f"don-windows-{args.version}.zip"
    digest, size = stage_install.write_zip(stage, archive)
    print(f"{archive}\nsha256 {digest}\n{size} bytes")
    return archive


def main_() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--jobs", type=int, default=L.JOBS_DEFAULT)
    parser.add_argument("--skip-build", action="store_true", help="only assemble from the existing build folders")
    parser.add_argument("--from-builds", type=Path, help="package already built pieces (no build); see the module doc")
    parser.add_argument("--vcredist", type=Path, default=VCREDIST_DIR, help="folder fetch_vcredist.py filled")
    parser.add_argument("--version", help="default: git describe --tags --always of HEAD")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "player", help="folder of the .zip")
    args = parser.parse_args()
    commit = out(["git", "-C", ROOT, "rev-parse", "HEAD"])
    args.version = args.version or out(["git", "-C", ROOT, "describe", "--tags", "--always"])
    stage = ROOT / "build" / "player-stage" / f"don-windows-{args.version}"
    if args.from_builds:
        b = args.from_builds
        package(stage, args, b / "libs", b / "ps2_recomp.exe", b / "toolchain", b / "licenses",
                b / "ps2_runtime_macros.h" if (b / "ps2_runtime_macros.h").is_file() else None, commit)
        return 0
    jobs = min(args.jobs, 6)
    main = L.main_checkout()
    start = datetime.datetime.now()
    log(f"HEAD {commit[:12]} {out(['git', '-C', ROOT, 'log', '-1', '--format=%s', commit])[:90]}")
    if out(["git", "-C", ROOT, "status", "--porcelain", "--untracked-files=no"]):
        log("note: the working tree has uncommitted changes; they are NOT in the package (only the commit is)")
    src = L.prepare_source(main, commit, "player-src-win")
    runtime, rstamp = L.prepare_runtime(main, src, "player-ps2recomp-win")
    pcsx2, pstamp = prepare_pcsx2(main, src)
    deps = (main / "third_party/win-deps/clangcl").resolve()
    base = ROOT / "build" / "player-build-win"
    dirs = {n: base / n for n in ("game", "gs", "spu2", "vu")}
    if not args.skip_build:
        dirs = build(main, src, runtime, pcsx2, deps, jobs)
    libs = assemble_libs(dirs, deps, commit, f"ps2recomp:{rstamp[:16]} pcsx2:{pstamp[:16]}")
    recomp = base / "recomp" / "ps2xRecomp" / "ps2_recomp.exe"
    if not recomp.is_file():
        sys.exit(f"missing {recomp}: build ps2_recomp.exe first (tools/pipeline/build_tools_windows.sh, T-738)")
    toolchain = ROOT / "build" / "toolchain" / "windows"
    if not (toolchain / "bin" / "clang++.exe").exists():
        log("fetching the pinned Windows toolchain (tools/pipeline/fetch_toolchain.py --platform windows)")
        run([sys.executable, "-I", ROOT / "tools/pipeline/fetch_toolchain.py", "--platform", "windows"])
    licenses = ROOT / "build" / "player-build" / "licenses-win"
    shutil.rmtree(licenses, ignore_errors=True)
    licenses.mkdir(parents=True)
    shutil.copy2(runtime / "LICENSE", licenses / "PS2Recomp-LICENSE")
    shutil.copy2(pcsx2 / "COPYING.GPLv3", licenses / "PCSX2-COPYING.GPLv3")
    collect_dependency_licenses(deps.parent.parent / "win-deps-src", licenses)
    archive = package(stage, args, libs, recomp, toolchain, licenses,
                      runtime / "ps2xRuntime" / "include" / "ps2_runtime_macros.h", commit)
    secs = int((datetime.datetime.now() - start).total_seconds())
    log(f"built {archive} ({archive.stat().st_size // 1048576} MB) from commit {commit} in {secs // 60}m{secs % 60:02d}s")
    return 0


if __name__ == "__main__":
    sys.exit(main_())
