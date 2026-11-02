#!/usr/bin/env python3
"""Pin the code layout of the PS2 executable: main text end and islands of code inside the data.

Usage:
  layout.py --csv config/functions.ghidra.csv --elf disc/SLPS_256.75 \
            --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml

The main text is the unbroken run of CSV rows that starts at 0x100000 (two consecutive rows are
in the same run when the gap between them is smaller than TEXT_GAP). Every row at or beyond its
end belongs to an island (rows less than MERGE_GAP apart are merged). TEXT_GAP is larger than
MERGE_GAP because real code inside the text has holes that Ghidra did not turn into functions
(0x488 bytes at 0x2B8DB8 and 0x15A0 bytes at 0x3362F0, both valid R5900 code). Each island is classified
as kernel_patch_blob, code or false_positive. Needs rabbitizer (R5900 decoder).
"""
import argparse
import csv
import hashlib
import struct
import sys
from pathlib import Path

MERGE_GAP = 0x400  # rows closer than this are the same island
TEXT_GAP = 0x10000  # rows closer than this are the same run of main text
MAX_ROW_SPAN = 0x10000  # a row longer than this inside an island is a stray label, not a body
TEXT_START = 0x00100000
LABEL_PREFIXES = ('entry_', 'caseD_', 'default')
LOOKAHEAD = 4  # instructions after a lui in which its addiu/ori must appear
OP_LUI, OP_ADDIU, OP_ORI = 0x0F, 0x09, 0x0D
KERNEL_HI = range(0x8000, 0x8008)  # lui high halves that address kernel memory
PT_LOAD = 1


def load(path):
    """Return (entry, load address, file bytes, memory size, sha256) of the first PT_LOAD with data."""
    d = Path(path).read_bytes()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        raise SystemExit(f'{path}: not an ELF32 little endian file')
    entry, = struct.unpack_from('<I', d, 0x18)
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
    for i in range(phnum):
        t, off, va, _, fsz, msz, _, _ = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if t == PT_LOAD and fsz:
            return entry, va, d[off:off + fsz], msz, hashlib.sha256(d).hexdigest()
    raise SystemExit(f'{path}: no PT_LOAD with file data')


def is_label(name):
    return name.startswith(LABEL_PREFIXES)


def read_rows(path):
    """Return [(name, start, end)] sorted by start, from a CSV with the header Name,Start,End,Size."""
    with open(path, newline='') as f:
        rows = [(r['Name'], int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(f)]
    rows.sort(key=lambda r: (r[1], r[2]))
    return rows


def read_vu(path):
    """Return [(start, end)] of the VU microprograms from the TSV with columns elf_addr, bytes."""
    out = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            s = int(r['elf_addr'], 16)
            out.append((s, s + int(r['bytes'])))
    return out


def split_text(rows, start=TEXT_START, gap=TEXT_GAP):
    """Return (text_end, island_rows): the end of the unbroken run that starts at start, and the rest."""
    end = None
    i = 0
    for i, (_, s, e) in enumerate(rows):
        if end is None:
            if s < start:
                continue
            if s - start >= MERGE_GAP:
                raise ValueError('no row near the text start')
            end = e
        elif s - end >= gap:
            return end, rows[i:]
        else:
            end = max(end, e)
    if end is None:
        raise ValueError('no row at or after the text start')
    return end, []


def group_islands(rows, gap=MERGE_GAP):
    """Merge rows less than gap bytes apart; return [{'start','end','functions','labels','bodies'}].

    A row longer than MAX_ROW_SPAN counts as a one-word marker (a stray label that Ghidra gave
    a huge span); 'bodies' lists the spans of the function rows, the part that is really code.
    """
    islands = []
    for name, s, e in rows:
        if e - s > MAX_ROW_SPAN:
            e = s + 4
        if islands and s - islands[-1]['end'] < gap:
            isl = islands[-1]
            isl['end'] = max(isl['end'], e)
        else:
            isl = {'start': s, 'end': e, 'functions': 0, 'labels': 0, 'bodies': []}
            islands.append(isl)
        if is_label(name):
            isl['labels'] += 1
        else:
            isl['functions'] += 1
            isl['bodies'].append((s, e))
    return islands


def decode_valid(word, vram):
    import rabbitizer
    if word == 0:
        return True
    return rabbitizer.Instruction(word, vram=vram, category=rabbitizer.InstrCategory.R5900).isValid()


def scan_words(blob, base, start, end):
    """Return (words, invalid vram list, set of lui high halves) for the word-aligned range [start, end)."""
    start &= ~3
    end = (end + 3) & ~3
    lo = max(start, base)
    hi = min(end, base + len(blob))
    n = max(0, (hi - lo) // 4)
    words = struct.unpack_from(f'<{n}I', blob, lo - base) if n else ()
    invalid = [lo + i * 4 for i, w in enumerate(words) if not decode_valid(w, lo + i * 4)]
    if end > hi or start < lo:
        invalid.append(hi)  # part of the island is outside the file data
    highs = {w & 0xFFFF for w in words if w >> 26 == OP_LUI}
    return words, invalid, highs


def overlaps(s, e, ranges):
    return any(s < ve and vs < e for vs, ve in ranges)


def classify(valid, highs, in_vu):
    """kernel_patch_blob, code or false_positive from the evidence of one island."""
    if in_vu or not valid:
        return 'false_positive'
    if any(h in KERNEL_HI for h in highs):
        return 'kernel_patch_blob'
    return 'code'


def describe(isl, valid, bad_all, highs, in_vu):
    parts = []
    if in_vu:
        parts.append('overlaps a VU microprogram')
    if isl['bodies']:
        parts.append('all words of the function rows decode as R5900' if valid
                     else 'some words of the function rows do not decode as R5900')
    else:
        parts.append('all words decode as R5900' if valid else 'words do not decode as R5900')
    if bad_all:
        parts.append(f'{len(bad_all)} word(s) of the whole range do not decode (first at 0x{bad_all[0]:08X})')
    k = sorted(h for h in highs if h in KERNEL_HI)
    if k:
        parts.append('lui high halves ' + ', '.join(f'0x{h:04X}' for h in k))
    parts.append(f"{isl['functions']} function row(s), {isl['labels']} label row(s)")
    return '; '.join(parts)


def analyse(rows, blob, base, vu):
    """Return (text_end, islands). An island with function rows is judged on those rows only."""
    text_end, rest = split_text(rows)
    islands = group_islands(rest)
    for isl in islands:
        _, bad_all, highs = scan_words(blob, base, isl['start'], isl['end'])
        if isl['bodies']:
            bad, highs = [], set()
            for s, e in isl['bodies']:
                _, b, h = scan_words(blob, base, s, e)
                bad += b
                highs |= h
        else:
            bad = bad_all
        in_vu = overlaps(isl['start'], isl['end'], vu)
        valid = not bad
        isl['kind'] = classify(valid, highs, in_vu)
        isl['note'] = describe(isl, valid, bad_all, highs, in_vu)
        del isl['bodies']
    return text_end, islands


def find_address_refs(blob, base, text_end, lo_addr, hi_addr):
    """Return [pc] of lui + addiu/ori pairs in the text that build an address in [lo_addr, hi_addr)."""
    n = len(blob) // 4
    words = struct.unpack_from(f'<{n}I', blob)
    ti = min(n, max(0, (text_end - base) // 4))
    found = []
    for i in range(ti):
        w = words[i]
        if w >> 26 != OP_LUI:
            continue
        rt = (w >> 16) & 0x1F
        hi = w & 0xFFFF
        for j in range(i + 1, min(i + 1 + LOOKAHEAD, n)):
            x = words[j]
            op = x >> 26
            if op not in (OP_ADDIU, OP_ORI) or (x >> 21) & 0x1F != rt or (x >> 16) & 0x1F != rt:
                continue
            lo = x & 0xFFFF
            if op == OP_ADDIU and lo & 0x8000:
                lo -= 0x10000
            addr = ((hi << 16) + lo) & 0xFFFFFFFF
            if lo_addr <= addr < hi_addr:
                found.append((base + i * 4, addr))
            break
    return found


def find_data_refs(blob, base, text_end, lo_addr, hi_addr):
    """Return [(address of the word, value)] for words outside the text and outside the range itself
    whose value is a word-aligned address in range (an odd value is a number, not a pointer)."""
    n = len(blob) // 4
    words = struct.unpack_from(f'<{n}I', blob)
    first = max(0, (text_end - base) // 4)
    return [(base + i * 4, words[i]) for i in range(first, n)
            if lo_addr <= words[i] < hi_addr and words[i] % 4 == 0 and not lo_addr <= base + i * 4 < hi_addr]


def function_at(rows, addr):
    """Name of the smallest non-label row that contains addr, or '-'."""
    best = None
    for name, s, e in rows:
        if s > addr:
            break
        if not is_label(name) and s <= addr < e and (best is None or e - s < best[2] - best[1]):
            best = (name, s, e)
    return best[0] if best else '-'


def render_toml(sha, base, file_end, bss_end, entry, text_end, islands):
    out = ['# Generated by tools/funcmap/layout.py; do not edit by hand.',
           '[elf]',
           f'sha256 = "{sha}"',
           f'load_start = 0x{base:08X}',
           f'file_end   = 0x{file_end:08X}',
           f'bss_end    = 0x{bss_end:08X}',
           f'entry      = 0x{entry:08X}',
           '',
           '[text]',
           f'start = 0x{TEXT_START:08X}',
           f'end   = 0x{text_end:08X}']
    for isl in islands:
        out += ['',
                '[[island]]',
                f"start = 0x{isl['start']:08X}",
                f"end   = 0x{isl['end']:08X}",
                f"kind  = \"{isl['kind']}\"",
                f"functions = {isl['functions']}",
                f"labels = {isl['labels']}",
                f"note = \"{isl['note']}\""]
    return '\n'.join(out) + '\n'


def main(argv):
    ap = argparse.ArgumentParser(description='Pin the code layout of the PS2 executable.')
    ap.add_argument('--csv', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--vu', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args(argv)

    rows = read_rows(args.csv)
    entry, base, blob, msz, sha = load(args.elf)
    vu = read_vu(args.vu)
    text_end, islands = analyse(rows, blob, base, vu)
    first_vu = min(s for s, _ in vu)
    file_end = base + len(blob)

    print(f'text.end = 0x{text_end:08X}  (first VU microprogram at 0x{first_vu:08X})')
    if not 0x390000 <= text_end <= first_vu or text_end % 4:
        print('text end is outside 0x390000..first VU microprogram or not aligned: not writing', file=sys.stderr)
        return 1

    tail_lo, tail_hi = text_end, first_vu
    tail = blob[tail_lo - base:tail_hi - base]
    print(f'bytes 0x{tail_lo:08X}..0x{tail_hi:08X} ({len(tail)}): '
          f'{sum(1 for b in tail if b == 0)} zero bytes, '
          f'distinct non-zero words {sorted({hex(w) for w in struct.unpack_from(f"<{len(tail) // 4}I", tail) if w})}')
    for isl in islands:
        print(f"island 0x{isl['start']:08X}-0x{isl['end']:08X} {isl['kind']:18s} "
              f"functions={isl['functions']} labels={isl['labels']}  {isl['note']}")
    for isl in islands:
        if isl['kind'] != 'kernel_patch_blob':
            continue
        refs = find_address_refs(blob, base, text_end, isl['start'], isl['end'])
        drefs = find_data_refs(blob, base, text_end, isl['start'], isl['end'])
        print(f"references to island 0x{isl['start']:08X}: {len(refs)} lui/addiu, {len(drefs)} data words")
        for pc, addr in refs:
            print(f'  0x{pc:08X} -> 0x{addr:08X} in {function_at(rows, pc)}')
        for pc, v in drefs:
            print(f'  data 0x{pc:08X} = 0x{v:08X}')

    Path(args.out).write_text(render_toml(sha, base, file_end, base + msz, entry, text_end, islands))
    print(f'wrote {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
