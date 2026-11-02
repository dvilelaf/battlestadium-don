#!/usr/bin/env python3
"""Check that the generated code computes each sqrt.s of the ELF from the register it names.

Usage:
  check_sqrt.py --elf disc/SLPS_256.75 --layout config/layout.toml --generated generated
                [--map work/functions.filtered.csv]

Every sqrt.s word in the main text (fmt S, funct 4) must appear in generated/*.cpp as a
`ctx->pc = 0x<addr>u;` statement followed by `ctx->f[fd] = FPU_SQRT_S(... ctx->f[ft] ...)`.
Without PS2Recomp patch 0002 the source was fs (always 0), so the instructions with ft != fs
are the ones that prove the fix. An address that is missing from the generated code is skipped
when it lies inside a function replaced by a runtime handler (declared in
ps2_recompiled_stubs.h); otherwise it is an error. Exit 0 without errors, 1 with errors.
"""
import argparse
import csv
import re
import struct
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools' / 'funcmap'))
import layout  # noqa: E402

OP_COP1 = 0x11
FMT_S = 0x10
FUNCT_SQRT = 0x04
STUBS_HEADER = 'ps2_recompiled_stubs.h'
STUB_RE = re.compile(r'void\s+\w+_0x([0-9a-fA-F]+)\s*\(')
PC_RE = re.compile(r'ctx->pc = 0x([0-9a-fA-F]+)u;')
LABEL_RE = re.compile(r'[A-Za-z_]\w*:$')
GOTO_RE = re.compile(r'goto\b')
DEST_RE = re.compile(r'ctx->f\[(\d+)\]\s*=')
ARG_RE = re.compile(r'FPU_SQRT_S\s*\(.*?ctx->f\[(\d+)\]')
STATEMENT_MAX_LINES = 4  # a statement may be wrapped over a few lines


def find_sqrt(blob, base, start, end):
    """Return [(address, fd, ft, fs)] of the sqrt.s words in the file data of the range [start, end)."""
    lo = max(start & ~3, base)
    hi = min((end + 3) & ~3, base + len(blob))
    n = max(0, (hi - lo) // 4)
    words = struct.unpack_from(f'<{n}I', blob, lo - base) if n else ()
    found = []
    for i, w in enumerate(words):
        if (w >> 26) == OP_COP1 and ((w >> 21) & 31) == FMT_S and (w & 0x3F) == FUNCT_SQRT:
            found.append((lo + 4 * i, (w >> 6) & 31, (w >> 16) & 31, (w >> 11) & 31))
    return found


def parse_statement(lines, i):
    """Return (destination, source) of the statement that starts at lines[i], or (None, None)."""
    parts = []
    for line in lines[i:i + STATEMENT_MAX_LINES]:
        s = line.strip()
        if not s or s.startswith('//'):
            continue
        parts.append(s)
        if ';' in s:
            break
    stmt = ' '.join(parts)
    d = DEST_RE.search(stmt)
    a = ARG_RE.search(stmt)
    if 'FPU_SQRT_S' not in stmt or not d or not a:
        return None, None
    return int(d.group(1)), int(a.group(1))


def collect_sqrt(folder):
    """Return {address: [(file name, destination register, source register)]} of every pc statement.

    The generated code can repeat a `ctx->pc = ...;` line (before the comment and again before the
    statement), so the pc lines since the last statement all take the next real statement. A label
    keeps them pending; a `goto` drops them, because a jump site is not an executed instruction (the
    instruction is recorded at the label).
    """
    out = {}
    for path in sorted(Path(folder).glob('*.cpp')):
        data = path.read_bytes()
        if b'FPU_SQRT_S' not in data:  # cheap test first: most files have no sqrt
            continue
        lines = data.decode('utf-8', errors='replace').splitlines()
        pending = []
        for i, line in enumerate(lines):
            s = line.strip()
            m = PC_RE.search(s)
            if m:
                addr = int(m.group(1), 16)
                if addr not in pending:
                    pending.append(addr)
                continue
            if not s or s.startswith('//') or LABEL_RE.match(s) or not pending:
                continue
            if GOTO_RE.match(s):
                pending = []
                continue
            dest, src = parse_statement(lines, i)
            for addr in pending:
                out.setdefault(addr, []).append((path.name, dest, src))
            pending = []
    return out


def replaced_ranges(generated, map_csv):
    """Return [(start, end)] of the functions replaced by runtime handlers and listed in the map."""
    header = Path(generated) / STUBS_HEADER
    if not header.is_file():
        raise SystemExit(f'{header}: not found')
    starts = {int(m.group(1), 16) for m in STUB_RE.finditer(header.read_text(encoding='utf-8', errors='replace'))}
    ranges = []
    with open(map_csv, newline='') as f:
        for r in csv.DictReader(f):
            s = int(r['Start'], 16)
            if s in starts:
                ranges.append((s, int(r['End'], 16)))
    return ranges


def in_ranges(addr, ranges):
    return any(s <= addr < e for s, e in ranges)


def reg(n):
    return 'none' if n is None else f'f{n}'


def compare(sqrt_words, generated_map, ranges):
    """Return (error lines, checked, errors, skipped, instructions with ft != fs)."""
    lines = []
    checked = errors = skipped = ft_ne_fs = 0
    for addr, fd, ft, fs in sqrt_words:
        occurrences = generated_map.get(addr, [])
        if not occurrences and in_ranges(addr, ranges):
            skipped += 1
            continue
        checked += 1
        if ft != fs:
            ft_ne_fs += 1
        if not occurrences:
            lines.append(f'error 0x{addr:08X}: expected fd={fd} ft={ft}, found missing')
            errors += 1
        for name, dest, src in occurrences:
            if dest != fd or src != ft:
                lines.append(f'error 0x{addr:08X}: expected fd={fd} ft={ft}, '
                             f'found {name}: dest={reg(dest)} src={reg(src)}')
                errors += 1
    return lines, checked, errors, skipped, ft_ne_fs


def main(argv):
    ap = argparse.ArgumentParser(description='Check the sqrt.s instructions of the generated code.')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--layout', required=True)
    ap.add_argument('--generated', required=True)
    ap.add_argument('--map', default=str(REPO / 'work' / 'functions.filtered.csv'))
    args = ap.parse_args(argv)

    with open(args.layout, 'rb') as f:
        text = tomllib.load(f)['text']
    _, base, blob, _, _ = layout.load(args.elf)
    words = find_sqrt(blob, base, text['start'], text['end'])
    generated_map = collect_sqrt(args.generated)
    ranges = replaced_ranges(args.generated, args.map)
    lines, checked, errors, skipped, ft_ne_fs = compare(words, generated_map, ranges)

    for line in lines:
        print(line)
    summary = f'sqrt.s: {checked} checked, {errors} errors'
    if skipped:
        summary += f', {skipped} skipped'
    print(summary)
    print(f'of which {ft_ne_fs} have ft != fs')
    return 0 if errors == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
