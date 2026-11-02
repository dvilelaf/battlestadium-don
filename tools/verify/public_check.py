"""Publication gate: fails when the published tree holds game material or personal data (T-664).

Usage, from the repository root:

    uv run --no-project python -I tools/verify/public_check.py [--root FOLDER] [--manifest FILE] [--personal FILE]
                                                    [--large-allow FILE]

Without --root it checks the files git tracks, which is what gets published. With --root it checks
every regular file under FOLDER. Symbolic links are never followed. Prints one line per hit
(`file:line: reason`, or `file: reason` when the whole file is the problem), then a verdict.
Exit 0 when nothing is found, 1 when there are hits.

The six classes come from AGENTS.md rule 1 (nothing from the game is versioned) and from the
owner's personal patterns, read from a git-ignored file (default work/public/personal-patterns.md).
"""

import argparse
import fnmatch
import hashlib
import os
import re
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO / "config" / "disc_manifest.toml"

# Disc images, executables, IRX modules, data archives, textures and emulator dumps (AGENTS.md
# rule 1), and the game payload formats of config/disc_manifest.toml. Compared case-insensitively.
FORBIDDEN_SUFFIXES = (".iso", ".elf", ".irx", ".afs", ".fpk", ".bin", ".rvz", ".gz")

LARGE_FILE_BYTES = 1_000_000

# Files over LARGE_FILE_BYTES that may be published, each with its reason. Keep this table short.
LARGE_FILE_ALLOW = {
    # Ghidra function table: names, start addresses and sizes. AGENTS.md rule 1 allows names and
    # addresses; the table holds no code bytes and no decompiled function bodies.
    "config/functions.ghidra.csv": "function names, addresses and sizes (table, 2.2 MB)",
    # Frozen recompiler function list (T-612): one line per function, Name,Start,End,Size. Names,
    # addresses and sizes only; no code bytes. The other rules still scan it.
    "config/recomp/functions.filtered.csv": "function names, addresses and sizes (table, 2.3 MB, T-612)",
}

# Strings that identify the owner and the owner's machines are never kept in git. They live in a
# git-ignored file (work/ is ignored), one literal case-sensitive substring per line written as
# ``- `pattern` ``; other lines are ignored. Matches are reported without their text.
DEFAULT_PERSONAL = REPO / "work" / "public" / "personal-patterns.md"
PERSONAL_LINE = re.compile(r"^- `(.+)`\s*$")

# Strings blanked out of a line before the personal patterns are matched (case-insensitive). The project's own
# source URL holds the account name; the owner decided on 2026-10-27 that it may be published. The name is split
# so that this file does not trip the owner's real patterns. Any other use of the name stays a hit.
PUBLIC_IDENTITY_ALLOW = ("https://github.com/" + "d" + "vilelaf" + "/battlestadium-don",)

# A dump line is a line made only of byte tokens (16 or more two-digit hex values, separated by
# spaces), with an optional leading offset such as `00000000`. A byte run inside a table cell or a
# sentence is an inline fragment, not a dump line: AGENTS.md rule 1 allows short fragments that
# explain a diagnosis (e.g. the "first bytes" table at docs/knowledge/03-data-formats.md:95-96).
# A single dump line, or a short fragment, is not a hit; two or more consecutive dump lines are.
# Do not lower HEX_RUN_MIN_LINES, and do not drop the run rule.
HEX_LINE = re.compile(r"^\s*(?:[0-9A-Fa-f]{8}:?\s+)?[0-9A-Fa-f]{2}(?:[ \t]+[0-9A-Fa-f]{2}){15,}\s*$")
HEX_RUN_MIN_LINES = 2


@dataclass(frozen=True)
class Hit:
    path: str
    line: int  # 0 when the whole file is the problem
    reason: str


def load_manifest_hashes(manifest: Path) -> set[str]:
    data = tomllib.loads(manifest.read_text(encoding="utf-8"))
    return {f["sha256"].lower() for v in data.get("variant", []) for f in v.get("file", [])}


def load_personal_patterns(path: Path) -> list[str]:
    found = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = PERSONAL_LINE.match(line)
        if match:
            found.append(match.group(1))
    return found


def load_large_allow(path: Path) -> list[str]:
    """Globs from a --large-allow file: one `<glob> <reason>` per line, `#` starts a comment.

    The globs only lift the size rule (T-697, the release archive). A glob made only of wildcards would lift
    it for every file, so it is an error, as is an entry without a reason.
    """
    globs = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        glob, _, reason = line.partition(" ")
        if not glob.strip("*?/"):
            sys.exit(f"public_check: {path}:{number}: pattern {glob!r} matches everything")
        if not reason.strip():
            sys.exit(f"public_check: {path}:{number}: entry has no reason")
        globs.append(glob)
    return globs


def check_file(rel: str, data: bytes, hashes: set[str], personal: list[str], large_allow: list[str] | None = None) -> list[Hit]:
    hits = []
    allowed_large = rel in LARGE_FILE_ALLOW or any(fnmatch.fnmatchcase(rel, g) for g in large_allow or [])
    if rel.lower().endswith(FORBIDDEN_SUFFIXES):
        hits.append(Hit(rel, 0, "forbidden extension (game payload)"))
    if len(data) > LARGE_FILE_BYTES and not allowed_large:
        hits.append(Hit(rel, 0, f"{len(data)} bytes: over 1 MB and not in the allow-list"))
    if rel.split("/")[0] == "generated":
        hits.append(Hit(rel, 0, "inside generated/ (generated code is never published)"))
    if hashlib.sha256(data).hexdigest() in hashes:
        hits.append(Hit(rel, 0, "SHA-256 equals a file listed in the disc manifest"))

    lines = data.decode("latin-1").splitlines()
    for number, line in enumerate(lines, 1):
        scrubbed = line
        for allowed in PUBLIC_IDENTITY_ALLOW:
            scrubbed = re.sub(re.escape(allowed), "", scrubbed, flags=re.IGNORECASE)
        for pattern in personal:
            if pattern in scrubbed:
                hits.append(Hit(rel, number, "personal pattern (see the --personal file)"))

    # The empty line at the end flushes a run that reaches the last line.
    run_start, run_len = 0, 0
    for number, line in enumerate(lines + [""], 1):
        if HEX_LINE.search(line):
            if run_len == 0:
                run_start = number
            run_len += 1
            continue
        if run_len >= HEX_RUN_MIN_LINES:
            hits.append(Hit(rel, run_start, f"{run_len} consecutive 16-byte hex lines (dump)"))
        run_len = 0
    return hits


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
    ).stdout
    return [name.decode("utf-8") for name in out.split(b"\0") if name]


def walked_files(root: Path) -> list[str]:
    found = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [name for name in dirnames if name != ".git"]
        for name in filenames:
            found.append((Path(dirpath) / name).relative_to(root).as_posix())
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check the published tree (T-664).")
    parser.add_argument("--root", type=Path, help="check every file under this folder, not the git-tracked files")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST, help="disc manifest with the hashes")
    parser.add_argument("--personal", type=Path, default=DEFAULT_PERSONAL, help="git-ignored file with the personal patterns")
    parser.add_argument("--large-allow", type=Path, help="file of `<glob> <reason>` lines: files that may exceed the size limit (size rule only)")
    args = parser.parse_args(argv)
    large_allow = load_large_allow(args.large_allow) if args.large_allow else []

    personal: list[str] = []
    if args.personal.is_file():
        personal = load_personal_patterns(args.personal)
    else:
        print(f"public_check: personal rule skipped (no {args.personal})")

    root = args.root.resolve() if args.root else REPO
    rels = tracked_files(root) if args.root is None else walked_files(root)
    hashes = load_manifest_hashes(args.manifest)

    hits: list[Hit] = []
    checked = 0
    for rel in rels:
        path = root / rel
        if path.is_symlink() or not path.is_file():
            continue
        checked += 1
        hits += check_file(rel, path.read_bytes(), hashes, personal, large_allow)

    for hit in hits:
        location = f"{hit.path}:{hit.line}" if hit.line else hit.path
        print(f"{location}: {hit.reason}")
    if hits:
        files = len({hit.path for hit in hits})
        print(f"public_check: FAILED, {len(hits)} hit(s) in {files} file(s) of {checked} checked")
        return 1
    print(f"public_check: OK ({checked} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
