"""Tests of tools/afs_survey.py with synthetic data (they do not need the game)."""
import struct

import pytest

import afs_survey


def _record(name, date):
    # 0x30 bytes: char name[0x20], 6 x u16 date, u32 padding
    y, mo, d, h, mi, s = date
    return name.encode().ljust(0x20, b"\0") + struct.pack("<6HI", y, mo, d, h, mi, s, 0)


def _pack(entries, names):
    # header, entry table, name pointer, data at 0x800 and name table after the data
    count = len(entries)
    buf = bytearray(b"AFS\0" + struct.pack("<I", count))
    data_off = 0x800
    layout = []
    for payload in entries:
        layout.append((data_off, len(payload)))
        data_off += (len(payload) + 0x7FF) & ~0x7FF
    name_off = data_off
    name_size = count * 0x30
    for off, size in layout:
        buf += struct.pack("<II", off, size)
    buf += struct.pack("<II", name_off, name_size)
    buf = buf.ljust(0x800, b"\0")
    for payload in entries:
        chunk = payload.ljust((len(payload) + 0x7FF) & ~0x7FF, b"\0")
        buf += chunk
    buf = buf.ljust(name_off, b"\0")
    for nm, date in names:
        buf += _record(nm, date)
    return bytes(buf)


def test_read_afs_lists_offsets_sizes_and_names(tmp_path):
    path = tmp_path / "synthetic.afs"
    path.write_bytes(
        _pack(
            [b"hello", b"abc"],
            [("A.TXT", (2001, 2, 3, 4, 5, 6)), ("B.DAT", (2010, 12, 31, 23, 59, 58))],
        )
    )
    f, ents, names, nt = afs_survey.read_afs(path)
    try:
        assert ents == [(0x800, 5), (0x1000, 3)]
        assert names == [
            ("A.TXT", "2001-02-03 04:05:06"),
            ("B.DAT", "2010-12-31 23:59:58"),
        ]
        assert nt == (0x1800, 0x60)
    finally:
        f.close()


def test_read_afs_rejects_other_data(tmp_path):
    path = tmp_path / "not_afs.bin"
    path.write_bytes(b"FPK\0" + bytes(60))
    with pytest.raises(SystemExit):
        afs_survey.read_afs(path)
