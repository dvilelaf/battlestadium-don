"""Tests of tools/funcmap/find_code_pointers.py with a synthetic ELF, CSV, layout and VU table."""
import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "funcmap"))
import find_code_pointers as fcp  # noqa: E402

pytest.importorskip("rabbitizer")

BASE = 0x100000
TEXT_END = 0x100100
FILE_END = 0x100400
JR_RA = 0x03E00008
BAD = 0x00000055  # not an R5900 instruction
LUI_V0_10 = 0x3C020010  # lui v0, 0x10
ADDIU_V0_V0 = 0x24420000  # addiu v0, v0, lo
ORI_V0_V0 = 0x34420000
JAL = 0x0C000000


def jal_to(addr):
    return JAL | (addr >> 2)


def make_blob(words_by_addr, size=FILE_END - BASE):
    words = [0] * (size // 4)
    for a, w in words_by_addr.items():
        words[(a - BASE) // 4] = w
    return struct.pack(f"<{len(words)}I", *words)


def write_elf(path, blob, base=BASE):
    """ELF32 LE with one PT_LOAD at the file offset 0x100."""
    hdr = bytearray(0x34)
    hdr[0:4] = b"\x7fELF"
    hdr[4], hdr[5] = 1, 1
    struct.pack_into("<I", hdr, 0x18, base)  # entry
    struct.pack_into("<I", hdr, 0x1C, 0x34)  # phoff
    struct.pack_into("<HH", hdr, 0x2A, 32, 1)
    ph = struct.pack("<8I", 1, 0x100, base, base, len(blob), len(blob), 5, 0)
    path.write_bytes(bytes(hdr) + ph + b"\0" * (0x100 - 0x34 - 32) + blob)


ROWS = [
    ("entry", 0x100000, 0x100020),
    ("FUN_a", 0x100020, 0x100060),
    ("entry_00100040", 0x100040, 0x100050),  # a label inside FUN_a
    ("FUN_b", 0x100060, 0x100100),
]


def run(words, rows=ROWS, vu=(), blobs=()):
    blob = make_blob(words)
    classes, _ = fcp.run(sorted(rows, key=lambda r: (r[1], r[2])), blob, BASE, BASE, TEXT_END, list(vu), list(blobs))
    return classes


def by_addr(classes, cls):
    return {e["address"]: e for e in classes[cls]}


def test_data_word_classes():
    words = {
        0x100200: 0x100020,  # row start
        0x100204: 0x100030,  # inside FUN_a
        0x100208: 0x100100,  # end of the text: not a candidate
        0x10020C: 0x100031,  # unaligned
        0x100210: 0x1000F0 + 0x100,  # beyond the text
        0x100300: 0x100050,  # inside FUN_a, after the label
    }
    c = run(words)
    assert set(by_addr(c, "row_start")) == {0x100020}
    assert set(by_addr(c, "inside_row")) == {0x100030, 0x100050}
    assert by_addr(c, "inside_row")[0x100030]["found_at"] == {"data": 0x100204}
    assert by_addr(c, "inside_row")[0x100030]["sources"] == ["data"]
    assert c["new_code"] == c["rejected"] == []


def test_data_scan_skips_the_text_vu_programs_and_kernel_blobs():
    words = {0x1000F8: 0x100020, 0x100200: 0x100020, 0x100280: 0x100030, 0x100300: 0x100050}
    c = run(words, vu=[(0x100280, 0x100290)], blobs=[(0x100300, 0x100310)])
    assert [(e["address"], e["found_at"]["data"]) for e in c["row_start"]] == [(0x100020, 0x100200)]
    assert c["inside_row"] == []


def test_row_innermost_function_is_reported_and_labels_are_the_fallback():
    rows = ROWS + [("FUN_big", 0x100000, 0x100100), ("entry_x", 0x100070, 0x100090)]
    c = run({0x100200: 0x100048, 0x100204: 0x100080, 0x100208: 0x1000A0}, rows=rows)
    assert by_addr(c, "inside_row")[0x100048]["row"] == "FUN_a"  # not the label, not FUN_big
    assert by_addr(c, "inside_row")[0x100080]["row"] == "FUN_b"
    assert fcp.innermost([("entry_q", 0x10, 0x20)], 0x14) == "entry_q"
    assert fcp.innermost([("FUN_q", 0x10, 0x20)], 0x30) == "-"


def test_code_pair_with_addiu_and_ori():
    words = {
        0x100020: LUI_V0_10, 0x100024: ADDIU_V0_V0 | 0x0030,  # 0x100030
        0x100028: LUI_V0_10, 0x10002C: 0x00000000, 0x100030: ORI_V0_V0 | 0x0070,  # 0x100070 (one nop between)
        0x100034: LUI_V0_10, 0x100038: ADDIU_V0_V0 | 0xFFE0,  # 0x0FFFE0 sign extended: outside
    }
    c = run(words)
    got = {e["address"]: e for e in c["inside_row"]}
    assert got[0x100030]["found_at"] == {"code_pair": 0x100020}
    assert got[0x100070]["found_at"] == {"code_pair": 0x100028}
    assert 0x0FFFE0 not in got


def test_code_pair_addiu_sign_extends_and_ori_does_not():
    rows = [("FUN_a", 0x100000, 0x120000)]
    words = {0x100000: 0x3C020011, 0x100004: ADDIU_V0_V0 | 0xFF00,  # 0x110000 - 0x100 = 0x10FF00
             0x100008: 0x3C020010, 0x10000C: ORI_V0_V0 | 0xFF00}  # 0x10FF00
    blob = make_blob(words, size=0x21000)
    cands = {}
    _, covered = fcp.build_coverage(rows, BASE, 0x120000)
    fcp.collect_code(blob, BASE, BASE, 0x120000, covered, cands)
    assert cands == {0x10FF00: {"code_pair": 0x100000}}  # the first pair wins, the second is the same address


def test_code_pair_window_and_register():
    words = {
        0x100028: LUI_V0_10, 0x10002C: 0, 0x100030: 0, 0x100034: 0, 0x100038: 0, 0x10003C: ADDIU_V0_V0 | 0x30,  # 5 apart
        0x100040: LUI_V0_10, 0x100044: 0x24630030,  # addiu v1, v1: other register
    }
    c = run(words)
    assert not any(c.values())


def test_code_pair_must_lie_inside_rows():
    rows = [("FUN_a", 0x100000, 0x100020), ("FUN_b", 0x100060, 0x100100)]
    c = run({0x100040: LUI_V0_10, 0x100044: ADDIU_V0_V0 | 0x70}, rows=rows)  # the pair is in a hole
    assert not any(c.values())
    c = run({0x100018: LUI_V0_10, 0x10001C: ADDIU_V0_V0 | 0x70}, rows=rows)
    assert [e["address"] for e in c["inside_row"]] == [0x100070]


def test_jal_targets_and_their_classes():
    words = {
        0x100020: jal_to(0x100060),  # row start
        0x100024: jal_to(0x100070),  # inside FUN_b
        0x100028: jal_to(0x100080 + 0x80),  # beyond the text
        0x10002C: jal_to(0x100000),  # row start (entry)
    }
    c = run(words)
    assert set(by_addr(c, "row_start")) == {0x100060, 0x100000}
    assert by_addr(c, "inside_row")[0x100070]["found_at"] == {"jal": 0x100024}
    assert by_addr(c, "inside_row")[0x100070]["row"] == "FUN_b"


def test_new_code_and_rejected():
    rows = [("FUN_a", 0x100000, 0x100040)]
    words = {
        0x100000: jal_to(0x100050), 0x100004: jal_to(0x100070), 0x100008: jal_to(0x100090),
        0x100050: LUI_V0_10, 0x100054: JR_RA, 0x100058: 0,  # good: 2 instructions decoded
        0x100070: LUI_V0_10, 0x100074: BAD, 0x100078: JR_RA,  # invalid word
        # 0x100090: zeros up to the end of the file data: no jr $ra within the limit / out of the file
    }
    c = run(words, rows=rows)
    nc = by_addr(c, "new_code")
    assert set(nc) == {0x100050} and nc[0x100050]["decoded"] == 2
    rj = by_addr(c, "rejected")
    assert set(rj) == {0x100070, 0x100090}
    assert rj[0x100070]["reason"] == "invalid word 0x00000055 at 0x00100074" and rj[0x100070]["decoded"] == 2
    assert rj[0x100090]["reason"].startswith("runs out of the file data")


def test_decode_run_limit():
    blob = make_blob({})
    ok, n, why = fcp.decode_run(blob, BASE, 0x100100)
    assert not ok and "file data" in why  # 0x300 bytes of zeros: ends before the limit
    big = struct.pack(f"<{fcp.MAX_DECODE + 8}I", *([0] * (fcp.MAX_DECODE + 8)))
    ok, n, why = fcp.decode_run(big, BASE, BASE)
    assert not ok and n == fcp.MAX_DECODE and why == f"no jr $ra within {fcp.MAX_DECODE} instructions"


def test_sources_are_merged_per_address():
    words = {0x100200: 0x100070, 0x100020: jal_to(0x100070), 0x100024: LUI_V0_10, 0x100028: ADDIU_V0_V0 | 0x70}
    c = run(words)
    e = by_addr(c, "inside_row")[0x100070]
    assert e["sources"] == ["data", "code_pair", "jal"]
    assert e["found_at"] == {"data": 0x100200, "code_pair": 0x100024, "jal": 0x100020}
    counts, by_source = fcp.count(c)
    assert counts["inside_row"] == 1
    assert by_source["jal"]["inside_row"] == 1 and by_source["data"]["row_start"] == 0


def test_outputs_respect_the_manual_reject_file(tmp_path):
    rows = [("FUN_a", 0x100000, 0x100040)]
    words = {0x100000: jal_to(0x100050), 0x100004: jal_to(0x100070), 0x100008: jal_to(0x100020),
             0x100050: JR_RA, 0x100070: BAD}
    c = run(words, rows=rows)
    manual = {0x100050: "manual: not a function", 0x100999: "stale"}
    extra, reject, held = fcp.build_outputs(c, manual)
    assert held == 1
    assert fcp.parse_addr_file(_write(tmp_path / "e.txt", extra)) == {0x100020: "inside_row FUN_a jal"}
    rej = fcp.parse_addr_file(_write(tmp_path / "r.txt", reject))
    assert rej[0x100050] == "manual: not a function" and rej[0x100999] == "stale"
    assert rej[0x100070].startswith("rejected jal: invalid word")
    assert extra.startswith("#") and reject.startswith("#")
    # regenerating from the file that was just written gives the same text
    again = fcp.build_outputs(c, rej)
    assert again[:2] == (extra, reject)


def _write(path, text):
    path.write_text(text)
    return path


def test_parse_addr_file_missing_and_comments(tmp_path):
    assert fcp.parse_addr_file(tmp_path / "none.txt") == {}
    p = _write(tmp_path / "a.txt", "# header\n\n0x00100010  # note # more\n0x100020\n")
    assert fcp.parse_addr_file(p) == {0x100010: "note # more", 0x100020: ""}


def test_read_layout(tmp_path):
    p = tmp_path / "layout.toml"
    p.write_text('[text]\nstart = 0x100000\nend = 0x100100\n'
                 '[[island]]\nstart = 0x100200\nend = 0x100300\nkind = "kernel_patch_blob"\n'
                 '[[island]]\nstart = 0x100300\nend = 0x100400\nkind = "false_positive"\n')
    assert fcp.read_layout(p) == (0x100000, 0x100100, [(0x100200, 0x100300)])


def test_main_end_to_end_is_idempotent(tmp_path):
    csv = tmp_path / "f.csv"
    csv.write_text("Name,Start,End,Size\n"
                   "entry,0x00100000,0x00100020,32\nFUN_a,0x00100020,0x00100060,64\nFUN_b,0x00100060,0x00100100,160\n"
                   "FUN_island,0x00100300,0x00100310,16\n")
    lay = tmp_path / "layout.toml"
    lay.write_text('[text]\nstart = 0x100000\nend = 0x100100\n')
    vu = tmp_path / "vu.tsv"
    vu.write_text("elf_addr\tvu_addr\tinstrs\tbytes\n0x100380\t0x000\t4\t16\n")
    elf = tmp_path / "game.elf"
    write_elf(elf, make_blob({0x100200: 0x100030, 0x100204: 0x100020, 0x100210: 0x100110 - 0x20,
                              0x100220: 0x100040, 0x100380: 0x100050,
                              0x100000: jal_to(0x100090), 0x100050: 0}))
    extra, reject, report = tmp_path / "extra.txt", tmp_path / "reject.txt", tmp_path / "out" / "r.json"
    reject.write_text("# manual\n0x00100040  # manual\n")
    argv = ["--csv", str(csv), "--layout", str(lay), "--elf", str(elf), "--vu", str(vu),
            "--extra", str(extra), "--reject", str(reject), "--report", str(report)]
    assert fcp.main(argv) == 0
    first = (extra.read_text(), reject.read_text(), report.read_text())
    d = json.loads(first[2])
    # 0x100050 sits in a VU program (skipped), 0x100300 is an island row (ignored)
    assert d["counts"] == {"row_start": 1, "inside_row": 4, "new_code": 0, "rejected": 0}
    assert d["counts_by_source"]["data"]["inside_row"] == 3
    assert d["held_back_by_reject_file"] == 1  # 0x100040 is listed by hand
    assert fcp.parse_addr_file(extra) == {0x100030: "inside_row FUN_a data", 0x1000F0: "inside_row FUN_b data",
                                          0x100090: "inside_row FUN_b jal"}
    assert fcp.parse_addr_file(reject) == {0x100040: "manual"}
    assert fcp.main(argv) == 0
    assert (extra.read_text(), reject.read_text(), report.read_text()) == first
