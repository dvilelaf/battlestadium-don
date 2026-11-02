#!/usr/bin/env python3
"""List the direct callers (jal instructions) of a function.

Usage: callers.py --elf <elf> --target 0xADDR [--text-start 0x100000] [--text-end 0x3902D4]
                  [--layout config/layout.toml] [--csv config/functions.ghidra.csv]

Prints one line per jal whose target is --target:
  <jal address>  <function>
<function> is the innermost non-label row of the CSV (header Name,Start,End,Size, hexadecimal
Start and End, End exclusive) that contains the jal; a label row (entry_*, caseD_*, default*)
that also contains it is appended as "[label ...]". Without --csv the function is "-".
With no caller it prints "0 calls". --layout overrides the range with [text] start and end.
"""
import argparse
import csv
import struct
import sys
import tomllib

PT_LOAD = 1
OP_JAL = 3
LABEL_PREFIXES = ('entry_', 'caseD_', 'default')


def load(path):
    """Return (load address, bytes) of the first PT_LOAD with file data of an ELF32 little endian file."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        raise SystemExit(f'{path}: not an ELF32 little endian file')
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if p_type == PT_LOAD and p_filesz:
            return p_vaddr, d[p_offset:p_offset + p_filesz]
    raise SystemExit(f'{path}: no PT_LOAD with file data')


def read_functions(path):
    """Return [(name, start, end)] from a CSV file with the header Name,Start,End,Size."""
    with open(path, newline='') as f:
        return [(row['Name'], int(row['Start'], 16), int(row['End'], 16)) for row in csv.DictReader(f)]


def is_label(name):
    return name.startswith(LABEL_PREFIXES)


def containing(functions, addr):
    """Return (function, label) names containing addr: the innermost non-label row and the innermost label row (or None)."""
    func = label = None
    fsize = lsize = None
    for name, start, end in functions:
        if not start <= addr < end:
            continue
        size = end - start
        if is_label(name):
            if lsize is None or size < lsize:
                label, lsize = name, size
        elif fsize is None or size < fsize:
            func, fsize = name, size
    return func, label


def find_callers(blob, base, text_start, text_end, target):
    """Return the addresses of the jal instructions in [text_start, text_end) whose target is target."""
    first = max(0, (text_start - base + 3) // 4)
    last = min(len(blob) // 4, (text_end - base) // 4)
    if last <= first:
        return []
    words = struct.unpack_from(f'<{last - first}I', blob, first * 4)
    found = []
    for k, w in enumerate(words):
        if w >> 26 != OP_JAL:
            continue
        pc = base + (first + k) * 4
        if (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2) == target:
            found.append(pc)
    return found


def main(argv):
    ap = argparse.ArgumentParser(description='List the jal instructions that call a function.')
    ap.add_argument('--elf', required=True, help='ELF executable (read, not run)')
    ap.add_argument('--target', required=True, type=lambda s: int(s, 0), help='function address, e.g. 0x001011D8')
    ap.add_argument('--text-start', type=lambda s: int(s, 0), default=0x100000, help='start of the code')
    ap.add_argument('--text-end', type=lambda s: int(s, 0), default=0x3902D4, help='end of the code')
    ap.add_argument('--layout', help='layout.toml; its [text] start and end override --text-start and --text-end')
    ap.add_argument('--csv', help='function table with the header Name,Start,End,Size')
    args = ap.parse_args(argv)
    start, end = args.text_start, args.text_end
    if args.layout:
        with open(args.layout, 'rb') as f:
            text = tomllib.load(f)['text']
        start, end = int(text['start']), int(text['end'])
    base, blob = load(args.elf)
    functions = read_functions(args.csv) if args.csv else []
    callers = find_callers(blob, base, start, end, args.target)
    if not callers:
        print('0 calls')
        return
    for pc in callers:
        func, label = containing(functions, pc) if functions else (None, None)
        line = f'0x{pc:08X}  {func or "-"}'
        if label:
            line += f'  [label {label}]'
        print(line)


if __name__ == '__main__':
    main(sys.argv[1:])
