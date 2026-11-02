#!/usr/bin/env python3
"""Survey a stripped PS2 ELF: code/data boundary, instruction classes, VIF/VU microcode blobs.

Usage: elf_survey.py <elf>
Needs rabbitizer (R5900 decoder) only to judge whether a word is a valid instruction.
"""
import struct
import sys
from collections import Counter

import rabbitizer

PAGE = 0x1000


def load(path):
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
    entry, = struct.unpack_from('<I', d, 0x18)
    for i in range(phnum):
        t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if t == 1 and fsz:
            return entry, va, d[off:off + fsz], msz
    raise SystemExit('no PT_LOAD with file data')


def valid(word, vram):
    if word == 0:
        return True
    ins = rabbitizer.Instruction(word, vram=vram, category=rabbitizer.InstrCategory.R5900)
    return ins.isValid()


def main():
    entry, base, blob, msz = load(sys.argv[1])
    n = len(blob) // 4
    words = struct.unpack_from(f'<{n}I', blob)
    end = base + len(blob)
    print(f'entry=0x{entry:X} load=0x{base:X}..0x{end:X} file={len(blob)} mem=0x{msz:X} bss_end=0x{base + msz:X}')

    # --- per-page validity + "looks like code" (jr ra density) to find the text/data boundary
    pages = []
    for p in range(0, n, PAGE // 4):
        chunk = words[p:p + PAGE // 4]
        ok = sum(valid(w, base + (p + i) * 4) for i, w in enumerate(chunk))
        jr = sum(1 for w in chunk if w == 0x03E00008)
        zero = sum(1 for w in chunk if w == 0)
        pages.append((base + p * 4, ok / len(chunk), jr, zero / len(chunk)))
    code_pages = [a for a, ok, jr, z in pages if ok > 0.97 and jr >= 1 and z < 0.6]
    # longest contiguous run of code pages
    runs, start, prev = [], None, None
    for a in code_pages:
        if start is None:
            start = prev = a
        elif a == prev + PAGE:
            prev = a
        else:
            runs.append((start, prev + PAGE))
            start = prev = a
    if start is not None:
        runs.append((start, prev + PAGE))
    runs.sort(key=lambda r: r[0])
    print('code-like page runs (>=4 pages):')
    for s, e in runs:
        if e - s >= 4 * PAGE:
            print(f'  0x{s:X}..0x{e:X}  {(e - s) / 1024:.0f} KB')
    text_lo = runs[0][0] if runs else base
    big = max(runs, key=lambda r: r[1] - r[0])
    # treat everything from load base to the end of the last big run as text
    text_hi = max(e for s, e in runs if e - s >= 16 * PAGE)
    print(f'assumed text: 0x{base:X}..0x{text_hi:X} ({(text_hi - base) / 1024:.0f} KB); data: 0x{text_hi:X}..0x{end:X} ({(end - text_hi) / 1024:.0f} KB)')

    ti = (text_hi - base) // 4
    # --- instruction classes inside text
    c = Counter()
    jal_targets = Counter()
    hw_lui = Counter()
    for i in range(ti):
        w = words[i]
        op = w >> 26
        if op == 0x03:
            jal_targets[(w & 0x03FFFFFF) << 2] += 1
            c['jal'] += 1
        elif op == 0x12:
            c['cop2'] += 1
            if w & (1 << 25):
                f = w & 0x3F
                if f == 0x38:
                    c['vcallms'] += 1
                elif f == 0x39:
                    c['vcallmsr'] += 1
            else:
                rs = (w >> 21) & 0x1F
                c[{1: 'qmfc2', 2: 'cfc2', 5: 'qmtc2', 6: 'ctc2', 8: 'bc2'}.get(rs, 'cop2_other')] += 1
        elif op == 0x36:
            c['lqc2'] += 1
        elif op == 0x3E:
            c['sqc2'] += 1
        elif op == 0x1C:
            c['mmi'] += 1
        elif op == 0x1E:
            c['lq'] += 1
        elif op == 0x1F:
            c['sq'] += 1
        elif op == 0x11:
            c['cop1'] += 1
            if (w >> 21) & 0x1F == 0x10:
                f = w & 0x3F
                if f == 0x04:
                    c['sqrt.s'] += 1
                elif f == 0x16:
                    c['rsqrt.s'] += 1
        elif op == 0x10:
            c['cop0'] += 1
        elif op == 0 and (w & 0x3F) == 0x0C:
            c['syscall'] += 1
        elif op == 0x0F:
            hi = w & 0xFFFF
            if hi in (0x1000, 0x1001, 0x1100, 0x1200, 0x7000, 0xB000, 0xB200, 0x1F80):
                hw_lui[hi] += 1
        if w == 0x03E00008:
            c['jr_ra'] += 1
    print('text instruction classes:')
    for k in sorted(c):
        print(f'  {k:12} {c[k]}')
    print('lui of hardware pages:', {f'0x{k:04X}': v for k, v in sorted(hw_lui.items())})
    inside = [t for t in jal_targets if base <= t < text_hi]
    outside = [t for t in jal_targets if not base <= t < text_hi]
    print(f'distinct jal targets: {len(jal_targets)} (inside text {len(inside)}, outside {len(outside)})')

    # --- function-pointer candidates in data pointing into text
    ptrs = Counter()
    for i in range(ti, n):
        w = words[i]
        if base <= w < text_hi and w & 3 == 0:
            ptrs[w] += 1
    only_ptr = [p for p in ptrs if p not in jal_targets]
    print(f'data words pointing into text: {sum(ptrs.values())} ({len(ptrs)} distinct, {len(only_ptr)} never a jal target)')

    # --- VIF MPG (0x4A) uploads embedded in data: header word 0x4Annaaaa followed by nn 64-bit VU instrs
    print('VIF MPG candidates in data:')
    total = 0
    i = ti
    found = []
    while i < n:
        w = words[i]
        if (w >> 24) & 0x7F == 0x4A:
            num = (w >> 16) & 0xFF or 256
            addr = w & 0xFFFF
            if i + 1 + num * 2 <= n and addr < 0x800:
                body = words[i + 1:i + 1 + num * 2]
                # VU upper words: bits 31..27 flags (I,E,M,D,T) mostly zero and many NOPs (0x000002FF)
                nops = sum(1 for k in range(1, len(body), 2) if body[k] & 0x7FF == 0x2FF)
                lower_nops = sum(1 for k in range(0, len(body), 2) if body[k] == 0x8000033C)
                if num >= 8 and (nops + lower_nops) >= num // 6:
                    found.append((base + i * 4, num, addr))
                    total += num
                    i += 1 + num * 2
                    continue
        i += 1
    # merge consecutive MPG chunks into programs
    progs = []
    for a, num, addr in found:
        if progs and progs[-1][0] + progs[-1][3] + 4 * 0 <= a <= progs[-1][0] + progs[-1][3] + 16 and addr == progs[-1][1] + progs[-1][2]:
            p = progs[-1]
            progs[-1] = (p[0], p[1], p[2] + num, a + 4 + num * 8 - p[0])
        else:
            progs.append((a, addr, num, 4 + num * 8))
    for a, addr, num, size in progs:
        print(f'  0x{a:X}: vu_addr=0x{addr:X} instrs={num} bytes={size}')
    print(f'  {len(found)} MPG chunks, {len(progs)} contiguous programs, {total} VU instructions total')


if __name__ == '__main__':
    main()
