"""Tests for tools/re/irx_imports.py on a synthetic IRX module (no game files)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "re"))

import irx_imports as irx  # noqa: E402

JR_RA = 0x03E00008


def li_zero(n):
    return 0x24000000 | n


def libname(name):
    return name.encode().ljust(8, b"\0")


def build_irx(name="testmod", version=0x0102, imports=(("loadcore", 0x0103, (5, 6)),),
              export=("testlib", 0x0101, (0x10, 0x20, 0x30))):
    # Loaded segment: import table(s), export table, zero padding.
    seg = b""
    for lib, ver, ords in imports:
        seg += struct.pack("<III", irx.IMPORT_MAGIC, 0, ver) + libname(lib)
        for o in ords:
            seg += struct.pack("<II", JR_RA, li_zero(o))
        seg += struct.pack("<II", 0, 0)
    exp_off = len(seg)
    ver, funcs = export[1], export[2]
    seg += struct.pack("<III", irx.EXPORT_MAGIC, 0, ver) + libname(export[0])
    seg += struct.pack("<%dI" % len(funcs), *funcs) + struct.pack("<I", 0)
    seg += b"\0" * 8

    iopmod = struct.pack("<6IH", 0, 0, 0, len(seg), 0, 0, version) + name.encode() + b"\0"
    ehsize, phentsize, shentsize = 0x34, 0x20, 0x28
    phoff = ehsize
    iopmod_off = phoff + phentsize
    seg_off = (iopmod_off + len(iopmod) + 3) & ~3
    shoff = seg_off + len(seg)
    ehdr = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    ehdr += struct.pack("<HHIIIIIHHHHHH", 0xFF80, 8, 1, 0, phoff, shoff, 0,
                        ehsize, phentsize, 1, shentsize, 3, 0)
    phdr = struct.pack("<8I", 1, seg_off, 0, 0, len(seg), len(seg), 7, 4)
    sh_null = b"\0" * shentsize
    sh_mod = struct.pack("<10I", 0, irx.SHT_IOPMOD, 0, 0, iopmod_off, len(iopmod), 0, 0, 1, 0)
    sh_text = struct.pack("<10I", 0, 1, 7, 0, seg_off, len(seg), 0, 0, 4, 0)
    blob = ehdr + phdr + iopmod
    blob += b"\0" * (seg_off - len(blob)) + seg
    blob += sh_null + sh_mod + sh_text
    return blob, exp_off


def test_module_info():
    data, _ = build_irx(name="mymodule", version=0x0305)
    assert irx.module_info(data) == ("mymodule", 0x0305)
    assert irx.fmt_version(0x0305) == "3.05"


def test_parse_imports_and_exports():
    data, _ = build_irx(imports=(("loadcore", 0x0103, (5, 6)), ("sysclib", 0x0104, (17,))))
    mod = irx.parse_irx(data)
    assert mod["name"] == "testmod"
    assert mod["version"] == 0x0102
    assert mod["imports"] == [("loadcore", 0x0103, [5, 6]), ("sysclib", 0x0104, [17])]
    assert mod["exports"] == [("testlib", 0x0101, 3)]


def test_two_stubs_and_terminator():
    data, _ = build_irx(imports=(("intrman", 0x0102, (17, 18)),))
    mod = irx.parse_irx(data)
    assert mod["imports"][0][2] == [17, 18]


def test_not_an_irx():
    assert irx.parse_irx(b"not an elf at all" * 4) is None
    assert irx.parse_irx(b"\x7fELF\x02\x01" + b"\0" * 64) is None


def test_relocated_zero_slot_counts_as_function():
    # A zero word with an R_MIPS_32 relocation is non-zero once loaded.
    seg = struct.pack("<III", irx.EXPORT_MAGIC, 0, 0x0101) + libname("zlib")
    seg += struct.pack("<4I", 0, 0x40, 0x50, 0) + b"\0" * 8
    imports, exports = irx.find_tables(seg, relocated=frozenset({20}))
    assert exports == [("zlib", 0x0101, 3)]
    imports, exports = irx.find_tables(seg)
    assert exports == [("zlib", 0x0101, 0)]


def test_case_labels_only_top_level():
    src = """
    bool X::dispatchImport(uint16_t ordinal, Cpu &cpu)
    {
        switch (ordinal)
        {
        case 4: // a
            return true;
        case 5:
        case 9:
        {
            switch (cpu.mode)
            {
            case 77:
                break;
            }
            return true;
        }
        default:
            return false;
        }
    }
    """
    assert irx.case_labels(src, "X::dispatchImport") == {4, 5, 9}


def test_classify():
    support = {"loadcore": {5, 6}, "dmacman": None, "secrman": {4}}
    providers = {"libsd": [("libsd", 0x0105, 4)], "mcman": [("mcman", 0x0207, 2)]}
    assert irx.classify("loadcore", 0x0103, 5, support, providers) == ("yes", "")
    assert irx.classify("LoadCore", 0x0103, 7, support, providers) == ("no", "")
    assert irx.classify("dmacman", 0x0102, 999, support, providers) == ("yes", "")
    assert irx.classify("secrman", 0x0103, 6, support, providers) == ("no", "")
    assert irx.classify("libsd", 0x0105, 3, support, providers) == ("module", "libsd")
    assert irx.classify("libsd", 0x0105, 4, support, providers) == ("no", "libsd")
    # different major version: the registry does not link it
    assert irx.classify("mcman", 0x0107, 1, support, providers) == ("no", "")


def test_read_pack_and_tsv(tmp_path):
    import fpk

    mod_a, _ = build_irx(name="aaa", imports=(("loadcore", 0x0103, (5, 6)),),
                         export=("alib", 0x0101, (1, 2, 3)))
    mod_b, _ = build_irx(name="bbb", imports=(("alib", 0x0101, (2, 9)),),
                         export=("blib", 0x0101, (1,)))
    names = ["modules/aaa.irx", "modules/bbb.irx"]
    body = b""
    ents = []
    off = 0x10 + 0x30 * len(names)
    for n, d in zip(names, (mod_a, mod_b)):
        ents.append((n, off, len(d), len(d)))
        body += d
        off += len(d)
    head = struct.pack("<4I", 0, len(names), 0x10, off)
    for n, o, p, u in ents:
        head += n.encode().ljust(0x20, b"\0") + struct.pack("<4I", 0, o, p, u)
    pack = tmp_path / "T.FPK"
    pack.write_bytes(head + body)
    assert fpk.parse(pack.read_bytes()) is not None

    modules = irx.load_modules([str(pack)])
    assert [(k, m["name"]) for _p, k, m in modules] == [("aaa", "aaa"), ("bbb", "bbb")]
    idx = irx.provider_index(modules)
    support = {"loadcore": {5}}
    assert irx.classify("alib", 0x0101, 2, support, idx) == ("module", "aaa")
    assert irx.classify("alib", 0x0101, 9, support, idx) == ("no", "aaa")
    assert irx.classify("loadcore", 0x0103, 6, support, idx) == ("no", "")
