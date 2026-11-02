#!/usr/bin/env python3
"""Fetches Microsoft's Visual C++ 2015-2022 x64 redistributable and unpacks its runtime DLLs into third_party/vcredist/.

    uv run --no-project python -I tools/pipeline/fetch_vcredist.py [--cache DIR] [--dest DIR]
    uv run --no-project python -I tools/pipeline/fetch_vcredist.py --check

Why: the Windows download ships the C/C++ runtime DLLs its programs import (msvcp140.dll, vcruntime140.dll ...) next to
the exe ("app-local"), so it starts on a Windows 10/11 PC without the redistributable installed (T-770). The files are
Microsoft's, unmodified; the licence text of the same bundle ships with them (tools/pipeline/package_windows.py).

The installer is NEVER run. It is a WiX burn bundle: the PE carries two cabinets after its last section (the installer UI,
and the "attached container" with the packages; measured at 479744 and 686152 for 14.44.35211.0). They are cut out of the file and opened with 7-Zip (`7z`, `7zz` or
`7za` on PATH). The UI cabinet holds the bundle manifest (burn's file `0`), which names every payload with its SHA-1 and
its place; the container holds the MSIs and the cabinets of the amd64 runtime packages, whose members are the DLLs
named `<file>.dll_amd64`. Only those cabinets and the UI's license.rtf are read.

The download URL is Microsoft's content-addressed one (the SHA-256 is part of the path, so it never serves another
build); its SHA-256 and the version are pinned in tools/pipeline/vcredist-windows.sha256. A mismatch is an error and
nothing is extracted. Output (third_party/vcredist/x64/): the *.dll files, license.rtf, VERSION.txt (the pin and the file
version of each DLL). Every DLL is checked to be an x64 PE whose file version is the bundle's, so all come from one
redistributable version.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fetch_toolchain as T  # noqa: E402  (download, sha256_file, find_sevenzip)

ROOT = HERE.parents[1]
PIN_FILE = HERE / "vcredist-windows.sha256"
DEST = ROOT / "third_party" / "vcredist" / "x64"
DEFAULT_CACHE = ROOT / "third_party" / "vcredist" / "cache"
MACHINE_AMD64 = 0x8664
CAB_MAGIC = b"MSCF"
LICENSE_MEMBER = "license.rtf"            # FilePath of the English licence in the bundle manifest
PACKAGE_DIRS = ("vcRuntimeMinimum_amd64", "vcRuntimeAdditional_amd64")   # the x64 packages (the arm64 one is skipped)
MEMBER_SUFFIX = "_amd64"
VERSION_FILE = "VERSION.txt"


def read_pin(pin_file: Path = PIN_FILE) -> dict[str, str]:
    pin = dict(line.split(None, 1) for line in pin_file.read_text(encoding="utf-8").splitlines() if line.strip())
    for key in ("sha256", "version", "name", "url"):
        if key not in pin:
            sys.exit(f"[vcredist] {pin_file.name} has no '{key}' line")
    return {k: v.strip() for k, v in pin.items()}


def pe_overlay_offset(data: bytes) -> int:
    """Where the bytes after the last section's raw data begin (the burn containers)."""
    if data[:2] != b"MZ":
        sys.exit("[vcredist] not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    ends = [sum(struct.unpack_from("<II", data, table + 40 * i + 16)) for i in range(sections)]   # SizeOfRawData + PointerToRawData
    return max(ends)


def split_cabinets(data: bytes) -> list[bytes]:
    """The consecutive cabinets that start at the PE overlay (the size of each is in its header, offset 8)."""
    cabs, at = [], pe_overlay_offset(data)
    while data[at:at + 4] == CAB_MAGIC:
        size = struct.unpack_from("<I", data, at + 8)[0]
        cabs.append(data[at:at + size])
        at = data.find(CAB_MAGIC, at + size)   # burn pads the UX cabinet with 10368 bytes before the container
        if at < 0:
            break
    return cabs


def sevenzip_extract(archive: Path, dest: Path) -> None:
    subprocess.run([T.find_sevenzip(), "x", "-y", "-bso0", "-bsp0", f"-o{dest}", str(archive)], check=True)


def fixed_file_version(data: bytes) -> str:
    """FILEVERSION of a PE (the VS_FIXEDFILEINFO block of its version resource), as a.b.c.d."""
    at = data.find(b"\xbd\x04\xef\xfe")
    if at < 0:
        return ""
    ms, ls = struct.unpack_from("<II", data, at + 8)
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def pe_machine(data: bytes) -> int:
    if data[:2] != b"MZ":
        return -1
    return struct.unpack_from("<H", data, struct.unpack_from("<I", data, 0x3C)[0] + 4)[0]


def payloads(manifest: Path) -> dict[str, tuple[str, str, str]]:
    """The burn manifest's payloads: FilePath (lower case, / separators) -> (SourcePath, SHA-1 or '', Container or '')."""
    root = ET.parse(manifest).getroot()
    found = {}
    for el in root.iter():
        if el.tag.endswith("}Payload") or el.tag == "Payload":
            path = el.get("FilePath", "").replace("\\", "/").lower()
            found[path] = (el.get("SourcePath", ""), (el.get("Hash") or "").lower(), el.get("Container") or "")
    return found


def unpack(archive: Path, dest: Path, scratch: Path, pin: dict[str, str]) -> None:
    data = archive.read_bytes()
    cabs = split_cabinets(data)
    if len(cabs) != 2:
        sys.exit(f"[vcredist] expected 2 cabinets after the installer's sections, found {len(cabs)}")
    folders = []
    for i, cab in enumerate(cabs):
        (scratch / f"c{i}.cab").write_bytes(cab)
        folders.append(scratch / f"x{i}")
        folders[-1].mkdir()
        sevenzip_extract(scratch / f"c{i}.cab", folders[-1])
    ux, container = folders
    found = payloads(ux / "0")
    wanted = {f"packages/{d.lower()}/cab1.cab": d for d in PACKAGE_DIRS}
    dest.mkdir(parents=True)
    for path, folder in wanted.items():
        if path not in found:
            sys.exit(f"[vcredist] the bundle manifest has no payload {path}")
        source, sha1, _ = found[path]
        blob = container / source
        if not blob.is_file() or hashlib.sha1(blob.read_bytes()).hexdigest() != sha1:
            sys.exit(f"[vcredist] {path}: missing or its SHA-1 differs from the bundle manifest")
        sub = scratch / folder
        sub.mkdir()
        sevenzip_extract(blob, sub)
        for member in sorted(sub.iterdir()):
            if member.name.endswith(".dll" + MEMBER_SUFFIX):
                shutil.copyfile(member, dest / member.name[:-len(MEMBER_SUFFIX)])
    license_source, license_sha1, _ = found[LICENSE_MEMBER]
    license_data = (ux / license_source).read_bytes()
    if hashlib.sha1(license_data).hexdigest() != license_sha1:
        sys.exit("[vcredist] license.rtf: its SHA-1 differs from the bundle manifest")
    (dest / "license.rtf").write_bytes(license_data)


def verify_dlls(dest: Path, pin: dict[str, str]) -> list[str]:
    """Every DLL is an x64 PE of the pinned version; returns the VERSION.txt lines."""
    dlls = sorted(dest.glob("*.dll"))
    if not dlls:
        sys.exit(f"[vcredist] no DLL in {dest}")
    lines = []
    for dll in dlls:
        data = dll.read_bytes()
        if pe_machine(data) != MACHINE_AMD64:
            sys.exit(f"[vcredist] {dll.name} is not an x64 PE")
        version = fixed_file_version(data)
        if version != pin["version"]:
            sys.exit(f"[vcredist] {dll.name} has file version {version or '?'}, the pinned redistributable is {pin['version']}")
        lines.append(f"{dll.name} {version} {hashlib.sha256(data).hexdigest()}")
    return lines


def check(dest: Path = DEST) -> int:
    pin = read_pin()
    stamp = dest / VERSION_FILE
    if not stamp.is_file() or f"sha256 {pin['sha256']}" not in stamp.read_text().splitlines():
        print(f"[vcredist] {dest} is missing or from another pin: run fetch_vcredist.py", file=sys.stderr)
        return 1
    verify_dlls(dest, pin)
    if not (dest / "license.rtf").is_file():
        print("[vcredist] license.rtf is missing", file=sys.stderr)
        return 1
    return 0


def fetch(cache: Path, dest: Path = DEST) -> int:
    pin = read_pin()
    if check(dest) == 0:
        print(f"[vcredist] up to date: {dest} (version {pin['version']})")
        return 0
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / pin["name"]
    if not (archive.is_file() and T.sha256_file(archive) == pin["sha256"]):
        print(f"[vcredist] downloading {pin['url']}")
        got = T.download(pin["url"], archive)
        if got != pin["sha256"]:
            archive.unlink()
            sys.exit(f"[vcredist] SHA-256 mismatch: expected {pin['sha256']}, got {got}. Nothing was extracted.")
    staging, scratch = dest.with_name(dest.name + ".tmp"), cache / "unpack"
    for folder in (staging, scratch):
        shutil.rmtree(folder, ignore_errors=True)
    scratch.mkdir()
    try:
        unpack(archive, staging, scratch, pin)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    lines = verify_dlls(staging, pin)
    (staging / VERSION_FILE).write_text(f"sha256 {pin['sha256']}\nversion {pin['version']}\n" + "\n".join(lines) + "\n")
    shutil.rmtree(dest, ignore_errors=True)
    staging.rename(dest)
    print(f"[vcredist] extracted {len(lines)} DLLs of version {pin['version']} to {dest}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE, help="download cache folder")
    parser.add_argument("--dest", type=Path, default=DEST, help="output folder (default third_party/vcredist/x64)")
    parser.add_argument("--check", action="store_true", help="verify the extracted folder only")
    args = parser.parse_args()
    return check(args.dest) if args.check else fetch(args.cache, args.dest)


if __name__ == "__main__":
    sys.exit(main())
