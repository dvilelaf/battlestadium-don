#!/usr/bin/env python3
"""List Eighting FPK packs inside an AFS (or a standalone .FPK): inner names, sizes, extension census.

FPK header (LE, PS2): u32 0, u32 count, u32 header_size(0x10), u32 total_size
entry (0x30):        char name[0x20], u32 0?, u32 offset, u32 packed_size, u32 unpacked_size

Usage: fpk_survey.py <file.afs|file.fpk> [-v]
"""
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # python -I drops the script dir
from afs_survey import read_afs  # noqa: E402


def parse_fpk(buf):
    zero, count, hsize, total = struct.unpack_from('<4I', buf, 0)
    if zero != 0 or hsize != 0x10 or count > 4096:
        return None
    ents = []
    for i in range(count):
        o = hsize + i * 0x30
        name = buf[o:o + 0x20].split(b'\0')[0].decode('ascii', 'replace')
        pad, off, psz, usz = struct.unpack_from('<4I', buf, o + 0x20)
        ents.append((name, off, psz, usz))
    return total, ents


def main():
    path = sys.argv[1]
    verbose = '-v' in sys.argv
    packs = []
    if open(path, 'rb').read(4) == b'AFS\x00':
        f, ents, names, _ = read_afs(path)
        for i, (off, size) in enumerate(ents):
            f.seek(off)
            packs.append((f'{path}#{i}', f.read(size)))
    else:
        packs.append((path, open(path, 'rb').read()))
    ext = Counter()
    ext_bytes = Counter()
    stored = packed = 0
    nfiles = 0
    for label, buf in packs:
        r = parse_fpk(buf)
        if not r:
            print(label, 'NOT FPK', buf[:16].hex(' '))
            continue
        total, ents = r
        if verbose:
            print(f'{label}: {len(ents)} files, total={total} (blob {len(buf)})')
        for name, off, psz, usz in ents:
            e = name.rsplit('.', 1)[-1].lower() if '.' in name else '(none)'
            ext[e] += 1
            ext_bytes[e] += usz
            nfiles += 1
            if psz == usz:
                stored += 1
            else:
                packed += 1
            if verbose:
                print(f'    {name:34} off=0x{off:07X} packed={psz:8} unpacked={usz:8}')
    print(f'{len(packs)} packs, {nfiles} files; packed={packed} stored={stored}')
    print('extension census (count, unpacked bytes):')
    for e, n in ext.most_common():
        print(f'  .{e:8} {n:5}  {ext_bytes[e] / 1024:10.0f} KB')


if __name__ == '__main__':
    main()
