#!/usr/bin/env python3
"""Extract the components of a PCSX2 savestate (.p2s) and analyse the main memory image.

Usage:
  p2s_extract.py <savestate.p2s> <folder> [--elf disc/SLPS_256.75] [--layout config/layout.toml]
  p2s_extract.py extract <savestate.p2s> <folder> ...            (same thing)
  p2s_extract.py code-pointers <folder|ee_ram.bin> [--layout ...] [--csv ...] [--elf ...]
  p2s_extract.py vu1-program <folder|vu1_micro.bin> [--programs ...] [--elf ...]

A .p2s is a ZIP (PCSX2 v2.8.2, SaveState.cpp). Entries are stored, Deflate or Zstandard
(ZIP method 93; the default of v2.8.2 is Zstandard, level 3). Python's zipfile cannot read
Zstandard, so the ZIP is parsed here and Zstandard is decoded with, in this order: the
`zstandard` package, the stdlib `compression.zstd` (Python 3.14+), the `zstd` command line tool.
Nothing else is needed (`uv run --no-project --with zstandard python -I ...` is the portable way).

The savestate contains game memory: extract only into work/, never version the output.
"""
import argparse
import json
import shutil
import struct
import subprocess
import sys
import tomllib
import zipfile
import zlib
from array import array
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / 'tools' / 'funcmap'))
import layout  # noqa: E402

METHODS = {0: 'store', 8: 'deflate', 20: 'zstd (deprecated id)', 93: 'zstd'}
ZSTD_METHODS = (20, 93)

ENTRY_VERSION = 'PCSX2 Savestate Version.id'
# ZIP entry name -> (fixed output name, expected size or None); sizes from MemoryTypes.h / VUmicro.h
ENTRIES = {
    'eeMemory.bin': ('ee_ram.bin', 32 * 1024 * 1024),
    'iopMemory.bin': ('iop_ram.bin', 2 * 1024 * 1024),
    'eeHwRegs.bin': ('ee_hw.bin', 0x10000),
    'iopHwRegs.bin': ('iop_hw.bin', 0x10000),
    'Scratchpad.bin': ('scratchpad.bin', 0x4000),
    'vu0Memory.bin': ('vu0_mem.bin', 0x1000),
    'vu1Memory.bin': ('vu1_mem.bin', 0x4000),
    'vu0MicroMem.bin': ('vu0_micro.bin', 0x1000),
    'vu1MicroMem.bin': ('vu1_micro.bin', 0x4000),
    'SPU2.bin': ('spu2.bin', None),
    'USB.bin': ('usb.bin', None),
    'PAD.bin': ('pad.bin', None),
    'GS.bin': ('gs.bin', None),
    'Achievements.bin': ('achievements.bin', None),
    'PCSX2 Internal Structures.dat': ('internal.bin', None),
    'Screenshot.png': ('screenshot.png', None),
}

VU1_INSTRS = 0x4000 // 8
MPG_CMD = 0x4A
DEFAULT_ELF = REPO / 'disc' / 'SLPS_256.75'
DEFAULT_LAYOUT = REPO / 'config' / 'layout.toml'
DEFAULT_CSV = REPO / 'config' / 'functions.ghidra.csv'
DEFAULT_EXTRA = [REPO / 'config' / 'extra_entry_points.txt', REPO / 'config' / 'manual_entry_points.txt']
DEFAULT_VU = REPO / 'docs' / 'knowledge' / 'data' / 'vu_programs.tsv'


class P2sError(Exception):
    pass


# ---------------------------------------------------------------- Zstandard / ZIP

def zstd_decompress(raw):
    """Decode one Zstandard frame stream with whichever backend exists."""
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(raw)
    except ImportError:
        pass
    try:
        from compression import zstd  # Python 3.14+
        return zstd.decompress(raw)
    except ImportError:
        pass
    exe = shutil.which('zstd')
    if exe:
        r = subprocess.run([exe, '-d', '-c', '-q'], input=raw, capture_output=True)
        if r.returncode:
            raise P2sError(f'zstd failed: {r.stderr.decode(errors="replace").strip()}')
        return r.stdout
    raise P2sError('the savestate uses Zstandard and no decoder was found: run with '
                   '`uv run --no-project --with zstandard python -I ...` or install the zstd tool')


def zstd_backend():
    try:
        import zstandard  # noqa: F401
        return 'zstandard'
    except ImportError:
        pass
    try:
        from compression import zstd  # noqa: F401
        return 'compression.zstd'
    except ImportError:
        pass
    return 'zstd-cli' if shutil.which('zstd') else None


def read_entry(fp, info):
    """Return the decompressed bytes of one ZIP entry (CRC and size checked)."""
    if info.flag_bits & 1:
        raise P2sError(f'{info.filename}: encrypted entry')
    fp.seek(info.header_offset)
    h = fp.read(30)
    if len(h) != 30 or h[:4] != b'PK\x03\x04':
        raise P2sError(f'{info.filename}: bad local header')
    nlen, elen = struct.unpack_from('<HH', h, 26)
    fp.seek(info.header_offset + 30 + nlen + elen)
    raw = fp.read(info.compress_size)
    if len(raw) != info.compress_size:
        raise P2sError(f'{info.filename}: truncated data')
    m = info.compress_type
    if m == 0:
        data = raw
    elif m == 8:
        data = zlib.decompressobj(-15).decompress(raw)
    elif m in ZSTD_METHODS:
        data = zstd_decompress(raw)
    else:
        raise P2sError(f'{info.filename}: unsupported ZIP method {m}')
    if len(data) != info.file_size:
        raise P2sError(f'{info.filename}: size {len(data)} != {info.file_size}')
    if zlib.crc32(data) != info.CRC:
        raise P2sError(f'{info.filename}: CRC mismatch')
    return data


def parse_version(data):
    if len(data) < 4:
        return {'raw_size': len(data)}
    ver, = struct.unpack_from('<I', data)
    text = data[4:36].split(b'\0')[0].decode('ascii', 'replace')
    return {'save_version': f'0x{ver:08X}', 'major': ver >> 16, 'minor': ver & 0xFFFF, 'pcsx2': text}


# ---------------------------------------------------------------- extract

def load_elf_segment(elf):
    """(load address, file bytes) of the executable, or None if it is not available."""
    if not elf or not Path(elf).is_file():
        return None
    _, va, data, _, _ = layout.load(elf)
    return va, data


def code_range_check(ram, seg, layout_path):
    """Compare the text range of the executable with the same range of the main memory image."""
    if seg is None or not layout_path or not Path(layout_path).is_file():
        return None
    t = tomllib.loads(Path(layout_path).read_text())['text']
    va, data = seg
    start, end = t['start'], t['end']
    a = ram[start:end]
    b = data[start - va:end - va]
    if len(a) != len(b):
        return {'start': start, 'end': end, 'error': 'range outside the memory image'}
    diff = sum(1 for x, y in zip(a, b) if x != y) if a != b else 0
    return {'start': f'0x{start:08X}', 'end': f'0x{end:08X}', 'bytes': len(a), 'differing_bytes': diff}


def cmd_extract(args):
    src = Path(args.savestate)
    out = Path(args.folder)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {'source': src.name, 'entries': []}
    with open(src, 'rb') as fp:
        try:
            zf = zipfile.ZipFile(fp)
        except zipfile.BadZipFile as e:
            raise P2sError(f'{src}: not a ZIP file ({e})')
        infos = zf.infolist()
        names = {i.filename for i in infos}
        if ENTRY_VERSION in names:
            manifest['savestate_version'] = parse_version(read_entry(fp, zf.getinfo(ENTRY_VERSION)))
        else:
            print(f'[p2s] warning: no "{ENTRY_VERSION}" entry', file=sys.stderr)
        ram = None
        for i in infos:
            e = {'name': i.filename, 'size': i.file_size, 'stored_size': i.compress_size,
                 'method': i.compress_type, 'method_name': METHODS.get(i.compress_type, '?')}
            target = ENTRIES.get(i.filename)
            if target:
                data = read_entry(fp, i)
                e['extracted_as'] = target[0]
                (out / target[0]).write_bytes(data)
                if target[1] is not None and len(data) != target[1]:
                    e['note'] = f'expected 0x{target[1]:X} bytes'
                    print(f'[p2s] warning: {i.filename} is 0x{len(data):X} bytes, expected 0x{target[1]:X}',
                          file=sys.stderr)
                if i.filename == 'eeMemory.bin':
                    ram = data
                if i.filename == 'GS.bin' and len(data) >= 4:
                    manifest['gs_state_version'] = struct.unpack_from('<I', data)[0]
            manifest['entries'].append(e)
    missing = [k for k in ('eeMemory.bin', 'iopMemory.bin', 'vu1MicroMem.bin', 'vu1Memory.bin', 'GS.bin')
               if k not in names]
    if missing:
        manifest['missing_required'] = missing
        print(f'[p2s] warning: missing entries: {", ".join(missing)}', file=sys.stderr)
    if ram is not None:
        elf = args.elf if args.elf else DEFAULT_ELF
        chk = code_range_check(ram, load_elf_segment(elf), args.layout)
        if chk is None:
            print('[p2s] code range check skipped (executable or layout not available)')
        else:
            manifest['code_range_check'] = chk
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'[p2s] {src.name}: {len(manifest["entries"])} entries, version '
          f'{manifest.get("savestate_version", {}).get("save_version", "?")}')
    for e in manifest['entries']:
        print(f'  {e["name"]:34} {e["size"]:>10}  {e["method_name"]:8} -> {e.get("extracted_as", "(not extracted)")}')
    chk = manifest.get('code_range_check')
    if chk:
        print(f'[p2s] code range {chk.get("start")}-{chk.get("end")}: '
              f'{chk.get("differing_bytes", "?")} differing bytes of {chk.get("bytes", "?")}')
    return 0


# ---------------------------------------------------------------- shared helpers

def find_component(path, name):
    p = Path(path)
    if p.is_dir():
        p = p / name
    if not p.is_file():
        raise P2sError(f'{p}: not found (run the extraction first)')
    return p.read_bytes()


def read_address_list(paths):
    out = set()
    for p in paths:
        p = Path(p)
        if not p.is_file():
            continue
        for line in p.read_text().splitlines():
            line = line.split('#')[0].strip()
            if line:
                out.add(int(line, 16))
    return out


# ---------------------------------------------------------------- code-pointers

def is_call(word):
    op = word >> 26
    if op == 0x03:
        return True  # jal
    if op == 0 and word & 0x3F == 0x09:
        return True  # jalr
    if op == 0x01 and (word >> 16) & 0x1F in (0x10, 0x11, 0x12, 0x13):
        return True  # bltzal, bgezal and their likely forms (bal)
    return False


JR_RA = 0x03E00008


def row_names(rows, addrs):
    """{address: name of the smallest row that contains it} for rows (name, start, end)."""
    out = {}
    active = []
    i = 0
    srt = sorted(rows, key=lambda r: r[1])
    for a in sorted(addrs):
        while i < len(srt) and srt[i][1] <= a:
            active.append(srt[i])
            i += 1
        active = [r for r in active if r[2] > a]
        if active:
            out[a] = min(active, key=lambda r: r[2] - r[1])[0]
    return out


def cmd_code_pointers(args):
    ram = find_component(args.savestate_dir, 'ee_ram.bin')
    t = tomllib.loads(Path(args.layout).read_text())['text']
    ts, te = t['start'], t['end']
    _, va, data, _, _ = layout.load(args.elf)
    img_lo, img_hi = va, va + len(data)
    rows = [r for r in layout.read_rows(args.csv) if ts <= r[1] < te]
    n = (te - ts) // 4
    covered = bytearray(n)
    starts = {}
    for name, s, e in rows:
        starts.setdefault(s, name)
        a = (s - ts) // 4
        b = min(n, (e - ts + 3) // 4)
        if b > a:
            covered[a:b] = b'\x01' * (b - a)
    extra = read_address_list(args.extra)

    words = array('I')
    words.frombytes(ram[:len(ram) // 4 * 4])
    if sys.byteorder == 'big':
        words.byteswap()
    lo_w, hi_w = img_lo // 4, img_hi // 4
    first = {}
    count = {}
    scanned = 0
    for i, w in enumerate(words):
        if lo_w <= i < hi_w:
            continue
        scanned += 1
        if ts <= w < te and not w & 3:
            count[w] = count.get(w, 0) + 1
            if w not in first:
                first[w] = i * 4

    def is_return(a):
        off = a - 8 - va
        if off < 0 or off + 4 > len(data) or a - 8 < ts:
            return False
        return is_call(struct.unpack_from('<I', data, off)[0])

    def word_at(a):
        off = a - va
        if off < 0 or off + 4 > len(data):
            return None
        return struct.unpack_from('<I', data, off)[0]

    def flags(a):
        """P: addiu/daddiu $sp, $sp, -N in the first two instructions. E: preceded by `jr $ra` + delay slot."""
        f = ''
        for k in (0, 4):
            w = word_at(a + k)
            if (w is not None and w >> 26 in (0x09, 0x19) and (w >> 21) & 31 == 29 and (w >> 16) & 31 == 29
                    and w & 0x8000):
                f += 'P'
                break
        if word_at(a - 8) == JR_RA:
            f += 'E'
        return f or '-'

    classes = {'row_start': 0, 'listed_extra': 0, 'inside_row': 0, 'no_row': 0}
    uncovered = []
    returns = []
    for a in sorted(count):
        if a in starts:
            classes['row_start'] += 1
        elif a in extra:
            classes['listed_extra'] += 1
        else:
            inside = covered[(a - ts) // 4]
            classes['inside_row' if inside else 'no_row'] += 1
            (returns if is_return(a) else uncovered).append((a, bool(inside)))

    print(f'[p2s] scanned {scanned} words of main memory outside the executable image '
          f'0x{img_lo:08X}-0x{img_hi:08X}')
    print(f'[p2s] code-pointer values (multiples of 4 in 0x{ts:08X}-0x{te:08X}): '
          f'{sum(count.values())} occurrences, {len(count)} distinct')
    print(f'  start of a row:                        {classes["row_start"]}')
    print(f'  listed in the entry-point files:       {classes["listed_extra"]}')
    print(f'  not covered, inside a row:             {classes["inside_row"]}')
    print(f'  not covered, in no row:                {classes["no_row"]}')
    print(f'  of the not covered: return addresses   {len(returns)} (previous pair is a call and its delay slot)')
    print(f'  of the not covered: others             {len(uncovered)}')

    like = [x for x in uncovered if flags(x[0]) != '-']
    print(f'  of the others: entry-like (prologue P, or after a jr $ra E)    {len(like)}')
    print(f'  of the others: not entry-like (mostly integers and data)       {len(uncovered) - len(like)}')

    def show(title, items, limit):
        print(f'\n{title} ({len(items)}): address  class  flags  first found at  occurrences')
        for a, inside in items[:limit] if limit else items:
            print(f'  0x{a:08X}  {"inside_row" if inside else "no_row    "}  {flags(a):2}  0x{first[a]:08X}  {count[a]}')
        if limit and len(items) > limit:
            print(f'  ... {len(items) - limit} more (use --limit 0 for all)')

    show('Uncovered code pointers that are not return addresses', uncovered, args.limit)
    if args.show_returns:
        show('Uncovered return addresses', returns, args.limit)
    if args.tsv:
        names = row_names(rows, [a for a, _ in uncovered + returns])
        with open(args.tsv, 'w') as f:
            f.write('address\tclass\trow\tflags\treturn_address\tfirst_found_at\toccurrences\n')
            for ret, items in ((0, uncovered), (1, returns)):
                for a, inside in items:
                    f.write(f'0x{a:08X}\t{"inside_row" if inside else "no_row"}\t{names.get(a, "-")}\t{flags(a)}\t'
                            f'{ret}\t0x{first[a]:08X}\t{count[a]}\n')
    return 0


# ---------------------------------------------------------------- vu1-program

def program_blocks(data, va, elf_addr, nbytes):
    """Parse a VIF stream of MPG blocks: [(first instruction index, bytes of the block)]."""
    off = elf_addr - va
    blob = data[off:off + nbytes]
    p = 0
    blocks = []
    while p + 4 <= len(blob):
        w, = struct.unpack_from('<I', blob, p)
        p += 4
        if (w >> 24) & 0x7F == MPG_CMD:
            num = (w >> 16) & 0xFF or 256
            body = blob[p:p + num * 8]
            if len(body) != num * 8:
                break
            blocks.append(((w & 0xFFFF) % VU1_INSTRS, body))
            p += num * 8
    return blocks


def cmd_vu1_program(args):
    micro = find_component(args.savestate_dir, 'vu1_micro.bin')
    if len(micro) != VU1_INSTRS * 8:
        raise P2sError(f'vu1_micro.bin is {len(micro)} bytes, expected {VU1_INSTRS * 8}')
    _, va, data, _, _ = layout.load(args.elf)
    import csv
    with open(args.programs, newline='') as f:
        progs = list(csv.DictReader(f, delimiter='\t'))
    nonzero = [micro[i * 8:i * 8 + 8] != b'\0' * 8 for i in range(VU1_INSTRS)]
    mismatch_tsv = 0
    infos = []  # (elf_addr, vu_addr, instrs, set of memory positions equal to the program's instruction)
    for r in progs:
        blocks = program_blocks(data, va, int(r['elf_addr'], 16), int(r['bytes']))
        ninstr = sum(len(b) // 8 for _, b in blocks)
        if blocks and (ninstr != int(r['instrs']) or blocks[0][0] != int(r['vu_addr'], 16)):
            mismatch_tsv += 1
        eq = set()
        for idx, body in blocks:
            for k in range(len(body) // 8):
                if micro[(idx + k) * 8:(idx + k) * 8 + 8] == body[k * 8:k * 8 + 8]:
                    eq.add(idx + k)
        infos.append((r['elf_addr'], r['vu_addr'], ninstr, eq))
    full = [i for i in infos if i[2] and len(i[3]) == i[2]]
    partial = sorted((i for i in infos if i[2] and len(i[3]) < i[2] and len(i[3]) >= args.min_fraction * i[2]),
                     key=lambda i: -len(i[3]) / i[2])
    none = len(infos) - len(full) - len(partial)
    union = set().union(*(i[3] for i in infos)) if infos else set()
    # greedy decomposition: the program that explains the most still unexplained instructions, until the gain is small
    left = set(union)
    chosen = []
    cands = [i for i in infos if i[3]]
    while left:
        best = max(cands, key=lambda i: len(i[3] & left), default=None)
        gain = len(best[3] & left) if best else 0
        if gain < args.min_gain:
            break
        chosen.append((best, gain))
        left -= best[3]
    nz = sum(nonzero)
    covered = VU1_INSTRS - len(left) - len(set(range(VU1_INSTRS)) - union)
    print(f'[p2s] {len(progs)} programs: {len(full)} loaded in full, {len(partial)} partial '
          f'(at least {args.min_fraction:.0%} of the instructions equal at their target address), {none} not found')
    if mismatch_tsv:
        print(f'[p2s] warning: {mismatch_tsv} program(s) whose MPG blocks disagree with the TSV (instrs or vu_addr)')
    print(f'[p2s] VU1 code memory: {VU1_INSTRS} instructions, {nz} not zero')
    print(f'  equal to an instruction of some program at the same address: {len(union)} '
          f'({100 * len(union) / VU1_INSTRS:.1f}% of the memory)')
    print(f'  explained by the programs listed in the decomposition:        {covered} '
          f'({100 * covered / VU1_INSTRS:.1f}%)')
    print('\nLoaded in full: elf_addr  vu_addr(instr)  instrs')
    for e in full:
        print(f'  {e[0]}  {e[1]}  {e[2]}')
    print('\nPartial: elf_addr  vu_addr(instr)  instrs  equal  fraction')
    for e in partial[:args.limit or None]:
        print(f'  {e[0]}  {e[1]}  {e[2]}  {len(e[3])}  {len(e[3]) / e[2]:.2f}')
    print(f'\nDecomposition (greedy, gain >= {args.min_gain}): elf_addr  vu_addr(instr)  instrs  equal  new_instructions')
    for e, gain in chosen:
        print(f'  {e[0]}  {e[1]}  {e[2]}  {len(e[3])}  {gain}')
    return 0


# ---------------------------------------------------------------- main

def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract', help='extract the components of a savestate (default command)')
    e.add_argument('savestate')
    e.add_argument('folder')
    e.add_argument('--elf', default=None, help='executable for the code range check (default disc/SLPS_256.75)')
    e.add_argument('--layout', default=str(DEFAULT_LAYOUT))
    e.set_defaults(fn=cmd_extract)

    c = sub.add_parser('code-pointers', help='code pointers in the heap and stack that the function map does not cover')
    c.add_argument('savestate_dir', metavar='folder_or_ee_ram.bin')
    c.add_argument('--layout', default=str(DEFAULT_LAYOUT))
    c.add_argument('--csv', default=str(DEFAULT_CSV))
    c.add_argument('--elf', default=str(DEFAULT_ELF))
    c.add_argument('--extra', action='append', default=None,
                   help='entry-point file (repeatable; default extra_entry_points.txt and manual_entry_points.txt)')
    c.add_argument('--limit', type=int, default=100, help='lines to list (0 = all)')
    c.add_argument('--tsv', default=None, help='write every uncovered candidate (returns included) to this TSV')
    c.add_argument('--show-returns', action='store_true', help='also list the return addresses')
    c.set_defaults(fn=cmd_code_pointers)

    v = sub.add_parser('vu1-program', help='which microprograms of the executable are in VU1 code memory')
    v.add_argument('savestate_dir', metavar='folder_or_vu1_micro.bin')
    v.add_argument('--programs', default=str(DEFAULT_VU))
    v.add_argument('--elf', default=str(DEFAULT_ELF))
    v.add_argument('--min-fraction', type=float, default=0.5, help='fraction of equal instructions to report a partial match')
    v.add_argument('--min-gain', type=int, default=8, help='smallest gain of a program in the decomposition')
    v.add_argument('--limit', type=int, default=30, help='partial matches to list (0 = all)')
    v.set_defaults(fn=cmd_vu1_program)
    return ap


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] not in ('extract', 'code-pointers', 'vu1-program', '-h', '--help'):
        argv.insert(0, 'extract')
    args = build_parser().parse_args(argv)
    if getattr(args, 'extra', 1) is None:
        args.extra = DEFAULT_EXTRA
    try:
        return args.fn(args)
    except (P2sError, FileNotFoundError, OSError) as e:
        print(f'[p2s] error: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
