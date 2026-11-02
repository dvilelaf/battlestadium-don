#!/usr/bin/env python3
"""Inventory of the imports and exports of the IOP modules (IRX) inside FPK packages.

Usage: irx_imports.py <package.FPK> [<package.FPK> ...] [--tsv]

Without --tsv it prints the module list with exports and imports. With --tsv it prints one
row per import with the column `supported` (yes / module / no, see classify()).

Each entry of the pack that is an IRX (32-bit little-endian MIPS ELF) is decompressed in
memory (nothing is written to disk) and parsed:
  - module name and version: section of type 0x70000080 (.iopmod);
  - import tables: 0x41E00000, 0, version word, 8-byte library name, then 8-byte stubs
    `jr $ra` (0x03E00008) + `li $zero, ordinal` (0x2400NNNN), ended by two zero words;
  - export tables: 0x41C00000, 0, version word, 8-byte library name, then function
    pointers up to a zero word.
The output holds module names, library names, versions and ordinals only (no code).
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import fpk  # noqa: E402

IMPORT_MAGIC = 0x41E00000
EXPORT_MAGIC = 0x41C00000
JR_RA = 0x03E00008
SHT_IOPMOD = 0x70000080
PT_LOAD = 1
SHT_REL = 9
R_MIPS_32 = 2


def is_irx(data):
    return len(data) > 0x34 and data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1


def _cstr(data):
    return data.split(b"\0")[0].decode("ascii", "replace")


def module_info(data):
    """Return (name, version) from the section of type 0x70000080, or None."""
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    for i in range(shnum):
        o = shoff + i * shentsize
        if o + 0x28 > len(data):
            break
        sh_type, = struct.unpack_from("<I", data, o + 4)
        if sh_type != SHT_IOPMOD:
            continue
        off, size = struct.unpack_from("<II", data, o + 0x10)
        sec = data[off:off + size]
        if len(sec) < 26:
            return None
        version, = struct.unpack_from("<H", sec, 24)
        return _cstr(sec[26:]), version
    return None


def loaded_image(data):
    """Return the PT_LOAD segments laid out by virtual address (zero-filled gaps and bss)."""
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    img = bytearray()
    for i in range(phnum):
        o = phoff + i * phentsize
        if o + 0x20 > len(data):
            break
        p_type, p_off, p_vaddr, _pa, p_filesz, p_memsz = struct.unpack_from("<6I", data, o)
        if p_type != PT_LOAD or not p_filesz:
            continue
        end = p_vaddr + p_filesz
        if len(img) < end:
            img.extend(b"\0" * (end - len(img)))
        img[p_vaddr:end] = data[p_off:p_off + p_filesz]
    return bytes(img)


def relocated_offsets(data):
    """Return the set of image offsets that the loader patches with an R_MIPS_32 relocation.

    Such a word is non-zero once loaded even if it is zero in the file (a pointer to offset 0).
    """
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    out = set()
    for i in range(shnum):
        o = shoff + i * shentsize
        if o + 0x28 > len(data):
            break
        sh_type, = struct.unpack_from("<I", data, o + 4)
        if sh_type != SHT_REL:
            continue
        off, size = struct.unpack_from("<II", data, o + 0x10)
        for k in range(size // 8):
            r_offset, r_info = struct.unpack_from("<II", data, off + k * 8)
            if (r_info & 0xFF) == R_MIPS_32:
                out.add(r_offset)
    return out


def _words(img):
    n = len(img) // 4
    return struct.unpack_from("<%dI" % n, img, 0)


def find_tables(img, relocated=frozenset()):
    """Return (imports, exports).

    imports: [(library, version, [ordinals])]; exports: [(library, version, nfunctions)].
    The export count follows the IOP emulator: function words up to the first word that is zero
    after loading (a word with a relocation counts as non-zero).
    """
    w = _words(img)
    n = len(w)
    imports, exports = [], []
    i = 0
    while i + 5 <= n:
        magic = w[i]
        if magic not in (IMPORT_MAGIC, EXPORT_MAGIC) or w[i + 1] != 0:
            i += 1
            continue
        version = w[i + 2] & 0xFFFF
        library = _cstr(img[(i + 3) * 4:(i + 5) * 4])
        j = i + 5
        if magic == IMPORT_MAGIC:
            ordinals = []
            while j + 1 < n:
                a, b = w[j], w[j + 1]
                if a == 0 and b == 0:
                    break
                if a != JR_RA or (b & 0xFFFF0000) != 0x24000000:
                    break
                ordinals.append(b & 0xFFFF)
                j += 2
            imports.append((library, version, ordinals))
        else:
            count = 0
            while j < n and (w[j] != 0 or j * 4 in relocated):
                count += 1
                j += 1
            exports.append((library, version, count))
        i = max(j, i + 5)
    return imports, exports


def parse_irx(data):
    """Return dict(name, version, imports, exports) or None when it is not a usable IRX."""
    if not is_irx(data):
        return None
    info = module_info(data)
    if info is None:
        return None
    imports, exports = find_tables(loaded_image(data), relocated_offsets(data))
    return {"name": info[0], "version": info[1], "imports": imports, "exports": exports}


def read_pack(path):
    """Yield (entry_name, parsed_module) for every IRX in the FPK at path."""
    buf = Path(path).read_bytes()
    parsed = fpk.parse(buf)
    if parsed is None:
        raise ValueError("not an FPK: %s" % path)
    _endian, ents = parsed
    for ent in ents:
        data = fpk.read_entry(buf, ent)
        mod = parse_irx(data)
        if mod is not None:
            yield ent[0], mod


def fmt_version(v):
    return "%d.%02d" % (v >> 8, v & 0xFF)


# ---------------------------------------------------------------- IOP emulator support

REPO = Path(__file__).resolve().parents[2]
EMU_DIR = REPO / "third_party" / "PS2Recomp" / "ps2xIOP" / "src" / "emulator"

# library (lower case) -> (source file under EMU_DIR, signature of the function whose
# `switch (ordinal)` decides it). Mirrors the if-chain of dispatchImport() in iop_emulator.cpp.
DISPATCHERS = {
    "sysmem": ("imports/iop_sysmem.cpp", "IopSysmem::dispatchImport"),
    "cdvdman": ("imports/iop_cdvd.cpp", "bool dispatchImport"),
    "loadcore": ("imports/iop_loadcore.cpp", "IopLoadcore::dispatchImport"),
    "thbase": ("core/iop_kernel.cpp", "IopKernel::dispatchThreadImport"),
    "threadman": ("core/iop_kernel.cpp", "IopKernel::dispatchThreadImport"),
    "thsemap": ("core/iop_kernel.cpp", "IopKernel::dispatchSemaphoreImport"),
    "thevent": ("core/iop_kernel.cpp", "IopKernel::dispatchEventImport"),
    "sifcmd": ("services/iop_rpc.cpp", "IopRpcBridge::dispatchSifCmdImport"),
    "sifman": ("services/iop_rpc.cpp", "IopRpcBridge::dispatchSifManImport"),
    "intrman": ("imports/iop_intrman.cpp", "IopIntrman::dispatchImport"),
    "ioman": ("imports/iop_ioman.cpp", "IopIoman::dispatchImport"),
    "vblank": ("imports/iop_vblank.cpp", "IopVblank::dispatchImport"),
    "timrman": ("imports/iop_timrman.cpp", "IopTimrman::dispatchImport"),
    "stdio": ("imports/iop_stdio.cpp", "IopStdio::dispatchImport"),
    "sysclib": ("imports/iop_sysclib.cpp", "IopSysclib::dispatchImport"),
    "heaplib": ("imports/iop_heaplib.cpp", "IopHeaplib::dispatchImport"),
}

# Handled inline in IopEmulator::dispatchImport (iop_emulator.cpp): library -> ordinals, None = all.
INLINE = {
    "secrman": {4, 5},
    "modload": {13},
    "dmacman": None,
}


def case_labels(source, signature):
    """Numeric `case N:` labels directly inside the first `switch (ordinal)` of a function."""
    start = source.index(signature)
    sw = source.index("switch (ordinal)", start)
    i = source.index("{", sw)
    depth = 0
    labels = set()
    pos = i
    n = len(source)
    while pos < n:
        ch = source[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                break
        elif depth == 1 and source.startswith("case ", pos) and not source[pos - 1].isalnum():
            m = re.match(r"case\s+(\d+)\s*:", source[pos:pos + 24])
            if m:
                labels.add(int(m.group(1)))
        pos += 1
    return labels


def emulator_support(emu_dir=EMU_DIR):
    """Return {library: set of ordinals, or None for 'every ordinal'} handled by the emulator."""
    out = {}
    cache = {}
    for lib, (rel, sig) in DISPATCHERS.items():
        if rel not in cache:
            cache[rel] = (Path(emu_dir) / rel).read_text(encoding="utf-8", errors="replace")
        out[lib] = case_labels(cache[rel], sig)
    for lib, ords in INLINE.items():
        out[lib] = None if ords is None else set(ords)
    return out


def classify(library, version, ordinal, support, providers):
    """Return (supported, provider): 'yes' (emulator), 'module' (exported by a game module), 'no'.

    The emulator is asked first (iop_emulator.cpp dispatchImport); only when it does not handle
    the (library, ordinal) does IopImportRegistry::resolve look at the export tables of loaded
    modules: same name (case-insensitive), same major version, ordinal inside the table.
    """
    lib = library.lower()
    if lib in support and (support[lib] is None or ordinal in support[lib]):
        return "yes", ""
    best = None
    for pname, pver, count in providers.get(lib, []):
        if (pver >> 8) != (version >> 8):
            continue
        if best is None or pver > best[1]:
            best = (pname, pver, count)
    if best is None:
        return "no", ""
    return ("module" if ordinal < best[2] else "no"), best[0]


def module_key(entry_name):
    """File stem of the pack entry ('modules/libsd.irx' -> 'libsd')."""
    return Path(entry_name).stem.lower()


def load_modules(paths):
    """Return [(package, key, parsed_module)] for all packs."""
    out = []
    for p in paths:
        for entry, mod in read_pack(p):
            out.append((Path(p).name, module_key(entry), mod))
    return out


def provider_index(modules):
    idx = {}
    for _pack, key, mod in modules:
        for lib, ver, count in mod["exports"]:
            idx.setdefault(lib.lower(), []).append((key, ver, count))
    return idx


TSV_HEADER = ("module", "module_name", "module_version", "imported_library", "version",
              "ordinal", "supported", "provider", "package")


def tsv_rows(paths, emu_dir=EMU_DIR):
    modules = load_modules(paths)
    support = emulator_support(emu_dir)
    providers = provider_index(modules)
    for pack, key, mod in modules:
        for lib, ver, ords in mod["imports"]:
            for o in ords:
                sup, prov = classify(lib, ver, o, support, providers)
                yield (key, mod["name"], fmt_version(mod["version"]), lib, fmt_version(ver),
                       o, sup, prov, pack)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    flags = {a for a in argv if a.startswith("--")}
    if not args:
        print(__doc__)
        return 2
    if "--tsv" in flags:
        print("\t".join(TSV_HEADER))
        for row in tsv_rows(args):
            print("\t".join(str(c) for c in row))
        return 0
    for p in args:
        for entry, mod in read_pack(p):
            print("%s\t%s\tmodule %s v%s" % (Path(p).name, entry, mod["name"], fmt_version(mod["version"])))
            for lib, ver, count in mod["exports"]:
                print("  exports\t%s v%s\t%d functions" % (lib, fmt_version(ver), count))
            for lib, ver, ords in mod["imports"]:
                print("  imports\t%s v%s\t%d: %s" % (lib, fmt_version(ver), len(ords), " ".join(map(str, ords))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
