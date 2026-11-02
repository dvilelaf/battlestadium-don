#!/usr/bin/env python3
"""Rough map of which middleware owns which part of the text, from string and constant anchors.

Usage: layout_survey.py <elf> [analyzer_config.toml]
"""
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # python -I drops the script dir
from elf_survey import load  # noqa: E402

TEXT_HI = 0x3902D4  # end of the main text (config/layout.toml)
BUCKET = 0x10000
CLASSES = [
    ('cri', re.compile(r'ADX|adx|CRI|SFD|mwPly|^E0\d{5,}|SJ[/ ]|CVFS|DTX|DVCI|M2V|MPV|LSC|lsc_|Sofdec|sfd')),
    ('sce', re.compile(r'^sce|Sce[A-Z]|lib(pad|mc|cdvd|dma|graph|kernl|ipu|mtap|sdr)|Libcdvd|PsII|rom0:|MPEG|macroblock')),
    ('rw', re.compile(r'\.csl$')),
    ('game', re.compile(r'fpack|snd/|chr/|stg/|%4\.2f|TASK|MEMORY|OSMEM|\.fpk|\.dat$|_ps2\.|_cmp\.|modules/|host0:|Tsk|battle')),
]


def cstring(blob, off):
    end = blob.find(b'\0', off)
    if end < 0 or end - off < 5 or end - off > 200:
        return None
    s = blob[off:end]
    if all(32 <= b < 127 for b in s):
        return s.decode('ascii')
    return None


def main():
    entry, base, blob, msz = load(sys.argv[1])
    n = len(blob) // 4
    words = struct.unpack_from(f'<{n}I', blob)
    ti = (TEXT_HI - base) // 4
    hits = defaultdict(list)
    for i in range(ti):
        w = words[i]
        if w >> 26 != 0x0F:
            continue
        rt = (w >> 16) & 0x1F
        hi = w & 0xFFFF
        for j in range(i + 1, min(i + 5, ti)):
            x = words[j]
            op = x >> 26
            if op in (0x09, 0x0D) and (x >> 21) & 0x1F == rt and (x >> 16) & 0x1F == rt:
                lo = x & 0xFFFF
                addr = ((hi << 16) + (lo - 0x10000 if op == 0x09 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                pc = base + i * 4
                if addr == 0x37002 or (hi == 0x1C02 and op == 0x0D):
                    hits['rw'].append(pc)
                elif TEXT_HI <= addr < base + len(blob):
                    s = cstring(blob, addr - base)
                    if s:
                        for name, rx in CLASSES:
                            if rx.search(s):
                                hits[name].append(pc)
                                break
                break
    if len(sys.argv) > 2:
        for m in re.finditer(r'"(_?sce\w+|\w+)@0x([0-9A-Fa-f]+)"', Path(sys.argv[2]).read_text()):
            if m.group(1).lstrip('_').startswith('sce'):
                hits['sce'].append(int(m.group(2), 16))
    nb = (TEXT_HI - base) // BUCKET + 1
    print(f'buckets of {BUCKET // 1024} KB from 0x{base:X}; one column per bucket, digit = log-ish hit count')
    for name in ('sce', 'cri', 'rw', 'game'):
        c = Counter((a - base) // BUCKET for a in hits[name])
        row = ''.join('.' if c[b] == 0 else str(min(9, 1 + c[b] // 4)) for b in range(nb))
        lo, hi = (min(hits[name]), max(hits[name])) if hits[name] else (0, 0)
        print(f'{name:5} {len(hits[name]):5} hits 0x{lo:X}..0x{hi:X}  {row}')
    print('addr  ' + ' ' * 31 + ''.join(f'{(base + b * BUCKET) >> 16:X}'[-1] for b in range(nb)))


if __name__ == '__main__':
    main()
