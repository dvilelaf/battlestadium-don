"""Checks of the build folders that tools/pipeline/package_linux.py reuses. Called by it before a build; not run alone.

cached_var reads a variable of a folder's CMakeCache.txt; check_path_maps stops the build when a configured folder
was made without the current -ffile-prefix-map options (see its docstring).
"""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

# The Visual C++ runtime DLLs (T-770): what the Windows download must ship app-local. The UCRT (api-ms-win-crt-*,
# ucrtbase.dll) is part of Windows 10+ and is deliberately not matched.
VC_RUNTIME = re.compile(r"^(msvcp\d+|vcruntime\d+|concrt\d+|vcomp\d+|vcamp\d+|vccorlib\d+|mfc\d+|mfcm\d+|msvcr\d+|msvcm\d+)"
                        r"[a-z0-9_]*\.dll$", re.I)
DATA_DIRECTORY_IMPORT, DATA_DIRECTORY_DELAY_IMPORT = 1, 13
PE_EXTENSIONS = (".exe", ".dll")
# Folders of a package holding files that are not the program's own: the pinned compiler (it runs from its own folder).
NOT_CHECKED = ("toolchain",)


def cached_var(build_dir: Path, var: str) -> str | None:
    """The value of a variable in a build folder's CMakeCache.txt, or None."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith(f"{var}:"):
            return line.split("=", 1)[1]
    return None


def check_path_maps(folders: list[Path], wanted: list[str]) -> None:
    """Stops when a configured build folder was made without the current -ffile-prefix-map options.

    cmake reads CFLAGS/CXXFLAGS only at a folder's first configure, so a reused folder keeps whatever flags it had:
    one configured before the maps existed (or from another checkout path) would ship this machine's folder names in
    its binaries. Never a silent pass: the folder is named and the fix is to delete it. `wanted` is the list of
    -ffile-prefix-map options of the current checkout."""
    for folder in folders:
        found = [v for v in (cached_var(folder, n) for n in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS")) if v is not None]
        if not (folder / "CMakeCache.txt").is_file():
            continue   # not configured yet: it will be, with the maps
        if not found or any(not set(wanted) <= set(v.split()) for v in found):
            sys.exit(f"build folder {folder} was configured without the current -ffile-prefix-map options "
                     f"(CMAKE_C_FLAGS/CMAKE_CXX_FLAGS in its CMakeCache.txt); it would ship personal paths. "
                     f"Delete it and run again: rm -rf {folder}")


def is_vc_runtime(name: str) -> bool:
    return VC_RUNTIME.match(name) is not None


def pe_imports(data: bytes) -> list[str]:
    """Lower-case DLL names a PE imports (import table, then delay-import table); [] for anything that is not a PE."""
    try:
        return _pe_imports(data)
    except (struct.error, IndexError, ValueError):
        return []


def _pe_imports(data: bytes) -> list[str]:
    if data[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return []
    sections, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    count = struct.unpack_from("<I", data, dirs - 4)[0]
    table = opt + opt_size
    spans = [struct.unpack_from("<IIII", data, table + 40 * i + 8) for i in range(sections)]   # vsize, rva, rawsize, raw

    def offset(rva: int) -> int:
        for vsize, start, raw_size, raw in spans:
            if start <= rva < start + max(vsize, raw_size):
                return raw + rva - start
        raise ValueError(f"RVA {rva:#x} is in no section")

    def name_at(rva: int) -> str:
        at = offset(rva)
        return data[at:data.index(b"\0", at)].decode("ascii", "replace").lower()

    names: list[str] = []
    for index, size, name_field in ((DATA_DIRECTORY_IMPORT, 20, 12), (DATA_DIRECTORY_DELAY_IMPORT, 32, 4)):
        if index >= count:
            continue
        rva, length = struct.unpack_from("<II", data, dirs + 8 * index)
        if not rva or not length:
            continue
        at = offset(rva)
        while data[at:at + size] != bytes(size) and len(data) >= at + size:
            names.append(name_at(struct.unpack_from("<I", data, at + name_field)[0]))
            at += size
    return names


def missing_vc_runtime(root: Path) -> list[tuple[str, str, str]]:
    """(importer, dll, folder) for every Visual C++ runtime DLL a shipped PE imports that is not beside it.

    Windows looks for a DLL first in the folder of the process's exe, so the files must be in the folder of the exe
    that loads them (the program's folder; bin/ for bin/ps2_recomp.exe). Every shipped .exe and .dll is read, the
    runtime DLLs themselves included, so the closure of the imports is covered. The toolchain's own files are not."""
    found = []
    for path in sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in PE_EXTENSIONS):
        rel = path.relative_to(root)
        if rel.parts[0] in NOT_CHECKED:
            continue
        beside = {f.name.lower() for f in path.parent.iterdir()}
        for name in dict.fromkeys(pe_imports(path.read_bytes())):
            if is_vc_runtime(name) and name not in beside:
                found.append((rel.as_posix(), name, rel.parent.as_posix()))
    return found


def check_vc_runtime(root: Path) -> list[str]:
    """One line per missing (importer, dll) pair of missing_vc_runtime; empty when the package is complete."""
    return [f"{importer} imports {name}, which the package does not hold in {folder}/"
            for importer, name, folder in missing_vc_runtime(root)]
