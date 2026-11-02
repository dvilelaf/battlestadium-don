import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 're'))

import callers  # noqa: E402

BASE = 0x100000


def jal(target):
    return (3 << 26) | ((target >> 2) & 0x03FFFFFF)


def make_elf(path, words):
    data = struct.pack(f'<{len(words)}I', *words)
    phoff = 0x34
    hdr = bytearray(0x34)
    hdr[:4] = b'\x7fELF'
    hdr[4], hdr[5] = 1, 1
    struct.pack_into('<I', hdr, 0x1C, phoff)
    struct.pack_into('<HH', hdr, 0x2A, 32, 1)
    off = phoff + 32
    ph = struct.pack('<8I', 1, off, BASE, BASE, len(data), len(data), 5, 0)
    path.write_bytes(bytes(hdr) + ph + data)


NOP = 0
# index: 0 jal 0x100020, 1 nop, 2 jal 0x100020, 3 jal 0x100010, 4..7 nop, 8 jal 0x100020 (outside range below)
WORDS = [jal(0x100020), NOP, jal(0x100020), jal(0x100010), NOP, NOP, NOP, NOP, jal(0x100020)]


def test_find_callers(tmp_path):
    elf = tmp_path / 'a.elf'
    make_elf(elf, WORDS)
    base, blob = callers.load(str(elf))
    assert base == BASE
    assert callers.find_callers(blob, base, BASE, BASE + 0x24, 0x100020) == [0x100000, 0x100008, 0x100020]
    assert callers.find_callers(blob, base, BASE, BASE + 0x20, 0x100020) == [0x100000, 0x100008]
    assert callers.find_callers(blob, base, BASE, BASE + 0x24, 0x100030) == []


def test_containing_prefers_function_over_label():
    funcs = [('outer', 0x100000, 0x100040), ('entry_100000', 0x100000, 0x100004), ('inner', 0x100008, 0x100010)]
    assert callers.containing(funcs, 0x100000) == ('outer', 'entry_100000')
    assert callers.containing(funcs, 0x10000C) == ('inner', None)
    assert callers.containing(funcs, 0x100100) == (None, None)


def test_main_output(tmp_path, capsys):
    elf = tmp_path / 'a.elf'
    make_elf(elf, WORDS)
    csv_path = tmp_path / 'f.csv'
    csv_path.write_text('Name,Start,End,Size\nfa,0x00100000,0x00100008,8\nfb,0x00100008,0x00100024,28\n')
    layout = tmp_path / 'layout.toml'
    layout.write_text('[text]\nstart = 0x100000\nend = 0x100020\n')
    callers.main(['--elf', str(elf), '--target', '0x100020', '--csv', str(csv_path), '--layout', str(layout)])
    assert capsys.readouterr().out.splitlines() == ['0x00100000  fa', '0x00100008  fb']
    callers.main(['--elf', str(elf), '--target', '0x100030'])
    assert capsys.readouterr().out.strip() == '0 calls'
