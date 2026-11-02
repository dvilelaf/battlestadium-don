#!/usr/bin/env python3
"""Validate the function map (config/functions.ghidra.csv) against the layout and the executable.

Usage:
  validate.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 \
              [--extra config/extra_entry_points.txt] [--gaps-out work/reports/text_gaps.tsv]

Checks (errors are accumulated, nothing is modified):
  E1  no two functions overlap partially (nested functions are allowed and only counted)
  E2  Start and End are multiples of 4, End > Start and End - Start == Size
  E3  every row lies, over its whole range, inside the main text or inside one island
  E4  every jal inside a row of the main text targets the Start of some row or an --extra address
  E5  the entry point 0x100008 is the Start of a row or lies inside a function
Then prints a summary with the coverage of the main text and the uncovered gaps. Exit code 0 if
there are no errors, 1 otherwise. A row is a label when its name starts with entry_, caseD_ or
default; every other row (including the one named exactly "entry") is a function.
Needs rabbitizer (R5900 decoder) for the gap report.
"""
import argparse
import bisect
import csv
import struct
import sys
import tomllib
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import layout as _layout  # noqa: E402

ENTRY_POINT = 0x00100008
OP_JAL = 3
SHOWN = 10  # errors of each type printed by default
GAPS_SHOWN = 20

Row = namedtuple('Row', 'name start end size')
Error = namedtuple('Error', 'code message')
Gap = namedtuple('Gap', 'start end nonzero all_valid')


def is_label(name):
    return _layout.is_label(name)


def fmt_row(r):
    return f'{r.name} 0x{r.start:08X}-0x{r.end:08X} (size {r.size})'


def read_csv(path):
    """Return the rows of a CSV with the header Name,Start,End,Size, sorted by (start, end)."""
    with open(path, newline='') as f:
        rows = [Row(r['Name'], int(r['Start'], 16), int(r['End'], 16), int(r['Size']))
                for r in csv.DictReader(f)]
    rows.sort(key=lambda r: (r.start, r.end, r.name))
    return rows


def read_layout(path):
    """Return (text_start, text_end, [(start, end, kind)]) from config/layout.toml."""
    d = tomllib.loads(Path(path).read_text())
    islands = [(i['start'], i['end'], i.get('kind', '?')) for i in d.get('island', [])]
    return d['text']['start'], d['text']['end'], islands


def read_extra(path):
    """Return the set of addresses of a text file with one per line (`0xADDR` or `name@0xADDR`, # comments)."""
    out = set()
    for line in Path(path).read_text().splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            out.add(int(line.rsplit('@', 1)[-1], 16))
    return out


def split_rows(rows):
    return [r for r in rows if not is_label(r.name)], [r for r in rows if is_label(r.name)]


def check_e1(functions):
    """Return (errors, nested): partially overlapping function pairs, and how many functions are nested."""
    fs = sorted(functions, key=lambda r: (r.start, -r.end))
    starts = [r.start for r in fs]
    errors = []
    nested = set()
    for i, a in enumerate(fs):
        for j in range(i + 1, bisect.bisect_left(starts, a.end)):
            b = fs[j]
            if b.end <= a.end:
                nested.add(j)
            else:
                errors.append(Error('E1', f'partial overlap:\n    {fmt_row(a)}\n    {fmt_row(b)}'))
    return errors, len(nested)


def check_e2(rows):
    errors = []
    for r in rows:
        problems = []
        if r.start % 4 or r.end % 4:
            problems.append('Start or End not a multiple of 4')
        if r.end <= r.start:
            problems.append('End <= Start')
        if r.end - r.start != r.size:
            problems.append(f'End - Start = 0x{r.end - r.start:X} != Size')
        if problems:
            errors.append(Error('E2', f'{fmt_row(r)}: ' + '; '.join(problems)))
    return errors


def check_e3(rows, text_start, text_end, islands):
    errors = []
    for r in rows:
        if text_start <= r.start and r.end <= text_end:
            continue
        if any(s <= r.start and r.end <= e for s, e, _ in islands):
            continue
        errors.append(Error('E3', f'{fmt_row(r)} is not inside the main text or one island'))
    return errors


def check_e5(rows, entry=ENTRY_POINT):
    for r in rows:
        if r.start == entry or (not is_label(r.name) and r.start <= entry < r.end):
            return []
    return [Error('E5', f'entry point 0x{entry:08X} is not the Start of a row nor inside a function')]


def coverage(rows, text_start, text_end):
    """Per-word flags of the main text: 1 if at least one row covers the word."""
    n = (text_end - text_start) // 4
    cov = bytearray(n)
    for r in rows:
        lo = max(r.start, text_start)
        hi = min(r.end, text_end)
        if lo < hi:
            a = (lo - text_start) // 4
            b = (hi - text_start + 3) // 4
            cov[a:b] = b'\x01' * (b - a)
    return cov


def text_words(blob, base, text_start, text_end):
    """Words of the main text; the part that lies outside the file data reads as zero."""
    n = (text_end - text_start) // 4
    lo = (text_start - base)
    chunk = blob[lo:lo + n * 4] if lo >= 0 else b''
    chunk += b'\0' * (n * 4 - len(chunk))
    return struct.unpack(f'<{n}I', chunk)


def smallest_containing(rows, addr, functions_only=False):
    best = None
    for r in rows[:bisect.bisect_right([x.start for x in rows], addr)]:
        if r.start <= addr < r.end and not (functions_only and is_label(r.name)):
            if best is None or r.end - r.start < best.end - best.start:
                best = r
    return best


def check_e4(rows, words, cov, text_start, extra=()):
    """jal inside rows of the main text whose target is neither the Start of a row nor in extra."""
    known = {r.start for r in rows} | set(extra)
    first_jal = {}
    for i, w in enumerate(words):
        if cov[i] and w >> 26 == OP_JAL:
            t = (w & 0x03FFFFFF) << 2
            if t not in known and t not in first_jal:
                first_jal[t] = text_start + i * 4
    errors = []
    for t, pc in sorted(first_jal.items()):
        caller = smallest_containing(rows, pc, functions_only=True)
        inside = smallest_containing(rows, t)
        errors.append(Error(
            'E4', f'jal target 0x{t:08X}; called from jal at 0x{pc:08X} in '
                  f'{caller.name if caller else "no function row"}; '
                  + (f'target falls inside {fmt_row(inside)}' if inside else 'target is in no row')))
    return errors


def find_gaps(cov, words, text_start):
    """Return (zero_gaps, nonzero_gaps) of the uncovered runs of the main text.

    zero_gaps is the number of runs made only of zero words (padding); nonzero_gaps is a list of
    Gap with all_valid filled in (every word decodes as an R5900 instruction).
    """
    zero = 0
    other = []
    n = len(cov)
    i = 0
    while i < n:
        if cov[i]:
            i += 1
            continue
        j = i
        while j < n and not cov[j]:
            j += 1
        run = words[i:j]
        nz = sum(1 for w in run if w)
        if nz == 0:
            zero += 1
        else:
            vram = text_start + i * 4
            valid = all(_layout.decode_valid(w, vram + k * 4) for k, w in enumerate(run))
            other.append(Gap(vram, text_start + j * 4, nz, valid))
        i = j
    return zero, other


def write_gaps(path, gaps):
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with open(p, 'w', newline='') as f:
        f.write('start\tend\tbytes\tnonzero_words\tall_valid\n')
        for g in gaps:
            f.write(f'0x{g.start:08X}\t0x{g.end:08X}\t{g.end - g.start}\t{g.nonzero}\t{int(g.all_valid)}\n')


def count_labels_outside(functions, labels):
    fs = sorted(functions, key=lambda r: r.start)
    starts = [r.start for r in fs]
    maxend = []
    m = 0
    for r in fs:
        m = max(m, r.end)
        maxend.append(m)
    out = 0
    for lb in labels:
        k = bisect.bisect_right(starts, lb.start)
        if k == 0 or maxend[k - 1] <= lb.start:
            out += 1
    return out


def validate(rows, text_start, text_end, islands, blob, base, extra=()):
    """Run every check; return a dict with the errors, counts and gaps."""
    functions, labels = split_rows(rows)
    e1, nested = check_e1(functions)
    cov = coverage(rows, text_start, text_end)
    words = text_words(blob, base, text_start, text_end)
    zero_gaps, gaps = find_gaps(cov, words, text_start)
    errors = e1 + check_e2(rows) + check_e3(rows, text_start, text_end, islands) \
        + check_e4(rows, words, cov, text_start, extra) + check_e5(rows)
    return {
        'errors': errors, 'functions': len(functions), 'labels': len(labels), 'nested': nested,
        'labels_outside': count_labels_outside(functions, labels),
        'covered_words': sum(cov), 'text_words': len(cov), 'zero_gaps': zero_gaps, 'gaps': gaps,
    }


def print_summary(res, text_start, text_end, shown=SHOWN):
    print(f"rows: {res['functions']} functions, {res['labels']} labels")
    print(f"nested functions (inside another function, allowed): {res['nested']}")
    print(f"labels outside every function: {res['labels_outside']}")
    cw, tw = res['covered_words'], res['text_words']
    print(f'main text 0x{text_start:08X}-0x{text_end:08X}: {cw * 4} of {tw * 4} bytes covered by at least one row '
          f'({100 * cw / tw if tw else 0:.2f}%)')
    gaps = res['gaps']
    print(f"uncovered gaps: {res['zero_gaps'] + len(gaps)} "
          f"({res['zero_gaps']} only zero words = padding, {len(gaps)} with non-zero words)")
    nzw = sum(g.nonzero for g in gaps)
    valid = sum(1 for g in gaps if g.all_valid)
    print(f'gaps with non-zero words: {len(gaps)}, {nzw} non-zero words, {sum(g.end - g.start for g in gaps)} bytes, '
          f'{valid} decode completely as valid R5900 instructions, {len(gaps) - valid} do not')
    for g in sorted(gaps, key=lambda g: (-(g.end - g.start), g.start))[:GAPS_SHOWN]:
        print(f"  0x{g.start:08X}-0x{g.end:08X} {g.end - g.start:6d} bytes  "
              f"{'all words valid' if g.all_valid else 'has invalid words'}")
    by = {}
    for e in res['errors']:
        by.setdefault(e.code, []).append(e)
    print('errors: ' + ', '.join(f'{c}={len(by.get(c, []))}' for c in ('E1', 'E2', 'E3', 'E4', 'E5')))
    for c in ('E1', 'E2', 'E3', 'E4', 'E5'):
        for e in by.get(c, [])[:shown]:
            print(f'  {e.code} {e.message}')
        if len(by.get(c, [])) > shown:
            print(f'  ... {len(by[c]) - shown} more {c} (use --max-errors)')


def main(argv):
    ap = argparse.ArgumentParser(description='Validate the function map against the layout and the executable.')
    ap.add_argument('--csv', required=True)
    ap.add_argument('--layout', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--extra', help='file with extra entry point addresses (one per line)')
    ap.add_argument('--gaps-out', help='write the gaps with non-zero words as a TSV')
    ap.add_argument('--max-errors', type=int, default=SHOWN, help='errors printed per type (default 10)')
    args = ap.parse_args(argv)

    rows = read_csv(args.csv)
    text_start, text_end, islands = read_layout(args.layout)
    _, base, blob, _, _ = _layout.load(args.elf)
    extra = read_extra(args.extra) if args.extra else set()
    res = validate(rows, text_start, text_end, islands, blob, base, extra)
    print_summary(res, text_start, text_end, args.max_errors)
    if args.gaps_out:
        write_gaps(args.gaps_out, res['gaps'])
        print(f"wrote {args.gaps_out} ({len(res['gaps'])} gaps)")
    return 1 if res['errors'] else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
