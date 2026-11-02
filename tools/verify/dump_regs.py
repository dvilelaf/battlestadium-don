#!/usr/bin/env python3
"""Print the privileged GS registers of a PCSX2 GS dump at every vertical blank.

Usage:
  dump_regs.py <file.gs.zst | file.gs> [--decode]

The dump format is described in docs/knowledge/15-gs-dumps.md (GS/GSDump.h, GSDump.cpp in PCSX2):
  [0xFFFFFFFF][header size u32][9 x u32 header][serial][screenshot][GS state][0x2000-byte register block]
  then packets: 0 transfer [path u8][size u32][data]; 1 vsync [field u8]; 2 read FIFO [size u32];
  3 registers [0x2000 bytes] (written just before each vsync packet).

For the 19 registers the game and the console's system software set (the same ones, in the same order,
as don_gs_vsync in src/gsbridge/don_gs.h) it prints the 64-bit values at the dump's start ("initial") and at
each vertical blank. With --decode the fields of PMODE, SMODE1, SMODE2, DISPFB1/2 and DISPLAY1/2 follow by name.

Only the standard library is used. `.zst` files are decompressed with the `zstd` command line tool
(`zstd -dc`). The dump is game data: read it from work/, never version it (AGENTS.md rule 1); the
register values themselves are hardware constants and may be quoted.
"""
import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

PRIV_BYTES = 0x2000
# Names and offsets inside the 0x2000-byte block, in the order of don_gs_vsync's array.
REGISTERS = [
    ('PMODE', 0x00), ('SMODE1', 0x10), ('SMODE2', 0x20), ('SRFSH', 0x30), ('SYNCH1', 0x40), ('SYNCH2', 0x50),
    ('SYNCV', 0x60), ('DISPFB1', 0x70), ('DISPLAY1', 0x80), ('DISPFB2', 0x90), ('DISPLAY2', 0xA0),
    ('EXTBUF', 0xB0), ('EXTDATA', 0xC0), ('EXTWRITE', 0xD0), ('BGCOLOR', 0xE0), ('CSR', 0x1000),
    ('IMR', 0x1010), ('BUSDIR', 0x1040), ('SIGLBLID', 0x1080),
]

# (field name, first bit, width)
FIELDS = {
    'PMODE': [('EN1', 0, 1), ('EN2', 1, 1), ('CRTMD', 2, 3), ('MMOD', 5, 1), ('AMOD', 6, 1), ('SLBG', 7, 1),
              ('ALP', 8, 8)],
    'SMODE1': [('RC', 0, 3), ('LC', 3, 7), ('T1248', 10, 2), ('SLCK', 12, 1), ('CMOD', 13, 2), ('EX', 15, 1),
               ('PRST', 16, 1), ('SINT', 17, 1), ('XPCK', 18, 1), ('PCK2', 19, 2), ('SPML', 21, 4),
               ('GCONT', 25, 1), ('PHS', 26, 1), ('PVS', 27, 1), ('PEHS', 28, 1), ('PEVS', 29, 1),
               ('CLKSEL', 30, 2), ('NVCK', 32, 1), ('SLCK2', 33, 1), ('VCKSEL', 34, 2), ('VHP', 36, 1)],
    'SMODE2': [('INT', 0, 1), ('FFMD', 1, 1), ('DPMS', 2, 2)],
    'DISPFB': [('FBP', 0, 9), ('FBW', 9, 6), ('PSM', 15, 5), ('DBX', 32, 11), ('DBY', 43, 11)],
    'DISPLAY': [('DX', 0, 12), ('DY', 12, 11), ('MAGH', 23, 4), ('MAGV', 27, 2), ('DW', 32, 12), ('DH', 44, 11)],
}
FIELD_SETS = {'PMODE': 'PMODE', 'SMODE1': 'SMODE1', 'SMODE2': 'SMODE2', 'DISPFB1': 'DISPFB', 'DISPFB2': 'DISPFB',
              'DISPLAY1': 'DISPLAY', 'DISPLAY2': 'DISPLAY'}


class DumpError(Exception):
    pass


def read_dump(path):
    """Returns the dump's bytes (decompressing .zst with the zstd tool)."""
    path = Path(path)
    if path.suffix == '.zst':
        tool = shutil.which('zstd')
        if not tool:
            raise DumpError('the zstd command line tool is needed to read .zst dumps')
        result = subprocess.run([tool, '-dc', str(path)], capture_output=True)
        if result.returncode != 0:
            raise DumpError(f'zstd failed: {result.stderr.decode(errors="replace").strip()}')
        return result.stdout
    if path.suffix == '.xz':
        raise DumpError('.xz dumps are not supported')
    return path.read_bytes()


def parse_registers(data):
    """Returns (serial, [(label, field, {name: value})]): "initial" first, then one entry per vsync."""
    if len(data) < 8 or struct.unpack_from('<I', data, 0)[0] != 0xFFFFFFFF:
        raise DumpError('not a GS dump with the new header (no 0xFFFFFFFF at the start)')
    header_size = struct.unpack_from('<I', data, 4)[0]
    if len(data) < 8 + 36:
        raise DumpError('truncated header')
    (state_version, state_size, serial_offset, serial_size, crc, ss_w, ss_h, ss_offset,
     ss_size) = struct.unpack_from('<9I', data, 8)
    serial = data[8 + serial_offset:8 + serial_offset + serial_size].decode('ascii', 'replace')
    pos = 8 + header_size + state_size
    if pos + PRIV_BYTES > len(data):
        raise DumpError('truncated: no initial register block')

    def block(at):
        values = {}
        for name, offset in REGISTERS:
            values[name] = struct.unpack_from('<Q', data, at + offset)[0]
        return values

    out = [('initial', None, block(pos))]
    pos += PRIV_BYTES
    pending = None
    vsync = 0
    while pos < len(data):
        packet = data[pos]
        pos += 1
        if packet == 0:
            if pos + 5 > len(data):
                break
            size = struct.unpack_from('<I', data, pos + 1)[0]
            pos += 5 + size
        elif packet == 1:
            if pos + 1 > len(data):
                break
            vsync += 1
            field = data[pos]
            pos += 1
            if pending is not None:
                out.append((f'vsync {vsync}', field, pending))
                pending = None
        elif packet == 2:
            pos += 4
        elif packet == 3:
            if pos + PRIV_BYTES > len(data):
                break
            pending = block(pos)
            pos += PRIV_BYTES
        else:
            raise DumpError(f'unknown packet id {packet} at offset {pos - 1}')
    return serial, out


def decode(register, value):
    """Fields of a register as 'NAME=value' text, '' for registers without a field table."""
    table = FIELDS.get(FIELD_SETS.get(register, ''))
    if not table:
        return ''
    return ' '.join(f'{name}={(value >> first) & ((1 << width) - 1)}' for name, first, width in table)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('dump')
    parser.add_argument('--decode', action='store_true', help='also print the fields by name')
    args = parser.parse_args(argv)
    try:
        serial, moments = parse_registers(read_dump(args.dump))
    except (DumpError, OSError) as error:
        print(f'dump_regs: {error}', file=sys.stderr)
        return 1
    print(f'# {args.dump} serial={serial} moments={len(moments)}')
    for label, field, values in moments:
        suffix = '' if field is None else f' (field {field})'
        print(f'== {label}{suffix}')
        for name, _ in REGISTERS:
            line = f'{name:<9} 0x{values[name]:016X}'
            if args.decode:
                fields = decode(name, values[name])
                if fields:
                    line += '  ' + fields
            print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
