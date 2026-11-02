"""Verify that a Battle Stadium D.O.N PS2 disc image is one of the known variants.

Usage: uv run --no-project python -I tools/disc/verify_disc.py <iso>

The files are streamed out of the ISO with `7z x -so`; the image is never copied or modified.
Exit codes: 0 known variant, 1 error (no ISO, 7z failure, file missing), 2 not a known variant.
"""

import hashlib
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CONFIG_PATH = ROOT / "config" / "discs.toml"
DATA_NAME = "DATA.AFS"
BLOCK_SIZE = 1 << 20

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_MISMATCH = 2


class ToolError(Exception):
    """A problem that stops the verification before a decision can be made."""


def load_config(path=CONFIG_PATH):
    with open(path, "rb") as file:
        return tomllib.load(file)


def hash_member(iso, member, algorithm):
    """Hash one file of the ISO, reading the output of `7z x -so` in blocks."""
    try:
        proc = subprocess.Popen(
            ["7z", "x", "-so", str(iso), member],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise ToolError(f"cannot run 7z: {error}") from error

    digest = hashlib.new(algorithm)
    size = 0
    for block in iter(lambda: proc.stdout.read(BLOCK_SIZE), b""):
        digest.update(block)
        size += len(block)
    stderr = proc.stderr.read().decode(errors="replace").strip()
    if proc.wait() != 0:
        raise ToolError(f"7z could not read {member} from {iso}: {stderr}")
    if size == 0:
        raise ToolError(f"{member} is missing or empty in {iso}")
    return digest.hexdigest()


def classify(elf_sha256, data_sha1, config):
    """Decide which variant the disc is.

    Returns (code, id):
      (EXIT_OK, variant_id)        the ELF and DATA.AFS match a known variant
      (EXIT_MISMATCH, "unknown")   the ELF matches but DATA.AFS is from no known variant
      (EXIT_MISMATCH, None)        the ELF does not match the known executable
    """
    if elf_sha256.lower() != config["elf"]["sha256"].lower():
        return EXIT_MISMATCH, None
    for variant in config["variant"]:
        if data_sha1.lower() == variant["data_afs_sha1"].lower():
            return EXIT_OK, variant["id"]
    return EXIT_MISMATCH, "unknown"


def main(argv):
    if len(argv) != 1:
        print("usage: verify_disc.py <iso>", file=sys.stderr)
        return EXIT_ERROR
    iso = Path(argv[0])
    try:
        if not iso.is_file():
            raise ToolError(f"ISO not found: {iso}")
        try:
            config = load_config()
        except (OSError, tomllib.TOMLDecodeError) as error:
            raise ToolError(f"cannot read {CONFIG_PATH}: {error}") from error
        elf_sha256 = hash_member(iso, config["elf"]["name"], "sha256")
        data_sha1 = hash_member(iso, DATA_NAME, "sha1")
    except ToolError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return EXIT_ERROR

    code, variant = classify(elf_sha256, data_sha1, config)
    if variant is None:
        print(f"ELF SHA-256 mismatch: expected {config['elf']['sha256']}")
        print(f"ELF SHA-256 found:    {elf_sha256}")
    elif variant == "unknown":
        print("VARIANT: unknown")
        print(f"DATA.AFS SHA-1: {data_sha1}")
    else:
        print(f"VARIANT: {variant}")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
