"""Count the sites of each mnemonic in the game's code range (needs disc/SLPS_256.75 and rabbitizer).

The tests never call this; make_table.py does, when the file is there.
"""
from __future__ import annotations

import struct
from collections import Counter
from pathlib import Path

CODE_START, CODE_END = 0x100000, 0x3902D4


def code_words(elf: Path):
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF")
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    for i in range(phnum):
        typ, off, vaddr, _paddr, filesz, _memsz, _flags, _align = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if typ == 1 and vaddr <= CODE_START < vaddr + filesz:
            lo = off + (CODE_START - vaddr)
            n = (min(CODE_END, vaddr + filesz) - CODE_START) // 4
            return struct.unpack_from(f"<{n}I", data, lo)
    raise ValueError("code range not found in the ELF")


def count_sites(elf: Path) -> Counter:
    import rabbitizer
    counts: Counter = Counter()
    for i, word in enumerate(code_words(elf)):
        ins = rabbitizer.Instruction(word, vram=CODE_START + 4 * i, category=rabbitizer.InstrCategory.R5900)
        if ins.isValid():
            name = ins.getOpcodeName()
            if name == "c1" and word >> 21 == 0x230 and word & 63 == 4:   # rabbitizer does not know sqrt.s
                name = "sqrt.s"
            counts[name] += 1
    return counts
