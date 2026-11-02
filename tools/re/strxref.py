#!/usr/bin/env python3
"""Find the code that builds the address of a string: lui + addiu/ori pairs that point into data.

Usage: strxref.py --elf <elf> --text-end 0x3902D4 [--csv functions.csv] <regular expression>

Prints one line per reference:
  <lui address>  <string address>  <function>  "<string>"
<function> is the name from the CSV (header Name,Start,End,Size, hexadecimal Start and End)
when the lui address falls in a function, otherwise "-".

Generalises the scan of tools/layout_survey.py. Needs no rabbitizer: the ELF segment is read here.
"""
import argparse
import csv
import re
import struct
import sys

PT_LOAD = 1
MIN_LEN = 5  # shortest string that is reported
MAX_LEN = 200  # longest string that is reported
LOOKAHEAD = 4  # instructions after the lui in which its addiu/ori must appear
OP_LUI, OP_ADDIU, OP_ORI = 0x0F, 0x09, 0x0D


def load(path):
    """Return (entry, load address, bytes) of the first PT_LOAD with file data of an ELF32 little endian file."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        raise SystemExit(f'{path}: not an ELF32 little endian file')
    entry, = struct.unpack_from('<I', d, 0x18)
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if p_type == PT_LOAD and p_filesz:
            return entry, p_vaddr, d[p_offset:p_offset + p_filesz]
    raise SystemExit(f'{path}: no PT_LOAD with file data')


def read_functions(path):
    """Return [(name, start, end)] from a CSV file with the header Name,Start,End,Size."""
    with open(path, newline='') as f:
        return [(row['Name'], int(row['Start'], 16), int(row['End'], 16)) for row in csv.DictReader(f)]


def cstring(blob, off):
    """Return the zero-terminated printable ASCII string at off if its length is MIN_LEN..MAX_LEN, else None."""
    if off < 0:
        return None
    end = blob.find(b'\0', off)
    if end < 0 or not MIN_LEN <= end - off <= MAX_LEN:
        return None
    s = blob[off:end]
    if all(32 <= b < 127 for b in s):
        return s.decode('ascii')
    return None


def function_at(functions, addr):
    for name, start, end in functions:
        if start <= addr < end:
            return name
    return '-'


def find_references(blob, base, text_end, rx, functions=()):
    """Return [(lui address, string address, function, string)] for each reference to a string matching rx.

    Code is the part of blob below text_end; the referenced string must start at or above text_end.
    """
    n = len(blob) // 4
    words = struct.unpack_from(f'<{n}I', blob)
    ti = min(n, max(0, (text_end - base) // 4))
    end = base + len(blob)
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
            if op == OP_ADDIU and lo & 0x8000:  # addiu sign-extends its immediate, ori does not
                lo -= 0x10000
            addr = ((hi << 16) + lo) & 0xFFFFFFFF
            if text_end <= addr < end:
                s = cstring(blob, addr - base)
                if s and rx.search(s):
                    pc = base + i * 4
                    found.append((pc, addr, function_at(functions, pc), s))
            break  # only the first addiu/ori on the same register completes the pair
    return found


def main(argv):
    ap = argparse.ArgumentParser(description='Find the code that builds the address of a matching string.')
    ap.add_argument('--elf', required=True, help='ELF executable (read, not run)')
    ap.add_argument('--text-end', required=True, type=lambda s: int(s, 0), help='end of the code, e.g. 0x3902D4')
    ap.add_argument('--csv', help='function table with the header Name,Start,End,Size')
    ap.add_argument('regex', help='regular expression that the string must match')
    args = ap.parse_args(argv)
    _, base, blob = load(args.elf)
    functions = read_functions(args.csv) if args.csv else []
    rx = re.compile(args.regex)
    for pc, addr, func, s in find_references(blob, base, args.text_end, rx, functions):
        print(f'0x{pc:08X}  0x{addr:08X}  {func}  "{s}"')


if __name__ == '__main__':
    main(sys.argv[1:])
