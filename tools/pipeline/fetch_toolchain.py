#!/usr/bin/env python3
"""Fetches the pinned LLVM 17.0.6 clang and lld for Linux x86_64 into build/toolchain/linux/.

    uv run --no-project python -I tools/pipeline/fetch_toolchain.py [--cache DIR]
    uv run --no-project python -I tools/pipeline/fetch_toolchain.py --check

Why: the game module is compiled with a pinned, self-contained clang, so that it builds on the player's PC without
Docker (A1 in docs/tasks/M6-public-release.md). The archive is the official release from the llvm-project GitHub
release llvmorg-17.0.6. Its SHA-256 is pinned in tools/pipeline/toolchain.sha256; on the first download the hash is
recorded there. The pin is trust-on-first-use: LLVM signs only the source tarballs of this release (no `.sig` for the
binary archive), and GitHub's release API gives no digest for any of its 55 assets (both measured 2026-10-25), so the
hash has no second source to be checked against.

What it does:
  1. Downloads the archive into the cache folder (default build/toolchain/cache/, one attempt) or reuses it when its
     hash matches the pin. A mismatch is an error: nothing is extracted.
  2. Extracts only the members the module build needs, keeping the symlinks: bin/clang-17, bin/lld, the symlinks
     bin/clang, bin/clang++ and bin/ld.lld, lib/clang/17/include/ and, from the source tag, the full LLVM license (see LICENSE_URL below).
  3. Writes build/toolchain/linux/TOOLCHAIN.sha256: one SHA-256 over the sorted list of the tree's files and symlinks
     ("F <path> <sha256 of content>" or "L <path> <sha256 of the link target text>", one per line).

--check re-computes that tree hash without downloading anything and exits non-zero on a missing tree or a mismatch.

--platform windows does the same for the Windows module build (M7): it downloads the official LLVM-17.0.6-win64.exe
(an NSIS installer; the release has no zip or tar asset for win64, measured 2026-10-27), pinned in
tools/pipeline/toolchain-windows.sha256 (trust on first use, same reason as Linux), unpacks it with 7-Zip (`7z`, `7zz`
or `7za` on PATH, e.g. the build container's) and keeps only bin/clang++.exe, bin/lld-link.exe,
lib/clang/17/include/ and the licence, into build/toolchain/windows/ with the same tree hash file.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

LLVM_VERSION = "17.0.6"
ARCHIVE_NAME = f"clang+llvm-{LLVM_VERSION}-x86_64-linux-gnu-ubuntu-22.04.tar.xz"
RELEASE_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/"
ARCHIVE_URL = RELEASE_URL + ARCHIVE_NAME
ARCHIVE_PREFIX = f"clang+llvm-{LLVM_VERSION}-x86_64-linux-gnu-ubuntu-22.04/"
WINDOWS_ARCHIVE_NAME = f"LLVM-{LLVM_VERSION}-win64.exe"
WINDOWS_REAL_FILES = ("bin/clang++.exe", "bin/lld-link.exe")
SEVENZIP_NAMES = ("7z", "7zz", "7za")

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PIN_FILE = HERE / "toolchain.sha256"
WINDOWS_PIN_FILE = HERE / "toolchain-windows.sha256"
TOOLCHAIN_DIR = ROOT / "build" / "toolchain" / "linux"
WINDOWS_TOOLCHAIN_DIR = ROOT / "build" / "toolchain" / "windows"
TREE_HASH_FILE = "TOOLCHAIN.sha256"
DEFAULT_CACHE = ROOT / "build" / "toolchain" / "cache"

REAL_FILES = ("bin/clang-17", "bin/lld")
SYMLINKS = ("bin/clang", "bin/clang++", "bin/ld.lld")
RESOURCE_DIR = f"lib/clang/{LLVM_VERSION.split('.')[0]}/include/"
# The archive has no LICENSE or NOTICE at its top level (measured 2026-10-26: its only licence-like member is
# include/llvm/Support/LICENSE.TXT, a six-line "System Interface Library" notice that points to llvm.org). The full
# "Apache License v2.0 with LLVM Exceptions" text is llvm/LICENSE.TXT of the source tag; it is downloaded, checked
# against the SHA-256 below (measured on that tag, 279 lines) and shipped as LICENSE.TXT.
LICENSE_URL = f"https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{LLVM_VERSION}/llvm/LICENSE.TXT"
LICENSE_SHA256 = "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"
LICENSE_DEST = "LICENSE.TXT"
REQUIRED_OUTPUT = (*REAL_FILES, *SYMLINKS, f"{RESOURCE_DIR}stddef.h")
CHUNK = 1 << 20


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_pin(pin_file: Path = PIN_FILE, name: str = ARCHIVE_NAME) -> str:
    """Returns the pinned archive hash, or an empty string when not yet recorded."""
    if not pin_file.exists():
        return ""
    for line in pin_file.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == name:
            return parts[0]
    return ""


def write_pin(digest: str, pin_file: Path = PIN_FILE, name: str = ARCHIVE_NAME) -> None:
    pin_file.write_text(f"{digest}  {name}\n", encoding="utf-8")


def download(url: str, target: Path) -> str:
    """Downloads url into target through a .part file; returns its SHA-256."""
    partial = target.with_name(target.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "battlestadium-don-fetch-toolchain"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as out:
        for chunk in iter(lambda: response.read(CHUNK), b""):
            digest.update(chunk)
            out.write(chunk)
    partial.replace(target)
    return digest.hexdigest()


def ensure_archive(cache: Path, name: str = ARCHIVE_NAME, pin_file: Path = PIN_FILE) -> Path:
    """Returns the verified archive path, downloading it when the cache has no good copy."""
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    url = RELEASE_URL + name
    pinned = read_pin(pin_file, name)
    if archive.exists() and pinned and sha256_file(archive) == pinned:
        print(f"[toolchain] cache hit: {archive}")
        return archive
    print(f"[toolchain] downloading {url}")
    digest = download(url, archive)
    if not pinned:
        write_pin(digest, pin_file, name)
        print(f"[toolchain] recorded the SHA-256 in {pin_file.name}: {digest}")
    elif digest != pinned:
        archive.unlink()
        sys.exit(f"[toolchain] SHA-256 mismatch: expected {pinned}, got {digest}. Nothing was extracted.")
    return archive


def wanted(rel: str) -> bool:
    return rel in REAL_FILES or rel in SYMLINKS or rel.startswith(RESOURCE_DIR)


def extract(archive: Path, dest: Path) -> None:
    """Extracts the wanted members of the archive into dest, keeping symlinks as relative links."""
    with tarfile.open(archive, "r:xz") as tar:
        for member in tar:
            if not member.name.startswith(ARCHIVE_PREFIX):
                continue
            rel = member.name[len(ARCHIVE_PREFIX):].rstrip("/")
            if not rel or not wanted(rel):
                continue
            target = dest / rel
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym():
                link = Path(member.linkname)
                if link.is_absolute() or ".." in link.parts:
                    sys.exit(f"[toolchain] refusing link outside the tree: {rel} -> {member.linkname}")
                os.symlink(member.linkname, target)
            elif member.isfile():
                source = tar.extractfile(member)
                with target.open("wb") as out:
                    shutil.copyfileobj(source, out)
                os.chmod(target, 0o755 if member.mode & 0o111 else 0o644)
            else:
                sys.exit(f"[toolchain] unexpected member type: {rel}")
    missing = [name for name in REQUIRED_OUTPUT if not (dest / name).exists() and not (dest / name).is_symlink()]
    if missing:
        sys.exit(f"[toolchain] the archive lacks: {', '.join(missing)}")


def tree_hash(root: Path) -> str:
    """One SHA-256 over the sorted files and symlinks of root (the tree hash file itself excluded)."""
    lines = []
    for path in sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix()):
        rel = path.relative_to(root).as_posix()
        if rel == TREE_HASH_FILE:
            continue
        if path.is_symlink():
            lines.append(f"L {rel} {hashlib.sha256(os.readlink(path).encode('utf-8')).hexdigest()}\n")
        elif path.is_file():
            lines.append(f"F {rel} {sha256_file(path)}\n")
    return hashlib.sha256("".join(lines).encode("utf-8")).hexdigest()


def check(toolchain_dir: Path = TOOLCHAIN_DIR, entry: str = "bin/clang") -> int:
    stored_file = toolchain_dir / TREE_HASH_FILE
    if not stored_file.is_file():
        print(f"[toolchain] missing {stored_file}; run this script without --check")
        return 1
    stored = stored_file.read_text(encoding="utf-8").split()[0]
    actual = tree_hash(toolchain_dir)
    if actual != stored:
        print(f"[toolchain] tree hash mismatch: stored {stored}, actual {actual}")
        return 1
    if not (toolchain_dir / entry).exists():
        print(f"[toolchain] {entry} does not resolve")
        return 1
    print(f"[toolchain] OK tree {actual}")
    return 0


def fetch_license(cache: Path, target: Path) -> None:
    """Puts the full LLVM licence text at target, from the cache or the pinned URL; a wrong hash is an error."""
    cached = cache / f"LLVM-{LLVM_VERSION}-LICENSE.TXT"
    if not (cached.is_file() and sha256_file(cached) == LICENSE_SHA256):
        print(f"[toolchain] downloading {LICENSE_URL}")
        got = download(LICENSE_URL, cached)
        if got != LICENSE_SHA256:
            cached.unlink()
            sys.exit(f"[toolchain] LLVM licence SHA-256 mismatch: expected {LICENSE_SHA256}, got {got}")
    shutil.copyfile(cached, target)


def fetch(cache: Path) -> int:
    archive = ensure_archive(cache)
    staging = TOOLCHAIN_DIR.with_name(TOOLCHAIN_DIR.name + ".tmp")
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    extract(archive, staging)
    fetch_license(cache, staging / LICENSE_DEST)
    digest = tree_hash(staging)
    (staging / TREE_HASH_FILE).write_text(f"{digest}  build/toolchain/linux (tree)\n", encoding="utf-8")
    if TOOLCHAIN_DIR.exists():
        shutil.rmtree(TOOLCHAIN_DIR)
    staging.rename(TOOLCHAIN_DIR)
    print(f"[toolchain] extracted to {TOOLCHAIN_DIR} (tree {digest})")
    return 0


def find_sevenzip() -> str:
    for name in SEVENZIP_NAMES:
        found = shutil.which(name)
        if found:
            return found
    sys.exit(f"[toolchain] 7-Zip not found on PATH (tried {', '.join(SEVENZIP_NAMES)}); it unpacks the NSIS installer")


def extract_windows(archive: Path, dest: Path, scratch: Path) -> None:
    """Unpacks the wanted members of the NSIS installer with 7-Zip and copies them into dest."""
    wanted_args = [*WINDOWS_REAL_FILES, RESOURCE_DIR + "*"]
    subprocess.run([find_sevenzip(), "x", "-y", "-bso0", "-bsp0", f"-o{scratch}", str(archive), *wanted_args], check=True)
    for rel in WINDOWS_REAL_FILES:
        source = scratch / rel
        if not source.is_file():
            sys.exit(f"[toolchain] the installer lacks: {rel}")
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest / rel)
    shutil.copytree(scratch / RESOURCE_DIR.rstrip("/"), dest / RESOURCE_DIR.rstrip("/"))
    if not (dest / RESOURCE_DIR / "stddef.h").is_file():
        sys.exit(f"[toolchain] the installer lacks: {RESOURCE_DIR}stddef.h")


def fetch_windows(cache: Path) -> int:
    archive = ensure_archive(cache, WINDOWS_ARCHIVE_NAME, WINDOWS_PIN_FILE)
    staging = WINDOWS_TOOLCHAIN_DIR.with_name(WINDOWS_TOOLCHAIN_DIR.name + ".tmp")
    scratch = cache / "windows-unpack"
    for folder in (staging, scratch):
        if folder.exists():
            shutil.rmtree(folder)
    staging.mkdir(parents=True)
    try:
        extract_windows(archive, staging, scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    fetch_license(cache, staging / LICENSE_DEST)
    digest = tree_hash(staging)
    (staging / TREE_HASH_FILE).write_text(f"{digest}  build/toolchain/windows (tree)\n", encoding="utf-8")
    if WINDOWS_TOOLCHAIN_DIR.exists():
        shutil.rmtree(WINDOWS_TOOLCHAIN_DIR)
    staging.rename(WINDOWS_TOOLCHAIN_DIR)
    print(f"[toolchain] extracted to {WINDOWS_TOOLCHAIN_DIR} (tree {digest})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE, help="download cache folder")
    parser.add_argument("--check", action="store_true", help="verify the extracted tree hash only")
    parser.add_argument("--platform", choices=("linux", "windows"), default="linux", help="toolchain to fetch")
    args = parser.parse_args()
    if args.platform == "windows":
        return check(WINDOWS_TOOLCHAIN_DIR, "bin/clang++.exe") if args.check else fetch_windows(args.cache)
    return check() if args.check else fetch(args.cache)


if __name__ == "__main__":
    sys.exit(main())
