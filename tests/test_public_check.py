"""Tests of tools/verify/public_check.py (T-664): one fixture folder per rule.

Each rule gets a synthetic file that must be flagged and one that must not. The bytes are made
here (no game data, no dump), and the personal rule uses a made-up pattern in a tiny patterns
file written per test, so this file holds no real one. Every test keeps its fixture tree in `tmp_path / "tree"` and its manifest
next to it, outside the tree.
"""

import hashlib
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "verify"))

from public_check import main  # noqa: E402

EXAMPLE_PATTERN = "example-host-zz9"

HEX16 = " ".join(f"{i:02X}" for i in range(16))
DUMP = f"{HEX16}\n{HEX16}\n"


def write(tree: Path, rel: str, data: bytes) -> None:
    path = tree / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_manifest(folder: Path, sha256: str | None = None) -> Path:
    manifest = folder / "manifest.toml"
    body = ""
    if sha256 is not None:
        body = f'[[variant]]\n[[variant.file]]\npath = "X.DAT"\nsize = 1\nsha256 = "{sha256}"\n'
    manifest.write_text(body, encoding="utf-8")
    return manifest


def write_personal(folder: Path, patterns: tuple[str, ...] = (EXAMPLE_PATTERN,)) -> Path:
    path = folder / "personal-patterns.md"
    path.write_text("# notes, ignored\n" + "".join(f"- `{p}`\n" for p in patterns), encoding="utf-8")
    return path


def run(tree: Path, manifest: Path, capsys, personal: Path | None = None) -> tuple[int, str]:
    personal = personal or write_personal(manifest.parent)
    code = main(["--root", str(tree), "--manifest", str(manifest), "--personal", str(personal)])
    return code, capsys.readouterr().out


def test_forbidden_extension_is_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "disc/game.iso", b"synthetic")
    write(tree, "docs/readme.txt", b"plain text")
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "disc/game.iso: forbidden extension" in out
    assert "docs/readme.txt" not in out
    assert "public_check: FAILED, 1 hit(s) in 1 file(s) of 2 checked" in out


def test_forbidden_extension_is_case_insensitive(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "modules/MTAPMAN.IRX", b"synthetic")
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "modules/MTAPMAN.IRX: forbidden extension" in out


def test_file_over_1mb_is_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "data/blob.dat", b"\x00" * 1_500_000)
    write(tree, "data/small.dat", b"\x00" * 1000)
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "data/blob.dat: 1500000 bytes" in out
    assert "data/small.dat" not in out


def test_allow_listed_table_passes_size_but_is_still_scanned(tmp_path, capsys):
    tree = tmp_path / "tree"
    rel = "config/recomp/functions.filtered.csv"
    rows = "".join(f"FUN_{i:08X},0x{i:08X},0x{i + 8:08X},8\n" for i in range(40_000))
    table = ("Name,Start,End,Size\n" + rows).encode()
    assert len(table) > 1_000_000
    write(tree, rel, table)
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 0
    assert out == "public_check: OK (1 files)\n"

    write(tree, rel, table + DUMP.encode() + f"{EXAMPLE_PATTERN}\n".encode())
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    # header + 40 000 rows = 40 001 lines, so the dump starts on line 40 002
    assert f"{rel}:40002: 2 consecutive 16-byte hex lines" in out
    assert f"{rel}:40004: personal pattern" in out
    assert "over 1 MB" not in out


def test_other_large_table_is_still_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    rows = "".join(f"FUN_{i:08X},0x{i:08X},0x{i + 8:08X},8\n" for i in range(40_000))
    write(tree, "config/recomp/other.csv", ("Name,Start,End,Size\n" + rows).encode())
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "config/recomp/other.csv: " in out and "over 1 MB" in out


def test_personal_pattern_is_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "notes/log.txt", f"line one\nsee {EXAMPLE_PATTERN} here\n".encode())
    write(tree, "notes/other.txt", b"nothing personal in this one\n")
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "notes/log.txt:2: personal pattern" in out
    assert "notes/other.txt" not in out
    assert EXAMPLE_PATTERN not in out  # the report never repeats the pattern


def test_missing_personal_file_skips_the_rule(tmp_path, capsys):
    tree = tmp_path / "tree"
    missing = tmp_path / "absent.md"
    write(tree, "notes/log.txt", f"see {EXAMPLE_PATTERN} here\n".encode())
    write(tree, "disc/game.iso", b"synthetic")
    code, out = run(tree, write_manifest(tmp_path), capsys, personal=missing)
    assert f"public_check: personal rule skipped (no {missing})" in out
    assert "notes/log.txt" not in out
    assert code == 1  # the other rules still decide
    assert "disc/game.iso: forbidden extension" in out


def test_two_consecutive_hex_lines_are_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "docs/dump.txt", f"header\n{DUMP}tail\n".encode())
    # a single dump line, a short fragment, and two table rows that each hold 16 bytes in a cell
    write(tree, "docs/fragment.txt", f"{HEX16}\nsee 00 01 02 03 here\n".encode())
    write(tree, "docs/table.md", f"| a | {HEX16} |\n| b | {HEX16} |\n".encode())
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "docs/dump.txt:2: 2 consecutive 16-byte hex lines" in out
    assert "docs/fragment.txt" not in out
    assert "docs/table.md" not in out


def test_generated_path_is_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "generated/module.cpp", b"int x;\n")
    write(tree, "src/module.cpp", b"int x;\n")
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1
    assert "generated/module.cpp: inside generated/" in out
    assert "src/module.cpp" not in out


def test_manifest_hash_is_flagged(tmp_path, capsys):
    tree = tmp_path / "tree"
    payload = b"synthetic module bytes"
    manifest = write_manifest(tmp_path, hashlib.sha256(payload).hexdigest())
    write(tree, "copy.dat", payload)
    write(tree, "other.dat", b"something else")
    code, out = run(tree, manifest, capsys)
    assert code == 1
    assert "copy.dat: SHA-256 equals" in out
    assert "other.dat" not in out


def test_clean_tree_passes(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "src/main.cpp", b"int main() { return 0; }\n")
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 0
    assert out == "public_check: OK (1 files)\n"


# --- the archive allow-list for large files (T-697) ---

BIG = b"x" * 1_100_000


def run_with_allow(tree: Path, allow_text: str, tmp_path: Path, capsys, personal: Path | None = None):
    allow = tmp_path / "large.txt"
    allow.write_text(allow_text, encoding="utf-8")
    personal = personal or write_personal(tmp_path)
    code = main(["--root", str(tree), "--manifest", str(write_manifest(tmp_path)), "--personal", str(personal),
                 "--large-allow", str(allow)])
    return code, capsys.readouterr().out


def test_large_file_refused_without_the_option(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "don-linux/toolchain/lib/big.so", BIG)
    code, out = run(tree, write_manifest(tmp_path), capsys)
    assert code == 1 and "over 1 MB" in out


def test_large_file_accepted_by_a_matching_glob(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "don-linux/toolchain/lib/big.so", BIG)
    code, out = run_with_allow(tree, "# comment\n\n*/toolchain/** the pinned compiler\n", tmp_path, capsys)
    assert code == 0, out


def test_large_file_refused_by_a_non_matching_glob(tmp_path, capsys):
    tree = tmp_path / "tree"
    write(tree, "don-linux/lib/big.so", BIG)
    code, out = run_with_allow(tree, "*/toolchain/** the pinned compiler\n", tmp_path, capsys)
    assert code == 1 and "over 1 MB" in out


@pytest.mark.parametrize("pattern", ["**", "*", "*/**", "**/*", "?"])
def test_everything_pattern_is_rejected(tmp_path, capsys, pattern):
    tree = tmp_path / "tree"
    write(tree, "a.txt", b"x")
    with pytest.raises(SystemExit):
        run_with_allow(tree, f"{pattern} everything\n", tmp_path, capsys)


def test_hash_and_personal_rules_still_apply_inside_an_allowed_file(tmp_path, capsys):
    tree = tmp_path / "tree"
    data = BIG + f"\n{EXAMPLE_PATTERN}\n".encode()
    write(tree, "don-linux/toolchain/x.txt", data)
    manifest = write_manifest(tmp_path, hashlib.sha256(data).hexdigest())
    allow = tmp_path / "large.txt"
    allow.write_text("*/toolchain/** compiler\n", encoding="utf-8")
    code = main(["--root", str(tree), "--manifest", str(manifest), "--personal", str(write_personal(tmp_path)),
                 "--large-allow", str(allow)])
    out = capsys.readouterr().out
    assert code == 1
    assert "SHA-256 equals" in out and "personal pattern" in out and "over 1 MB" not in out


# T-725: the project's source URL is allowed even though it holds the account name. The name is split
# here so that this file does not trip the owner's real personal patterns.
ACCOUNT = "d" + "vilelaf"
ALLOWED_URL = f"https://github.com/{ACCOUNT}/battlestadium-don"


def personal_hits(tmp_path, capsys, text: str) -> tuple[int, str]:
    tree = tmp_path / "tree"
    write(tree, "notes/url.txt", text.encode())
    return run(tree, write_manifest(tmp_path), capsys, personal=write_personal(tmp_path, (ACCOUNT,)))


def test_allowed_source_url_is_not_a_personal_hit(tmp_path, capsys):
    code, out = personal_hits(tmp_path, capsys, f"source: {ALLOWED_URL}\n")
    assert code == 0 and "personal pattern" not in out


def test_allowed_url_does_not_hide_the_account_email(tmp_path, capsys):
    code, out = personal_hits(tmp_path, capsys, f"{ALLOWED_URL} {ACCOUNT}@example.com\n")
    assert code == 1 and "notes/url.txt:1: personal pattern" in out


def test_other_url_of_the_account_is_a_personal_hit(tmp_path, capsys):
    code, out = personal_hits(tmp_path, capsys, f"https://github.com/{ACCOUNT}/other\n")
    assert code == 1 and "notes/url.txt:1: personal pattern" in out
