#!/usr/bin/env python3
"""Lays out a staged install of `don` (the layout is documented in src/host/prepare_cli.h).

  uv run --no-project python -I tools/module/stage_install.py --out DIR --don BUILD/don --libs DIR \
      --recomp BUILD/ps2_recomp --toolchain build/toolchain/linux

--don      the program built with -DDON_LINK_GENERATED=OFF
--libs     a folder laid out as tools/pipeline/package_linux.py leaves build/player/don-linux/: libdon_gs.so,
           libdon_vu.so, libdon_spu2.so, resources/ and the shared libraries they need. Everything but `don` itself
           is copied beside the program (the program finds them through its RUNPATH $ORIGIN).
--recomp   the ps2_recomp executable (static since patch 0090)
--toolchain  build/toolchain/linux as tools/pipeline/fetch_toolchain.py leaves it
--macros-source  third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h (default: under --repo); the derived
           macros header sdk/inc/don_guest_macros.h is made from it by tools/pipeline/gen_shim_macros.py.
No game file is read or written. The output folder is replaced.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MACROS_SOURCE = Path("third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h")


def fail(message: str) -> "None":
    raise SystemExit(f"stage_install: {message}")


def need_file(path: Path, what: str) -> Path:
    if not path.is_file():
        fail(f"{what} not found: {path}")
    return path


def copy_tree(src: Path, dst: Path) -> None:
    shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)


class Platform:
    """What differs between the two downloads: file names, the library suffix and the packaging folder (M7, T-742)."""

    def __init__(self, name: str, program: str, built_program: str, own_libs: tuple[str, ...], recomp: str,
                 toolchain_parts: tuple[str, ...], library_glob: str, projects: dict[str, str]):
        self.name, self.program, self.built_program, self.own_libs = name, program, built_program, own_libs
        self.recomp, self.toolchain_parts, self.library_glob, self.projects = recomp, toolchain_parts, library_glob, projects

    @property
    def packaging(self) -> str:
        return self.name


PLATFORMS = {
    "linux": Platform("linux", "don", "don", ("libdon_gs.so", "libdon_vu.so", "libdon_spu2.so"), "ps2_recomp",
                      ("bin/clang++", "bin/ld.lld", "TOOLCHAIN.sha256"), "*.so*", {}),   # projects: set below
    # The program is the file the player double-clicks; the build calls it don.exe. The libraries sit beside it
    # (the host also looks in gs/ vu/ spu2/, but a flat folder is what the M6 layout of prepare_cli.h allows too).
    "windows": Platform("windows", "Battle Stadium DON.exe", "don.exe", ("don_gs.dll", "don_vu.dll", "don_spu2.dll"),
                        "ps2_recomp.exe", ("bin/clang++.exe", "bin/lld-link.exe", "TOOLCHAIN.sha256"), "*.dll", {}),
}


def platform_of(args_or_name) -> Platform:
    name = args_or_name if isinstance(args_or_name, str) else getattr(args_or_name, "platform", "linux")
    return PLATFORMS[name or "linux"]


def stage(args: argparse.Namespace) -> None:
    plat = platform_of(args)
    out: Path = args.out.resolve()
    don = need_file(args.don, "the program")
    libs: Path = args.libs
    recomp = need_file(args.recomp, plat.recomp)
    toolchain: Path = args.toolchain
    repo: Path = args.repo
    macros_source = args.macros_source or repo / MACROS_SOURCE
    need_file(macros_source, "the runtime macros header")
    for part in plat.toolchain_parts:
        if not (toolchain / part).exists():
            fail(f"the toolchain has no {part}: {toolchain}")
    for lib in plat.own_libs:
        need_file(libs / lib, lib)
    if not (libs / "resources").is_dir():
        fail(f"no resources/ in {libs}")

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    shutil.copy2(don, out / plat.program)
    for entry in sorted(libs.iterdir()):
        if entry.name in (plat.built_program, "BUILT_FROM.txt"):
            continue
        if entry.is_dir():
            copy_tree(entry, out / entry.name)
        else:
            shutil.copy2(entry, out / entry.name, follow_symlinks=False)

    (out / "bin").mkdir()
    shutil.copy2(recomp, out / "bin" / plat.recomp)
    copy_tree(toolchain, out / "toolchain")
    copy_tree(repo / "config", out / "config")

    sdk = out / "sdk"
    copy_tree(repo / "src" / "game_module", sdk / "game_module")
    (sdk / "host").mkdir(parents=True)
    shutil.copy2(repo / "src" / "host" / "game_module.h", sdk / "host" / "game_module.h")
    (sdk / "inc").mkdir()
    subprocess.run([sys.executable, "-I", str(repo / "tools" / "pipeline" / "gen_shim_macros.py"),
                    str(macros_source), str(sdk / "inc" / "don_guest_macros.h")], check=True)
    print(f"stage_install: {out}")


# --- the public package (T-696): documents, notices and the archive ---------------------------------------------------

# licence files the package must carry (name in licenses/)
REQUIRED_LICENSES = ("PS2Recomp-LICENSE", "PCSX2-COPYING.GPLv3")
# Every shared library shipped beside `don`: soname prefix -> project name. The project's licence text must be in
# licenses/ as <project>-<file name> (package_linux.py copies it from the build image's /opt/deps/share); a shipped
# library that is not listed, or whose text is missing, stops the packaging and names the library.
LIBRARY_PROJECTS = {
    "libSDL3": "SDL3", "libjpeg": "libjpeg-turbo", "libturbojpeg": "libjpeg-turbo", "libpng": "libpng",
    "libwebp": "libwebp", "libsharpyuv": "libwebp", "liblz4": "lz4", "libzstd": "zstd", "libplutovg": "plutovg",
    "libplutosvg": "plutosvg", "libshaderc": "shaderc", "libryml": "rapidyaml",
}
# The project's own libraries (their licence is the repository's LICENSE).
OWN_LIBRARIES = ("libdon_gs", "libdon_vu", "libdon_spu2")
# The Windows twin (T-742): the DLLs tools/package_windows/THIRD-PARTY.txt lists (built by tools/setup/build_win_deps.sh),
# matched by lower-case name prefix, plus the Microsoft C/C++ runtime DLLs if a build ships them (their redistribution
# terms are the Visual C++ Redistributable's: put the text in licenses/ as Microsoft-VCRedist-license.rtf).
WINDOWS_LIBRARY_PROJECTS = {
    "sdl3": "SDL3", "z.dll": "zlib", "zlib": "zlib", "libpng": "libpng", "jpeg": "libjpeg-turbo", "turbojpeg": "libjpeg-turbo",
    "lz4": "lz4", "zstd": "zstd", "libwebp": "libwebp", "libsharpyuv": "libwebp", "freetype": "freetype",
    "plutovg": "plutovg", "plutosvg": "plutosvg", "ryml": "rapidyaml", "c4core": "rapidyaml", "shaderc": "shaderc",
    "vcruntime": "Microsoft-VCRedist", "msvcp": "Microsoft-VCRedist", "concrt": "Microsoft-VCRedist",
    "vcomp": "Microsoft-VCRedist", "ucrtbase": "Microsoft-VCRedist", "api-ms-": "Microsoft-VCRedist",
    "libc++": "LLVM", "libunwind": "LLVM",   # llvm-mingw runtime: LLVM's licence is shipped as LLVM-LICENSE.TXT
}
WINDOWS_OWN_LIBRARIES = ("don_gs", "don_vu", "don_spu2")
PLATFORMS["linux"].projects = LIBRARY_PROJECTS
PLATFORMS["windows"].projects = WINDOWS_LIBRARY_PROJECTS
DOCUMENTS = ("LICENSE", "LEGAL.md", "THIRD_PARTY_NOTICES.md")


def add_documents(out: Path, repo: Path, commit: str, version: str, licenses: Path | None,
                  platform: str = "linux") -> None:
    """Adds the licence texts, the notices, SOURCE.txt and README.txt to a staged install."""
    plat = platform_of(platform)
    for name in DOCUMENTS:
        shutil.copy2(need_file(repo / name, name), out / name)
    packaging = repo / "packaging" / plat.packaging
    source = need_file(packaging / "SOURCE.txt.in", "SOURCE.txt.in").read_text()
    (out / "SOURCE.txt").write_text(source.replace("@COMMIT@", commit).replace("@VERSION@", version))
    shutil.copy2(need_file(packaging / "README.txt", "README.txt"), out / "README.txt")
    if plat.name == "windows":   # plain text files a Windows player opens in Notepad
        for name in ("README.txt", "SOURCE.txt"):
            data = (out / name).read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            (out / name).write_bytes(data)

    lic = out / "licenses"
    lic.mkdir()
    shutil.copy2(need_file(out / "toolchain" / "LICENSE.TXT", "the LLVM licence (toolchain/LICENSE.TXT)"),
                 lic / "LLVM-LICENSE.TXT")
    libraries = sorted(f.name for f in out.glob(plat.library_glob))
    for name in REQUIRED_LICENSES:
        if licenses is None or not (licenses / name).is_file():
            fail(f"the licence file {name} is missing (looked in {licenses})")
    if licenses is not None:
        for f in sorted(licenses.iterdir()):
            if f.is_file():
                shutil.copy2(f, lic / f.name)
    for name in libraries:
        own = OWN_LIBRARIES if plat.name == "linux" else WINDOWS_OWN_LIBRARIES
        if name.lower().startswith(own):
            continue
        project = next((v for k, v in plat.projects.items() if name.lower().startswith(k.lower())), None)
        if project is None:
            fail(f"shipped library {name} is not in the {plat.name} library table (tools/module/stage_install.py): "
                 f"add its licence")
        if not any(f.name.startswith(project + "-") for f in lic.iterdir()):
            fail(f"the licence text of {project} (library {name}) is missing in {licenses}; "
                 f"the build image has none under /opt/deps/share/licenses/{project}/ or /opt/deps/share/doc/{project}/")
    (lic / "LIBRARIES.txt").write_text(
        "Shared libraries shipped beside the program. Their licences and sources are listed in\n"
        "THIRD_PARTY_NOTICES.md; the licence text of each is in this folder as <project>-<file>.\n\n"
        + "".join(f"{name}\n" for name in libraries))


def write_archive(out: Path, archive: Path) -> tuple[str, int]:
    """Writes out/ as archive (top folder = the archive's name without .tar.gz), the same bytes for the same files.
    Returns (sha256, size)."""
    top = archive.name.removesuffix(".tar.gz")

    def norm(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
        return info

    archive.parent.mkdir(parents=True, exist_ok=True)
    with archive.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
        tar.add(out, arcname=top, recursive=False, filter=norm)
        for path in sorted(out.rglob("*")):
            tar.add(path, arcname=f"{top}/{path.relative_to(out).as_posix()}", recursive=False, filter=norm)
    return hashlib.sha256(archive.read_bytes()).hexdigest(), archive.stat().st_size


def write_zip(out: Path, archive: Path) -> tuple[str, int]:
    """Writes out/ as a .zip (top folder = the archive's name without .zip), the same bytes for the same files.
    Returns (sha256, size)."""
    top = archive.name.removesuffix(".zip")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in sorted(out.rglob("*")):
            name = f"{top}/{path.relative_to(out).as_posix()}"
            info = zipfile.ZipInfo(name + ("/" if path.is_dir() else ""), date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            if path.is_dir():
                info.external_attr = (0o40755 << 16) | 0x10
                z.writestr(info, b"")
            else:
                info.external_attr = 0o100755 << 16 if path.suffix.lower() == ".exe" else 0o100644 << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, path.read_bytes(), compresslevel=9)
    return hashlib.sha256(archive.read_bytes()).hexdigest(), archive.stat().st_size


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--don", type=Path, required=True)
    p.add_argument("--libs", type=Path, required=True)
    p.add_argument("--recomp", type=Path, required=True)
    p.add_argument("--toolchain", type=Path, required=True)
    p.add_argument("--repo", type=Path, default=REPO)
    p.add_argument("--macros-source", type=Path)
    p.add_argument("--platform", choices=sorted(PLATFORMS), default="linux")
    stage(p.parse_args())


if __name__ == "__main__":
    main()
