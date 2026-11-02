#!/usr/bin/env python3
"""List an AFS archive (CRI) and fingerprint each entry by its leading bytes.

Usage: afs_survey.py <file.afs> [--dump N outdir]
"""
import struct
import sys
from collections import Counter


def read_afs(path):
    f = open(path, 'rb')
    head = f.read(8)
    if head[:4] != b'AFS\x00':
        raise SystemExit(f'{path}: not AFS (magic {head[:4]!r})')
    count, = struct.unpack('<I', head[4:])
    toc = f.read(count * 8 + 8)
    ents = [struct.unpack_from('<II', toc, i * 8) for i in range(count)]
    name_off, name_size = struct.unpack_from('<II', toc, count * 8)
    names = []
    if name_off and name_size >= count * 0x30:
        f.seek(name_off)
        blob = f.read(count * 0x30)
        for i in range(count):
            rec = blob[i * 0x30:(i + 1) * 0x30]
            nm = rec[:0x20].split(b'\0')[0].decode('ascii', 'replace')
            y, mo, d, h, mi, s = struct.unpack_from('<6H', rec, 0x20)
            names.append((nm, f'{y:04}-{mo:02}-{d:02} {h:02}:{mi:02}:{s:02}'))
    return f, ents, names, (name_off, name_size)


def main():
    path = sys.argv[1]
    f, ents, names, nt = read_afs(path)
    print(f'{path}: {len(ents)} entries, name table at 0x{nt[0]:X} size {nt[1]}')
    magics = Counter()
    exts = Counter()
    for i, (off, size) in enumerate(ents):
        f.seek(off)
        head = f.read(16)
        nm, ts = names[i] if names else ('', '')
        magic = head[:4]
        magics[magic] += 1
        exts[nm.rsplit('.', 1)[-1].lower() if '.' in nm else '(none)'] += 1
        if i < 40 or i >= len(ents) - 5:
            print(f'  {i:5} off=0x{off:08X} size={size:9} {nm:32} {ts}  {head.hex(" ")}')
        elif i == 40:
            print('  ...')
    print('extensions:', dict(exts.most_common()))
    print('leading 4 bytes:', {m.hex() + '/' + ''.join(chr(b) if 32 <= b < 127 else '.' for b in m): n for m, n in magics.most_common(25)})
    print('total payload:', sum(s for _, s in ents))
    if len(sys.argv) >= 5 and sys.argv[2] == '--dump':
        import os
        n = int(sys.argv[3])
        out = sys.argv[4]
        os.makedirs(out, exist_ok=True)
        off, size = ents[n]
        f.seek(off)
        nm = names[n][0] if names else f'{n:05}.bin'
        open(os.path.join(out, f'{n:05}_{os.path.basename(nm) or "bin"}'), 'wb').write(f.read(size))
        print('dumped', n, nm)


if __name__ == '__main__':
    main()
