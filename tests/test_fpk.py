"""Tests of tools/fpk.py with synthetic data (they do not need the game)."""
import struct

import fpk


def test_literals_and_short_copy():
    # control 0xC3 = 1 1 00 00 1 1: literal, literal, short copy (len 2, off -1), literal, literal
    src = bytes([0xC3, 0x10, 0x00, 0xFF, 0xD7, 0x56])
    assert fpk.decompress(src, 6) == bytes([0x10, 0x00, 0x00, 0x00, 0xD7, 0x56])


def test_long_copy_with_inline_length():
    # 8 literals and a long copy: off -8, len (6 + 2) = 8
    src = bytes([0xFF]) + b"ABCDEFGH" + bytes([0x40, 0xFF, 0xC6])
    assert fpk.decompress(src, 16) == b"ABCDEFGHABCDEFGH"


def test_long_copy_with_length_byte():
    # literal 'A' and a long copy: off -1, (lo & 7) == 0, length = 0x13 + 1 = 20
    src = bytes([0xA0, 0x41, 0xFF, 0xF8, 0x13])
    assert fpk.decompress(src, 21) == b"A" * 21


def test_zero_offset_bytes_are_a_copy_not_an_end_marker():
    # hi = lo = 0 is a valid copy from 0x2000 bytes back, not an end of stream
    head = bytes(range(256)) * 32  # 0x2000 bytes
    packed = b"".join(bytes([0xFF]) + head[i:i + 8] for i in range(0, len(head), 8))
    packed += bytes([0x40, 0x00, 0x00, 0x03])  # long copy: off -0x2000, len 3 + 1 = 4
    assert fpk.decompress(packed, len(head) + 4) == head + head[:4]


def _pack(endian, name, payload):
    entry = name.encode().ljust(0x20, b"\0") + struct.pack(endian + "4I", 0, 0x40, len(payload), len(payload))
    return struct.pack(endian + "4I", 0, 1, 0x10, 0x40 + len(payload)) + entry + payload


def test_parse_little_and_big_endian():
    for endian in "<>":
        buf = _pack(endian, "dir/file.bin", b"hello")
        got_endian, entries = fpk.parse(buf)
        assert got_endian == endian
        assert entries == [("dir/file.bin", 0x40, 5, 5)]
        assert fpk.read_entry(buf, entries[0]) == b"hello"


def test_parse_rejects_other_data():
    assert fpk.parse(b"AFS\0" + bytes(64)) is None
