"""Tests of tools/funcmap/validate.py with a synthetic ELF, CSV and layout (no game files)."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "funcmap"))
import validate  # noqa: E402

pytest.importorskip("rabbitizer")

BASE = 0x100000
TEXT_END = 0x100040
NOP = 0
JR_RA = 0x03E00008
BAD = 0x00000055  # not an R5900 instruction


def jal(target):
    return (3 << 26) | (target >> 2)


def make_elf(path, words, entry=0x100008):
    data = struct.pack(f"<{len(words)}I", *words)
    hdr = bytearray(52)
    hdr[:4] = b"\x7fELF"
    hdr[4], hdr[5] = 1, 1
    struct.pack_into("<I", hdr, 0x18, entry)
    struct.pack_into("<I", hdr, 0x1C, 52)
    struct.pack_into("<HH", hdr, 0x2A, 32, 1)
    ph = struct.pack("<8I", 1, 84, BASE, BASE, len(data), len(data), 5, 0x10)
    Path(path).write_bytes(bytes(hdr) + ph + data)


def make_layout(path, islands=()):
    t = f"[text]\nstart = 0x{BASE:X}\nend = 0x{TEXT_END:X}\n"
    for s, e in islands:
        t += f'\n[[island]]\nstart = 0x{s:X}\nend = 0x{e:X}\nkind = "code"\n'
    Path(path).write_text(t)


def make_csv(path, rows):
    lines = ["Name,Start,End,Size"]
    for n, s, e in rows:
        lines.append(f"{n},0x{s:08x},0x{e:08x},{e - s}")
    Path(path).write_text("\n".join(lines) + "\n")


def R(name, s, e, size=None):
    return validate.Row(name, s, e, e - s if size is None else size)


# 16 words at BASE: main function 0..0x20 calls a function at 0x100020 (callee 0x20..0x30)
GOOD_WORDS = [NOP, NOP, JR_RA, NOP, jal(0x100020), NOP, JR_RA, NOP, JR_RA, NOP, NOP, NOP, 0, 0, 0, 0]
GOOD_ROWS = [("entry", 0x100000, 0x100020), ("FUN_00100020", 0x100020, 0x100030)]


def codes(errors):
    return [e.code for e in errors]


def run(tmp_path, rows, words=GOOD_WORDS, islands=(), entry=0x100008, extra=None, extra_args=()):
    make_elf(tmp_path / "x.elf", words, entry)
    make_layout(tmp_path / "l.toml", islands)
    make_csv(tmp_path / "f.csv", rows)
    argv = ["--csv", str(tmp_path / "f.csv"), "--layout", str(tmp_path / "l.toml"), "--elf", str(tmp_path / "x.elf")]
    if extra is not None:
        (tmp_path / "extra.txt").write_text(extra)
        argv += ["--extra", str(tmp_path / "extra.txt")]
    return validate.main(argv + list(extra_args))


def test_good_case_exits_zero(tmp_path, capsys):
    assert run(tmp_path, GOOD_ROWS) == 0
    out = capsys.readouterr().out
    assert "errors: E1=0, E2=0, E3=0, E4=0, E5=0" in out
    assert "2 functions, 0 labels" in out


def test_e1_partial_overlap_is_reported_with_both_rows():
    errs, nested = validate.check_e1([R("FUN_a", 0x1000, 0x1020), R("FUN_b", 0x1010, 0x1030)])
    assert codes(errs) == ["E1"] and nested == 0
    assert "FUN_a" in errs[0].message and "FUN_b" in errs[0].message


def test_e1_nested_functions_are_allowed_and_counted():
    fs = [R("FUN_big", 0x1000, 0x2000), R("FUN_in", 0x1100, 0x1200), R("FUN_in2", 0x1200, 0x1300),
          R("FUN_same_start", 0x1000, 0x1010), R("FUN_after", 0x2000, 0x2010)]
    errs, nested = validate.check_e1(fs)
    assert errs == [] and nested == 3


def test_e1_ignores_labels_in_the_cli(tmp_path, capsys):
    rows = [("entry", 0x100000, 0x100020), ("entry_00100018", 0x100018, 0x100030), ("FUN_00100020", 0x100020, 0x100030)]
    assert run(tmp_path, rows) == 0
    assert "labels outside every function: 0" in capsys.readouterr().out


def test_e2_good_and_bad():
    assert validate.check_e2([R("FUN_a", 0x1000, 0x1010)]) == []
    assert codes(validate.check_e2([R("FUN_a", 0x1002, 0x1010, 0xE)])) == ["E2"]  # unaligned
    assert codes(validate.check_e2([R("FUN_a", 0x1010, 0x1010, 0)])) == ["E2"]  # End == Start
    assert codes(validate.check_e2([R("FUN_a", 0x1000, 0x1010, 0x20)])) == ["E2"]  # wrong Size


def test_e3_good_and_bad():
    isl = [(0x200000, 0x200100, "code")]
    ok = [R("FUN_a", 0x100000, 0x100040), R("FUN_i", 0x200000, 0x200100)]
    assert validate.check_e3(ok, BASE, TEXT_END, isl) == []
    bad = [R("FUN_x", 0x300000, 0x300010),  # in no island
           R("entry_big", 0x200000, 0x400000),  # starts in an island but its whole range is not
           R("FUN_edge", 0x100020, 0x100050)]  # crosses the end of the text
    errs = validate.check_e3(bad, BASE, TEXT_END, isl)
    assert codes(errs) == ["E3", "E3", "E3"]


def test_e4_good_extra_and_bad(tmp_path, capsys):
    # the jal at 0x100010 calls 0x100030, which is no row
    words = list(GOOD_WORDS)
    words[4] = jal(0x100030)
    assert run(tmp_path, GOOD_ROWS, words) == 1
    out = capsys.readouterr().out
    assert "E4=1" in out and "0x00100030" in out and "jal at 0x00100010" in out and "entry" in out
    assert "target is in no row" in out
    # the same target listed in --extra is accepted
    assert run(tmp_path, GOOD_ROWS, words, extra="# extra\nentry_00100030@0x100030\n") == 0


def test_e4_target_inside_a_row_names_that_row():
    rows = [R("FUN_a", 0x100000, 0x100020), R("FUN_b", 0x100020, 0x100030)]
    words = list(GOOD_WORDS)
    words[4] = jal(0x100024)
    cov = validate.coverage(rows, BASE, TEXT_END)
    errs = validate.check_e4(rows, words, cov, BASE)
    assert codes(errs) == ["E4"]
    assert "inside FUN_b" in errs[0].message and "FUN_a" in errs[0].message


def test_e4_ignores_jal_outside_every_row():
    rows = [R("FUN_a", 0x100000, 0x100010)]  # the jal at 0x100010 is not covered
    cov = validate.coverage(rows, BASE, TEXT_END)
    assert validate.check_e4(rows, list(GOOD_WORDS), cov, BASE) == []


def test_e5_good_and_bad():
    assert validate.check_e5([R("entry", 0x100008, 0x100020)]) == []  # starts at the entry
    assert validate.check_e5([R("entry", 0x100000, 0x100020)]) == []  # inside a function
    assert codes(validate.check_e5([R("entry_00100000", 0x100000, 0x100020)])) == ["E5"]  # label only
    assert codes(validate.check_e5([R("FUN_a", 0x100020, 0x100030)])) == ["E5"]


def test_gaps_zero_and_nonzero_valid_and_invalid(tmp_path, capsys):
    # rows cover 0x100000-0x100008, 0x100010-0x100014, 0x10001C-0x100020 and 0x100030-0x100034
    rows = [("entry", 0x100000, 0x100008), ("FUN_a", 0x100010, 0x100014), ("FUN_b", 0x10001C, 0x100020),
            ("FUN_c", 0x100030, 0x100034)]
    words = [NOP] * 16
    words[2], words[3] = JR_RA, NOP  # gap 0x100008-0x100010: valid non-zero
    words[5], words[6] = BAD, JR_RA  # gap 0x100014-0x10001C: has an invalid word
    # gap 0x100020-0x100030: zero padding; gap 0x100034-0x100040: zero padding
    out_tsv = tmp_path / "reports" / "gaps.tsv"
    rc = run(tmp_path, rows, words, extra_args=["--gaps-out", str(out_tsv)])
    out = capsys.readouterr().out
    assert rc == 1  # entry is not covered by a function row starting at/inside 0x100008
    assert "uncovered gaps: 4 (2 only zero words = padding, 2 with non-zero words)" in out
    assert "1 decode completely as valid R5900 instructions, 1 do not" in out
    lines = out_tsv.read_text().splitlines()
    assert lines[0] == "start\tend\tbytes\tnonzero_words\tall_valid"
    assert lines[1:] == ["0x00100008\t0x00100010\t8\t1\t1", "0x00100014\t0x0010001C\t8\t2\t0"]


def test_coverage_percentage_and_nested_function_in_cli(tmp_path, capsys):
    rows = [("FUN_big", 0x100000, 0x100030), ("FUN_in", 0x100020, 0x100028)]
    assert run(tmp_path, rows) == 0
    out = capsys.readouterr().out
    assert "nested functions (inside another function, allowed): 1" in out
    assert "48 of 64 bytes covered by at least one row (75.00%)" in out


def test_extra_file_formats(tmp_path):
    p = tmp_path / "e.txt"
    p.write_text("# c\n0x100030\nentry_00100034@0x100034  # note\n\n")
    assert validate.read_extra(p) == {0x100030, 0x100034}
