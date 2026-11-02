#!/usr/bin/env python3
"""Eighting FPK packs: parse (PS2 little-endian or GameCube big-endian) and decompress entries.

Compression is PRS-like with control bits read MSB first:
  1            literal byte
  0 0 s1 s0    short copy: length = s + 2, one offset byte (negative, -256..-1)
  0 1          long copy: two bytes (hi, lo); offset = ((hi << 8 | lo) >> 3) - 0x2000,
               length = (lo & 7) + 2, or next byte + 1 when (lo & 7) == 0
There is no end marker: the stream stops when the unpacked size is reached.
"""
import struct


def parse(buf):
    """Return (endian, [(name, offset, packed, unpacked)]) or None."""
    for e in '<>':
        zero, count, hsize, total = struct.unpack_from(e + '4I', buf, 0)
        if zero == 0 and hsize == 0x10 and 0 < count < 4096 and total <= len(buf) + 0x800:
            ents = []
            for i in range(count):
                o = hsize + i * 0x30
                name = buf[o:o + 0x20].split(b'\0')[0].decode('ascii', 'replace')
                pad, off, psz, usz = struct.unpack_from(e + '4I', buf, o + 0x20)
                ents.append((name, off, psz, usz))
            return e, ents
    return None


def decompress(src, out_size):
    out = bytearray()
    p = 0
    bits = 0
    nbits = 0

    def bit():
        nonlocal p, bits, nbits
        if nbits == 0:
            bits = src[p]
            p += 1
            nbits = 8
        nbits -= 1
        return (bits >> nbits) & 1

    n = len(src)
    while len(out) < out_size and p < n:
        if bit():
            out.append(src[p])
            p += 1
            continue
        if bit():
            hi, lo = src[p], src[p + 1]
            p += 2
            off = (((hi << 8) | lo) >> 3) - 0x2000
            ln = lo & 7
            if ln == 0:
                ln = src[p] + 1
                p += 1
            else:
                ln += 2
        else:
            ln = (bit() << 1 | bit()) + 2
            off = src[p] - 0x100
            p += 1
        start = len(out) + off
        if start < 0:
            raise ValueError('bad back-reference')
        for k in range(ln):
            out.append(out[start + k])
    return bytes(out[:out_size])


def read_entry(buf, ent):
    name, off, psz, usz = ent
    data = buf[off:off + psz]
    return data if psz == usz else decompress(data, usz)
