#!/usr/bin/env python3
"""Read a GameCube disc stored as RVZ (zstd only): list the file system and extract files.

Junk regions are returned as zeros (the lagged-Fibonacci padding is not regenerated), which is
fine for reading real files.

Usage: gc_rvz.py <disc.rvz> list
       gc_rvz.py <disc.rvz> extract <outdir> <pattern> [<pattern>...]   (fnmatch on the full path; "@dol" = main.dol)
"""
import fnmatch
import os
import struct
import sys

import zstandard


class Rvz:
    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x48)
        if head[:4] != b'RVZ\x01':
            raise SystemExit('not an RVZ file')
        disc_size, = struct.unpack_from('>I', head, 0x0C)
        self.iso_size, = struct.unpack_from('>Q', head, 0x24)
        disc = self.f.read(disc_size)
        self.disc_type, self.compression, self.level, self.chunk = struct.unpack_from('>IiiI', disc, 0)
        if self.compression != 5:
            raise SystemExit(f'compression {self.compression} not supported (only zstd=5)')
        self.dhead = disc[0x10:0x90]
        n_raw, raw_off, raw_size = struct.unpack_from('>IQI', disc, 0xB4)
        n_groups, group_off, group_size = struct.unpack_from('>IQI', disc, 0xC4)
        self.dctx = zstandard.ZstdDecompressor()
        raw = self._zread(raw_off, raw_size, n_raw * 0x18)
        self.raw = [struct.unpack_from('>QQII', raw, i * 0x18) for i in range(n_raw)]
        groups = self._zread(group_off, group_size, n_groups * 12)
        self.groups = [struct.unpack_from('>III', groups, i * 12) for i in range(n_groups)]
        self.cache = {}

    def _zread(self, off, size, out_size):
        self.f.seek(off)
        return self.dctx.decompress(self.f.read(size), max_output_size=out_size)

    def _group(self, g):
        if g in self.cache:
            return self.cache[g]
        off4, size, packed = self.groups[g]
        n = size & 0x7FFFFFFF
        if n == 0:
            data = bytes(self.chunk)
        else:
            self.f.seek(off4 * 4)
            data = self.f.read(n)
            if size & 0x80000000:
                data = self.dctx.decompress(data, max_output_size=max(packed, self.chunk) + 0x1000)
            if packed:
                out = bytearray()
                p = 0
                while p < len(data):
                    s, = struct.unpack_from('>I', data, p)
                    p += 4
                    if s & 0x80000000:
                        out += bytes(s & 0x7FFFFFFF)
                        p += 68
                    else:
                        out += data[p:p + s]
                        p += s
                data = bytes(out)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[g] = data
        return data

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            if off < 0x80:
                piece = self.dhead[off:min(0x80, off + size)]
            else:
                for r_off, r_size, g0, ng in self.raw:
                    start = r_off & ~0x7FFF
                    if start <= off < r_off + r_size:
                        rel = off - start
                        data = self._group(g0 + rel // self.chunk)
                        piece = data[rel % self.chunk:rel % self.chunk + size]
                        break
                else:
                    raise SystemExit(f'offset 0x{off:X} not covered')
            if not piece:
                raise SystemExit(f'short read at 0x{off:X}')
            out += piece
            off += len(piece)
            size -= len(piece)
        return bytes(out)


def fst(disc):
    dol_off, fst_off, fst_size = struct.unpack('>III', disc.read(0x420, 12))
    data = disc.read(fst_off, fst_size)
    n, = struct.unpack_from('>I', data, 8)
    names = n * 12
    files = []
    stack = [(n, '')]
    i = 1
    while i < n:
        while stack and i >= stack[-1][0]:
            stack.pop()
        w, a, b = struct.unpack_from('>III', data, i * 12)
        end = data.index(b'\0', names + (w & 0xFFFFFF))
        name = data[names + (w & 0xFFFFFF):end].decode('ascii', 'replace')
        path = stack[-1][1] + name
        if w >> 24:
            stack.append((b, path + '/'))
        else:
            files.append((path, a, b))
        i += 1
    return dol_off, files


def dol_size(disc, off):
    h = disc.read(off, 0x100)
    offs = struct.unpack_from('>18I', h, 0)
    sizes = struct.unpack_from('>18I', h, 0x90)
    return max(o + s for o, s in zip(offs, sizes))


def main():
    disc = Rvz(sys.argv[1])
    cmd = sys.argv[2]
    dol_off, files = fst(disc)
    print(f'id={disc.dhead[:6].decode()} name={disc.dhead[0x20:0x40].split(bytes(1))[0].decode("ascii", "replace")} '
          f'iso_size={disc.iso_size} chunk=0x{disc.chunk:X} files={len(files)} dol@0x{dol_off:X} size={dol_size(disc, dol_off)}')
    if cmd == 'list':
        for path, off, size in files:
            print(f'{size:11} 0x{off:08X} {path}')
    elif cmd == 'extract':
        out = sys.argv[3]
        pats = sys.argv[4:]
        os.makedirs(out, exist_ok=True)
        if '@dol' in pats:
            open(os.path.join(out, 'main.dol'), 'wb').write(disc.read(dol_off, dol_size(disc, dol_off)))
            print('extracted main.dol')
        for path, off, size in files:
            if any(fnmatch.fnmatch(path.lower(), p.lower()) for p in pats):
                dst = os.path.join(out, path.replace('/', '__'))
                open(dst, 'wb').write(disc.read(off, size))
                print('extracted', path, size)


if __name__ == '__main__':
    main()
