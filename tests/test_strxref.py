"""Tests for tools/re/strxref.py on a synthetic ELF32 file."""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "re"))

import strxref  # noqa: E402

BASE = 0x00100000
TEXT_END = BASE + 0x1000
PHDR_OFF = 0x100  # file offset of the PT_LOAD data


def lui(rt, hi):
    return (0x0F << 26) | (rt << 16) | hi


def addiu(rt, rs, imm):
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def ori(rt, rs, imm):
    return (0x0D << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


NOP = 0

# Code words at BASE + 4*index. Strings live at 0x108100, 0x108200 and 0x108300.
CODE = [
    lui(8, 0x0011),       # 0: $t0 = 0x00110000
    addiu(8, 8, 0x8100),  # 1: $t0 = 0x00110000 - 0x7F00 = 0x00108100 (sign-extended lo) -> "modules/0000.fpk"
    lui(4, 0x0010),       # 2: $a0 = 0x00100000
    NOP,                  # 3
    NOP,                  # 4
    ori(4, 4, 0x8300),    # 5: $a0 = 0x00108300 (ori does not sign-extend) -> "modules/1000.fpk"
    lui(5, 0x0010),       # 6: lui whose ori is 5 instructions later: not a pair
    NOP,                  # 7
    NOP,                  # 8
    NOP,                  # 9
    NOP,                  # 10
    ori(5, 5, 0x8100),    # 11
    lui(6, 0x0010),       # 12: next instruction writes another register: not a pair
    addiu(7, 7, 0x8100),  # 13
    lui(9, 0x0010),       # 14
    ori(9, 9, 0x8200),    # 15: $t1 = 0x00108200 -> "sce_not_a_match"
]

STRINGS = {
    0x8100: b"modules/0000.fpk\0",
    0x8200: b"sce_not_a_match\0",
    0x8300: b"modules/1000.fpk\0",
}


def make_elf(path):
    """Write an ELF32 little endian file: ELF header, one program header, then one PT_LOAD at BASE."""
    blob = bytearray(0x8400)
    struct.pack_into(f'<{len(CODE)}I', blob, 0, *CODE)
    for off, s in STRINGS.items():
        blob[off:off + len(s)] = s
    ehdr = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF' + bytes([1, 1, 1]) + bytes(9),
                       2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack('<8I', 1, PHDR_OFF, BASE, BASE, len(blob), len(blob), 5, PHDR_OFF)
    data = ehdr + phdr + bytes(PHDR_OFF - len(ehdr) - len(phdr)) + blob
    path.write_bytes(data)
    return path


def test_load_returns_pt_load_segment(tmp_path):
    entry, base, blob = strxref.load(make_elf(tmp_path / "game.elf"))
    assert entry == BASE
    assert base == BASE
    assert len(blob) == 0x8400
    assert blob[0x8100:0x8100 + 17] == b"modules/0000.fpk\0"


def test_find_references_pairs_lui_with_addiu_and_ori(tmp_path):
    _, base, blob = strxref.load(make_elf(tmp_path / "game.elf"))
    rx = re.compile(r"modules/.*\.fpk")
    refs = strxref.find_references(blob, base, TEXT_END, rx)
    assert refs == [
        (BASE + 0x0, 0x00108100, "-", "modules/0000.fpk"),
        (BASE + 0x8, 0x00108300, "-", "modules/1000.fpk"),
    ]


def test_find_references_applies_regex_filter(tmp_path):
    _, base, blob = strxref.load(make_elf(tmp_path / "game.elf"))
    refs = strxref.find_references(blob, base, TEXT_END, re.compile("sce"))
    assert refs == [(BASE + 0x38, 0x00108200, "-", "sce_not_a_match")]


def test_find_references_names_the_function(tmp_path):
    _, base, blob = strxref.load(make_elf(tmp_path / "game.elf"))
    functions = [("fn_a", BASE, BASE + 0x8), ("fn_b", BASE + 0x8, BASE + 0x40)]
    refs = strxref.find_references(blob, base, TEXT_END, re.compile("modules"), functions)
    assert [(pc, name) for pc, _, name, _ in refs] == [(BASE, "fn_a"), (BASE + 0x8, "fn_b")]


def test_read_functions_accepts_hex_with_and_without_prefix(tmp_path):
    csv_path = tmp_path / "functions.csv"
    csv_path.write_text("Name,Start,End,Size\n"
                        "fn_a,0x00100000,0x00100008,0x8\n"
                        "fn_b,00100008,00100040,38\n")
    assert strxref.read_functions(csv_path) == [
        ("fn_a", BASE, BASE + 0x8),
        ("fn_b", BASE + 0x8, BASE + 0x40),
    ]


def test_main_prints_one_line_per_reference(tmp_path, capsys):
    elf = make_elf(tmp_path / "game.elf")
    csv_path = tmp_path / "functions.csv"
    csv_path.write_text("Name,Start,End,Size\n"
                        "fn_a,0x00100000,0x00100008,0x8\n"
                        "fn_b,0x00100008,0x00100040,0x38\n")
    strxref.main(["--elf", str(elf), "--text-end", "0x101000", "--csv", str(csv_path), r"modules/.*\.fpk"])
    assert capsys.readouterr().out.splitlines() == [
        '0x00100000  0x00108100  fn_a  "modules/0000.fpk"',
        '0x00100008  0x00108300  fn_b  "modules/1000.fpk"',
    ]


def test_main_without_csv_prints_dash(tmp_path, capsys):
    elf = make_elf(tmp_path / "game.elf")
    strxref.main(["--elf", str(elf), "--text-end", "0x101000", r"modules/.*\.fpk"])
    assert capsys.readouterr().out.splitlines() == [
        '0x00100000  0x00108100  -  "modules/0000.fpk"',
        '0x00100008  0x00108300  -  "modules/1000.fpk"',
    ]
