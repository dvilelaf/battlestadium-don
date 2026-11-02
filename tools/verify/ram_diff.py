#!/usr/bin/env python3
"""Compare two guest memory images word by word (T-016 iteration 8).

Usage:
  ram_diff.py <ours.bin> <reference.bin> [--range 0x391000:0x5A6800] [--kind code|all]
              [--csv config/functions.ghidra.csv] [--limit N]

Both images are guest RAM at guest address 0 (the port writes one with DON_DUMP_RAM, the reference
ones are ee_ram.bin of work/ref/x/NN). Words are 32-bit little endian. `--kind code` keeps only the
words where at least one side looks like an address inside the game's code (TEXT_START..TEXT_END),
which is what matters when comparing a run with a reference from a different moment: a slot that
holds a code address on one side and something else on the other is a function pointer that was not
installed, or installed differently. Every printed line gives the name of the function-map row each
side falls in, when it falls in one. The summary groups the differing words into runs of consecutive
addresses, so a table with several wrong slots shows up as one group.

The reference image is from a later moment of the game, so many differences are legitimate; the
interesting ones are pointers into the code.

Standard library only; part of the repository tooling (`uv run --no-project python -I`).
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

TEXT_START = 0x100000
TEXT_END = 0x3902D4
FUNCTIONS_CSV = Path(__file__).resolve().parent.parent.parent / "config" / "functions.ghidra.csv"


def parse_address_range(text):
    start, _, end = text.partition(":")
    return int(start, 0), int(end, 0) if end else None


def load_functions(path):
    rows = []
    try:
        with Path(path).open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                try:
                    rows.append((row["Name"], int(row["Start"], 16), int(row["End"], 16)))
                except (KeyError, ValueError):
                    continue
    except OSError:
        pass
    return rows


def function_name(rows, address):
    for name, start, end in rows:
        if start <= address < end:
            return name
    return None


def in_code(value):
    return TEXT_START <= value < TEXT_END


def words(data, start, end):
    for address in range(start, end, 4):
        yield address, struct.unpack_from("<I", data, address)[0]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ours")
    parser.add_argument("reference")
    parser.add_argument("--range", dest="address_range", default="0x%x:0x%x" % (TEXT_START, TEXT_END),
                        help="guest range to compare, start:end (default the code and data segment)")
    parser.add_argument("--kind", choices=("code", "all"), default="code",
                        help="code: only words where one side is an address in the game's code")
    parser.add_argument("--csv", default=str(FUNCTIONS_CSV), help="function map for names")
    parser.add_argument("--limit", type=int, default=4000, help="maximum differing words printed")
    args = parser.parse_args(argv[1:])

    start, end = parse_address_range(args.address_range)
    ours = Path(args.ours).read_bytes()
    reference = Path(args.reference).read_bytes()
    if len(ours) != len(reference):
        print("warning: the images have different sizes (%d and %d)" % (len(ours), len(reference)))
    end = min(end, len(ours), len(reference)) if end else min(len(ours), len(reference))
    if start >= end:
        print("empty range")
        return 2

    functions = load_functions(args.csv)
    differences = []
    for address, value in words(ours, start, end):
        other = struct.unpack_from("<I", reference, address)[0]
        if value == other:
            continue
        if args.kind == "code" and not (in_code(value) or in_code(other)):
            continue
        differences.append((address, value, other))

    print("range 0x%x:0x%x, kind %s: %d differing word(s) of %d" %
          (start, end, args.kind, len(differences), (end - start) // 4))
    for address, value, other in differences[:args.limit]:
        names = []
        for label, side in (("ours", value), ("ref", other)):
            name = function_name(functions, side) if in_code(side) else None
            names.append("%s=0x%08x%s" % (label, side, " (%s)" % name if name else ""))
        print("0x%08x  %s" % (address, "  ".join(names)))
    if len(differences) > args.limit:
        print("... %d more differing word(s) not printed (--limit %d)" %
              (len(differences) - args.limit, args.limit))

    print("groups of consecutive differing words (address range, count, first pair):")
    run_start = None
    run = []
    for address, value, other in differences:
        if run_start is not None and address == run[-1][0] + 4:
            run.append((address, value, other))
            continue
        if run:
            print("  0x%08x:0x%08x  %4d  ours=0x%08x ref=0x%08x" %
                  (run_start, run_start + 4 * len(run), len(run), run[0][1], run[0][2]))
        run_start = address
        run = [(address, value, other)]
    if run:
        print("  0x%08x:0x%08x  %4d  ours=0x%08x ref=0x%08x" %
              (run_start, run_start + 4 * len(run), len(run), run[0][1], run[0][2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
