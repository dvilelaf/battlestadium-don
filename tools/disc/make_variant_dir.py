"""Build the folder the port runs from for one disc variant (Japanese, English, English voices).

Usage:
    uv run --no-project python -I tools/disc/make_variant_dir.py JP_DIR VARIANT DEST [--copy]

    JP_DIR    the folder of the Japanese disc's files (SLPS_256.75, SYSTEM.CNF, DIR.DIR, MODULES/,
              MOVIE.AFS and the three data files); `disc/` after tools/disc/extract.sh.
    VARIANT   what holds the variant's data files: a folder with the files extracted from its image
              (DATA.AFS, and BGM.AFS / SE.AFS when they differ), or the disc image itself (a file;
              it is read with `7z`, which has to be installed, and the three files are extracted).
    DEST      the folder to make. Run the port on DEST/SLPS_256.75 and give the image with --iso.

The three data files (DATA.AFS, BGM.AFS, SE.AFS) come from VARIANT; a file VARIANT lacks is taken from
JP_DIR. Everything else (executable, SYSTEM.CNF, DIR.DIR, MODULES, MOVIE.AFS) comes from JP_DIR: it is the
same on all three discs (docs/knowledge/07-english-patch.md). Files are symbolic links where the platform
allows them and copies otherwise, or always with --copy. The variant is recognised by the SHA-1 of the
DATA.AFS that was chosen, against config/discs.toml; an unknown DATA.AFS stops the tool.

DEST is only ever built over a folder this tool made: it writes `.make_variant_dir.json` there, listing what
it created, and refuses to touch a DEST that exists, is not empty and has no such file. Files the port puts
in DEST (memory cards, mc0/ and mc1/) are not in the list and are left alone.

Only the standard library is used. Exit codes: 0 done, 1 error (missing file, bad DEST, 7z failure),
2 DATA.AFS is not a known variant.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CONFIG_PATH = ROOT / "config" / "discs.toml"
MARKER = ".make_variant_dir.json"
EXTRACTED = "variant_files"  # inside DEST, when VARIANT is an image
DATA_FILES = ("DATA.AFS", "BGM.AFS", "SE.AFS")
JP_FILES = ("SLPS_256.75", "SYSTEM.CNF", "DIR.DIR", "MODULES", "MOVIE.AFS")
BLOCK_SIZE = 1 << 20

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_UNKNOWN = 2


class MakeError(Exception):
    """Anything that stops the tool with exit code 1."""


def sha(path, algorithm):
    digest = hashlib.new(algorithm)
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(BLOCK_SIZE), b""):
            digest.update(block)
    return digest.hexdigest()


def recognise(data_afs, config):
    """The id of the variant whose DATA.AFS SHA-1 this is, or None."""
    value = sha(data_afs, "sha1")
    for variant in config["variant"]:
        if value == variant["data_afs_sha1"].lower():
            return variant["id"]
    return None


def extract_image(image, folder):
    """Extract the data files of a disc image into `folder` with 7z."""
    try:
        result = subprocess.run(
            ["7z", "x", "-y", f"-o{folder}", str(image), *DATA_FILES],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise MakeError(f"cannot run 7z to read {image}: {error}") from error
    if result.returncode != 0:
        raise MakeError(f"7z could not read {image}: {result.stderr.decode(errors='replace').strip()}")


def check_dest(dest):
    """Return the names a previous run made in dest, or refuse a folder that is not ours."""
    if not dest.exists():
        return []
    if not dest.is_dir():
        raise MakeError(f"{dest} exists and is not a folder")
    marker = dest / MARKER
    if marker.is_file():
        try:
            return list(json.loads(marker.read_text())["made"])
        except (ValueError, KeyError) as error:
            raise MakeError(f"{marker} is not readable: {error}") from error
    if any(dest.iterdir()):
        raise MakeError(f"{dest} is not empty and was not made by this tool; give another DEST or empty it")
    return []


def remove_made(dest, names):
    for name in names:
        path = dest / name
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)


def place(source, target, copy):
    """Link (or copy) source at target. Returns "link" or "copy"."""
    source = source.resolve()
    if not copy:
        try:
            target.symlink_to(source, target_is_directory=source.is_dir())
            return "link"
        except (OSError, NotImplementedError):
            pass  # no permission for links on this platform: copy
    if source.is_dir():
        shutil.copytree(source, target)
    else:
        shutil.copy2(source, target)
    return "copy"


def make(jp_dir, variant, dest, copy=False, config=None):
    """Make dest. Returns (variant id or None, {name: "link"|"copy"})."""
    config = config or tomllib.loads(CONFIG_PATH.read_text())
    jp_dir, variant, dest = Path(jp_dir), Path(variant), Path(dest)

    missing = [name for name in JP_FILES if not (jp_dir / name).exists()]
    if missing:
        raise MakeError(f"{jp_dir} lacks {', '.join(missing)}")
    if not variant.exists():
        raise MakeError(f"{variant} does not exist")
    previous = check_dest(dest)

    elf = jp_dir / "SLPS_256.75"
    if sha(elf, "sha256") != config["elf"]["sha256"].lower():
        raise MakeError(f"{elf} is not the game's executable (SHA-256 differs from config/discs.toml)")

    holder = None
    if variant.is_dir():
        source_dir = variant
    else:
        # An image: extract into DEST itself (a folder of ours) so the links outlive this run.
        remove_made(dest, previous)
        previous = []
        holder = dest / EXTRACTED
        holder.mkdir(parents=True, exist_ok=True)
        source_dir = holder
    try:
        if holder is not None:
            extract_image(variant, holder)
        chosen = {}
        for name in DATA_FILES:
            for folder in (source_dir, jp_dir):
                if (folder / name).is_file():
                    chosen[name] = folder / name
                    break
            else:
                raise MakeError(f"no {name} in {source_dir} or {jp_dir}")
        variant_id = recognise(chosen["DATA.AFS"], config)
    except BaseException:
        if holder is not None:
            shutil.rmtree(holder, ignore_errors=True)
        raise
    if variant_id is None:
        if holder is not None:
            shutil.rmtree(holder, ignore_errors=True)
        return None, {}

    dest.mkdir(parents=True, exist_ok=True)
    remove_made(dest, previous)
    made = {}
    for name in JP_FILES:
        made[name] = place(jp_dir / name, dest / name, copy)
    for name in DATA_FILES:
        made[name] = place(chosen[name], dest / name, copy)
    listed = list(made)
    if holder is not None:
        listed.append(EXTRACTED)
    (dest / MARKER).write_text(json.dumps({"variant": variant_id, "made": listed}, indent=2) + "\n")
    return variant_id, made


def main(argv):
    parser = argparse.ArgumentParser(description="Make the folder for one disc variant.")
    parser.add_argument("jp_dir", help="folder with the Japanese disc's files")
    parser.add_argument("variant", help="folder with the variant's extracted files, or its disc image")
    parser.add_argument("dest", help="folder to make")
    parser.add_argument("--copy", action="store_true", help="copy the files instead of linking them")
    parser.add_argument("--config", default=str(CONFIG_PATH), help="variant table (default: config/discs.toml)")
    args = parser.parse_args(argv)
    try:
        config = tomllib.loads(Path(args.config).read_text())
        variant_id, made = make(args.jp_dir, args.variant, args.dest, args.copy, config)
    except (MakeError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_ERROR
    if variant_id is None:
        print("error: DATA.AFS is not one of the known variants (config/discs.toml); nothing was made", file=sys.stderr)
        return EXIT_UNKNOWN
    descriptions = {v["id"]: v["description"] for v in config["variant"]}
    print(f"variant: {variant_id} ({descriptions[variant_id]})")
    for name, how in made.items():
        print(f"  {how:4} {name}")
    print(f"run the port on: {Path(args.dest) / 'SLPS_256.75'}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
