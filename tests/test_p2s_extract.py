"""Tests for tools/ref/p2s_extract.py on synthetic savestates (ZIP files built here by hand).

Zstandard entries (ZIP method 93, the PCSX2 v2.8.2 default) are covered when an encoder is
available (`zstandard` package or the `zstd` tool); those tests are skipped otherwise.
"""
import json
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "ref"))

import p2s_extract  # noqa: E402

BASE = 0x00100000
TEXT_END = BASE + 0x100
EE_SIZE = 32 * 1024 * 1024


def zstd_compress(data):
    try:
        import zstandard
        c = zstandard.ZstdCompressor().compressobj()  # streaming: no content size in the frame, like libzip
        return c.compress(data) + c.flush()
    except ImportError:
        pass
    exe = shutil.which('zstd')
    if exe:
        return subprocess.run([exe, '-c', '-q'], input=data, capture_output=True, check=True).stdout
    return None


needs_zstd = pytest.mark.skipif(zstd_compress(b'x') is None or p2s_extract.zstd_backend() is None,
                                reason='no Zstandard encoder/decoder (zstandard package or zstd tool)')


def write_zip(path, entries):
    """entries: list of (name, data, method) with method 0, 8 or 93."""
    out = bytearray()
    central = bytearray()
    for name, data, method in entries:
        if method == 0:
            raw = data
        elif method == 8:
            c = zlib.compressobj(wbits=-15)
            raw = c.compress(data) + c.flush()
        else:
            raw = zstd_compress(data)
        nb = name.encode()
        crc = zlib.crc32(data)
        off = len(out)
        out += struct.pack('<4sHHHHHIIIHH', b'PK\x03\x04', 20, 0, method, 0, 0x21, crc, len(raw), len(data), len(nb), 0)
        out += nb + raw
        central += struct.pack('<4sHHHHHHIIIHHHHHII', b'PK\x01\x02', 20, 20, 0, method, 0, 0x21, crc, len(raw),
                               len(data), len(nb), 0, 0, 0, 0, 0, off) + nb
    cd_off = len(out)
    out += central
    out += struct.pack('<4sHHHHIIH', b'PK\x05\x06', 0, 0, len(entries), len(entries), len(central), cd_off, 0)
    Path(path).write_bytes(bytes(out))


def lui(rt, hi):
    return (0x0F << 26) | (rt << 16) | hi


JAL_TARGET = 0x100040
# Text of the synthetic executable: a call at 0x100000 (return address 0x100008), the rest NOPs.
CODE = [(0x03 << 26) | (JAL_TARGET >> 2), 0, 0, lui(8, 0x0010)] + [0] * 60  # 64 words = 0x100 bytes


def make_elf(path, blob_extra=None):
    """ELF32 LE with one PT_LOAD at BASE: the 0x100-byte text, then 0x700 bytes of data."""
    blob = bytearray(0x800)
    struct.pack_into(f'<{len(CODE)}I', blob, 0, *CODE)
    for off, b in (blob_extra or {}).items():
        blob[off:off + len(b)] = b
    ehdr = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF' + bytes([1, 1, 1]) + bytes(9),
                       2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack('<8I', 1, 0x100, BASE, BASE, len(blob), len(blob), 5, 0x100)
    Path(path).write_bytes(ehdr + phdr + bytes(0x100 - len(ehdr) - len(phdr)) + blob)
    return bytes(blob)


def make_layout(path):
    Path(path).write_text(f'[text]\nstart = 0x{BASE:08X}\nend = 0x{TEXT_END:08X}\n')


def components(blob, ee_patch=None):
    """Entries of a PCSX2 v2.8.2 savestate with the real sizes, in the order PCSX2 writes them."""
    ee = bytearray(EE_SIZE)
    ee[BASE:BASE + len(blob)] = blob
    for off, v in (ee_patch or {}).items():
        struct.pack_into('<I', ee, off, v)
    version = struct.pack('<I', 0x9A590000) + b'v2.8.2'.ljust(32, b'\0')
    return {
        'PCSX2 Savestate Version.id': version,
        'PCSX2 Internal Structures.dat': b'\x01' * 100,
        'eeMemory.bin': bytes(ee),
        'iopMemory.bin': bytes(2 * 1024 * 1024),
        'eeHwRegs.bin': bytes(0x10000),
        'iopHwRegs.bin': bytes(0x10000),
        'Scratchpad.bin': bytes(0x4000),
        'vu0Memory.bin': bytes(0x1000),
        'vu1Memory.bin': bytes(0x4000),
        'vu0MicroMem.bin': bytes(0x1000),
        'vu1MicroMem.bin': bytes(0x4000),
        'SPU2.bin': b'\x02' * 64,
        'PAD.bin': b'\x03' * 32,
        'GS.bin': struct.pack('<I', 9) + b'\x04' * 60,
        'Screenshot.png': b'\x89PNG fake',
    }


def build(tmp_path, comps, methods=None, name='state.p2s'):
    methods = methods or {}
    entries = []
    for n, d in comps.items():
        m = methods.get(n, 0 if n == 'PCSX2 Savestate Version.id' or n == 'Screenshot.png' else 8)
        entries.append((n, d, m))
    p = tmp_path / name
    write_zip(p, entries)
    return p


def setup(tmp_path, ee_patch=None, extra=None, vu1=None):
    elf = tmp_path / 'elf.bin'
    blob = make_elf(elf, extra)
    lay = tmp_path / 'layout.toml'
    make_layout(lay)
    comps = components(blob, ee_patch)
    if vu1 is not None:
        comps['vu1MicroMem.bin'] = vu1
    return elf, lay, comps


def run(capsys, *argv):
    rc = p2s_extract.main([str(a) for a in argv])
    return rc, capsys.readouterr()


def test_extract_deflate_and_stored(tmp_path, capsys):
    elf, lay, comps = setup(tmp_path)
    p = build(tmp_path, comps)
    out = tmp_path / 'out'
    rc, cap = run(capsys, p, out, '--elf', elf, '--layout', lay)
    assert rc == 0
    assert (out / 'ee_ram.bin').stat().st_size == EE_SIZE
    assert (out / 'iop_ram.bin').stat().st_size == 2 * 1024 * 1024
    assert (out / 'vu1_micro.bin').stat().st_size == 0x4000
    assert (out / 'vu1_mem.bin').stat().st_size == 0x4000
    assert (out / 'vu0_micro.bin').stat().st_size == 0x1000
    assert (out / 'vu0_mem.bin').stat().st_size == 0x1000
    assert (out / 'scratchpad.bin').stat().st_size == 0x4000
    assert (out / 'gs.bin').read_bytes() == comps['GS.bin']
    assert (out / 'ee_ram.bin').read_bytes() == comps['eeMemory.bin']
    m = json.loads((out / 'manifest.json').read_text())
    assert m['savestate_version'] == {'save_version': '0x9A590000', 'major': 0x9A59, 'minor': 0, 'pcsx2': 'v2.8.2'}
    assert m['gs_state_version'] == 9
    by = {e['name']: e for e in m['entries']}
    assert len(by) == len(comps)
    assert by['eeMemory.bin']['method_name'] == 'deflate' and by['eeMemory.bin']['size'] == EE_SIZE
    assert by['PCSX2 Savestate Version.id']['method_name'] == 'store'
    assert by['eeMemory.bin']['stored_size'] < EE_SIZE
    assert m['code_range_check']['differing_bytes'] == 0
    assert '0 differing bytes' in cap.out


def test_code_range_difference_is_counted(tmp_path, capsys):
    elf, lay, comps = setup(tmp_path, ee_patch={BASE + 0x20: 0x12345678})
    p = build(tmp_path, comps)
    out = tmp_path / 'out'
    rc, _ = run(capsys, p, out, '--elf', elf, '--layout', lay)
    assert rc == 0
    m = json.loads((out / 'manifest.json').read_text())
    assert m['code_range_check']['differing_bytes'] == 4  # 0x12345678 replaces 4 zero bytes


@needs_zstd
def test_extract_zstandard_entries(tmp_path, capsys):
    elf, lay, comps = setup(tmp_path)
    p = build(tmp_path, comps, methods={n: 93 for n in comps if n not in ('PCSX2 Savestate Version.id',
                                                                             'Screenshot.png')})
    out = tmp_path / 'out'
    rc, _ = run(capsys, p, out, '--elf', elf, '--layout', lay)
    assert rc == 0
    assert (out / 'ee_ram.bin').read_bytes() == comps['eeMemory.bin']
    assert (out / 'gs.bin').read_bytes() == comps['GS.bin']
    m = json.loads((out / 'manifest.json').read_text())
    by = {e['name']: e for e in m['entries']}
    assert by['eeMemory.bin']['method'] == 93 and by['eeMemory.bin']['method_name'] == 'zstd'
    assert by['PCSX2 Savestate Version.id']['method_name'] == 'store'


def test_zstandard_without_decoder_says_what_to_do(tmp_path, capsys, monkeypatch):
    monkeypatch.setitem(sys.modules, 'zstandard', None)  # makes `import zstandard` raise ImportError
    monkeypatch.setitem(sys.modules, 'compression', None)
    monkeypatch.setattr(p2s_extract.shutil, 'which', lambda _: None)
    p = tmp_path / 'z.p2s'
    entries = [('PCSX2 Savestate Version.id', struct.pack('<I', 1) + bytes(32), 0), ('GS.bin', b'abc', 8)]
    write_zip(p, entries)
    # rewrite the GS entry as method 93 with bytes that no decoder will ever see
    raw = p.read_bytes().replace(b'PK\x03\x04\x14\x00\x00\x00\x08\x00', b'PK\x03\x04\x14\x00\x00\x00\x5d\x00')
    raw = raw.replace(b'PK\x01\x02\x14\x00\x14\x00\x00\x00\x08\x00', b'PK\x01\x02\x14\x00\x14\x00\x00\x00\x5d\x00')
    p.write_bytes(raw)
    rc, cap = run(capsys, p, tmp_path / 'out')
    assert rc == 1
    assert 'Zstandard' in cap.err and 'zstandard' in cap.err


def test_corrupt_entry_is_rejected(tmp_path, capsys):
    elf, lay, comps = setup(tmp_path)
    comps = {'PCSX2 Savestate Version.id': comps['PCSX2 Savestate Version.id'], 'GS.bin': b'0123456789'}
    p = build(tmp_path, comps, methods={'GS.bin': 0})
    raw = bytearray(p.read_bytes())
    i = raw.index(b'0123456789')
    raw[i] ^= 0xFF
    p.write_bytes(bytes(raw))
    rc, cap = run(capsys, p, tmp_path / 'out')
    assert rc == 1 and 'CRC mismatch' in cap.err


def test_not_a_zip(tmp_path, capsys):
    p = tmp_path / 'junk.p2s'
    p.write_bytes(b'not a zip file at all' * 10)
    rc, cap = run(capsys, p, tmp_path / 'out')
    assert rc == 1 and 'not a ZIP' in cap.err


def extract_state(tmp_path, capsys, ee_patch=None, extra=None, vu1=None):
    elf, lay, comps = setup(tmp_path, ee_patch, extra, vu1)
    p = build(tmp_path, comps)
    out = tmp_path / 'out'
    rc, _ = run(capsys, p, out, '--elf', elf, '--layout', lay)
    assert rc == 0
    return elf, lay, out


def test_code_pointers_classification(tmp_path, capsys):
    heap = 0x00200000
    patch = {
        heap + 0x00: 0x00100040,   # start of a row
        heap + 0x04: 0x00100060,   # in the extra entry-point file
        heap + 0x08: 0x00100048,   # inside a row, not covered
        heap + 0x0C: 0x00100008,   # return address of the jal at 0x100000
        heap + 0x10: 0x001000C0,   # in no row
        heap + 0x14: 0x00100042,   # not a multiple of 4
        heap + 0x18: 0x00500000,   # outside the text
        heap + 0x1C: 0x00100048,   # repeated value
        BASE + 0x400: 0x00100050,  # inside the image of the executable: must be ignored
    }
    elf, lay, out = extract_state(tmp_path, capsys, ee_patch=patch)
    csv = tmp_path / 'f.csv'
    csv.write_text('Name,Start,End,Size\nf1,0x00100000,0x00100040,64\nf2,0x00100040,0x001000A0,96\n'
                   .replace('0x', ''))
    extra = tmp_path / 'extra.txt'
    extra.write_text('# comment\n0x00100060  # inside f2\n')
    rc, cap = run(capsys, 'code-pointers', out, '--layout', lay, '--csv', csv, '--elf', elf,
                  '--extra', extra, '--show-returns', '--tsv', tmp_path / 'cp.tsv')
    assert rc == 0
    o = cap.out
    assert '5 distinct' in o and '6 occurrences' in o
    assert 'start of a row:                        1' in o
    assert 'listed in the entry-point files:       1' in o
    assert 'not covered, inside a row:             2' in o
    assert 'not covered, in no row:                1' in o
    assert 'return addresses   1' in o
    assert 'others             2' in o
    assert '0x00100048  inside_row  -   0x00200008  2' in o
    assert 'entry-like' in o
    tsv_lines = (tmp_path / 'cp.tsv').read_text().splitlines()
    assert tsv_lines[0].split('\t')[:5] == ['address', 'class', 'row', 'flags', 'return_address']
    assert '0x00100048\tinside_row\tf2\t-\t0\t0x00200008\t2' in tsv_lines
    assert any(l.startswith('0x00100008\tinside_row\tf1\t') and l.split('\t')[4] == '1' for l in tsv_lines)
    assert '0x001000C0  no_row' in o
    assert '0x00100050' not in o
    assert '0x00100008' in o  # listed under the return addresses
    assert '0x00100042' not in o


def mpg(addr, instrs):
    return struct.pack('<I', (0x4A << 24) | (len(instrs) << 16) | addr) + b''.join(instrs)


def ins(n):
    return struct.pack('<II', n, 0x1000 + n)


def test_vu1_program_loaded_partial_and_missing(tmp_path, capsys):
    prog_a = [ins(1), ins(2), ins(3)]
    prog_b = [ins(10), ins(11), ins(12)]
    prog_c = [ins(20), ins(21)]
    stream_a = mpg(0x10, prog_a) + bytes(4)
    stream_b = mpg(0x20, prog_b) + bytes(4)
    stream_c = mpg(0x30, prog_c) + bytes(4)
    extra = {0x400: stream_a, 0x440: stream_b, 0x480: stream_c}
    micro = bytearray(0x4000)
    micro[0x10 * 8:0x13 * 8] = b''.join(prog_a)                       # A in full
    micro[0x20 * 8:0x22 * 8] = b''.join(prog_b[:2])                   # B: first two instructions only
    micro[0x22 * 8:0x23 * 8] = ins(99)                                # ...then something else
    elf, lay, out = extract_state(tmp_path, capsys, extra=extra, vu1=bytes(micro))
    tsv = tmp_path / 'vu.tsv'
    tsv.write_text('elf_addr\tvu_addr\tinstrs\tbytes\n'
                   f'0x{BASE + 0x400:X}\t0x010\t3\t{len(stream_a)}\n'
                   f'0x{BASE + 0x440:X}\t0x020\t3\t{len(stream_b)}\n'
                   f'0x{BASE + 0x480:X}\t0x030\t2\t{len(stream_c)}\n')
    rc, cap = run(capsys, 'vu1-program', out, '--programs', tsv, '--elf', elf, '--min-gain', '2')
    assert rc == 0
    o = cap.out
    assert '3 programs: 1 loaded in full, 1 partial' in o and '1 not found' in o
    assert 'warning' not in o
    assert 'VU1 code memory: 2048 instructions, 6 not zero' in o
    assert 'same address: 5 (0.2% of the memory)' in o
    assert f'0x{BASE + 0x400:X}  0x010  3\n' in o
    assert f'0x{BASE + 0x440:X}  0x020  3  2  0.67' in o
    # decomposition: A explains 3, B explains 2, C nothing
    dec = o.split('Decomposition')[1]
    assert f'0x{BASE + 0x400:X}  0x010  3  3  3' in dec and f'0x{BASE + 0x440:X}  0x020  3  2  2' in dec
    assert f'0x{BASE + 0x480:X}' not in dec


def test_vu1_program_needs_extracted_files(tmp_path, capsys):
    rc, cap = run(capsys, 'vu1-program', tmp_path / 'nothing')
    assert rc == 1 and 'not found' in cap.err
