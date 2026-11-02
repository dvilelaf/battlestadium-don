#!/usr/bin/env python3
"""Build step: translates the IOP sound module cri_adxi of the user's own disc to C++ (generated/iop_native/).

Finds cri_adxi's IRX in the extracted disc (MODULES/1000.FPK, else the loose MODULES/CRI_ADXI.IRX), relocates it to the
address the port's IOP emulator loads it at (0x6e000: the base the emitter and the runtime hash guard assume), runs the
emitter (emit_adxi.py) and writes generated/iop_native/adxi_native.cpp, only if its content changed (so the build does
not recompile it for nothing). Derived from game code: git-ignored, never versioned. When the module is not on the
disc the script says so and exits 0 without touching anything: the build then goes on as before, without it.
If the code at that address differs at run time (another disc variant, another load order) the emulator's hash guard
refuses the module, so the game still runs, interpreted.

Usage: uv run --no-project --with rabbitizer python -I tools/iop_recomp/make_native.py [--disc disc] [--out F]
Exit code: 0 done or nothing to do; 1 the IRX was found but could not be translated.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))  # tools/fpk.py

import emit_adxi  # noqa: E402
import fpk  # noqa: E402

IMAGE_SIZE = 0x20000
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def find_irx(disc: Path) -> bytes | None:
    pack = disc / "MODULES" / "1000.FPK"
    if pack.is_file():
        data = pack.read_bytes()
        parsed = fpk.parse(data)
        if parsed:
            for ent in parsed[1]:
                if ent[0].lower().endswith("cri_adxi.irx"):
                    return fpk.read_entry(data, ent)
    loose = disc / "MODULES" / "CRI_ADXI.IRX"
    return loose.read_bytes() if loose.is_file() else None


def relocate(irx: bytes, base: int) -> bytes:
    """Flat image of an IRX (ET_SCE_IOPRELEXEC) with its R_MIPS_32/26/HI16/LO16 relocations applied for `base`."""
    shoff, = struct.unpack_from("<I", irx, 32)
    shentsize, shnum = struct.unpack_from("<HH", irx, 46)
    secs = [struct.unpack_from("<10I", irx, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, align, entsize)
    img = bytearray(IMAGE_SIZE)
    for s in secs:
        if s[1] == 1 and s[5] and s[4]:
            img[s[3]:s[3] + s[5]] = irx[s[4]:s[4] + s[5]]

    def word(a: int) -> int:
        return struct.unpack_from("<I", img, a)[0]

    def put(a: int, w: int) -> None:
        struct.pack_into("<I", img, a, w & 0xFFFFFFFF)

    for s in secs:
        if s[1] != 9:  # SHT_REL
            continue
        target = secs[s[7]]
        pending_hi: list[int] = []
        for i in range(s[5] // 8):
            off, info = struct.unpack_from("<II", irx, s[4] + i * 8)
            kind, a = info & 0xFF, target[3] + off
            w = word(a)
            if kind == R_MIPS_32:
                w = (w + base) & 0xFFFFFFFF
            elif kind == R_MIPS_26:
                w = (w & 0xFC000000) | (((w & 0x3FFFFFF) + (base >> 2)) & 0x3FFFFFF)
            elif kind == R_MIPS_HI16:
                pending_hi.append(a)
                continue
            elif kind == R_MIPS_LO16 and pending_hi:
                lo = w & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                full = ((word(pending_hi[-1]) & 0xFFFF) << 16) + lo + base
                new_lo = full & 0xFFFF
                new_hi = ((full - (new_lo - 0x10000 if new_lo & 0x8000 else new_lo)) >> 16) & 0xFFFF
                for h in pending_hi:
                    put(h, (word(h) & 0xFFFF0000) | new_hi)
                pending_hi = []
                w = (w & 0xFFFF0000) | new_lo
            put(a, w)
    return bytes(img)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disc", type=Path, default=Path("disc"), help="folder with the extracted disc (MODULES/...)")
    ap.add_argument("--out", type=Path, default=Path("generated/iop_native/adxi_native.cpp"))
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x6E000, help="address the emulator loads cri_adxi at")
    ap.add_argument("--code-lo", type=lambda s: int(s, 0), default=0x6E000)
    ap.add_argument("--code-hi", type=lambda s: int(s, 0), default=0x78500, help="exclusive end of the code")
    args = ap.parse_args(argv)

    irx = find_irx(args.disc)
    if irx is None:
        print(f"[iop-native] cri_adxi not found under {args.disc}/MODULES: nothing generated, the build goes on without it")
        return 0
    try:
        img = emit_adxi.Image(relocate(irx, args.base), args.base)
        analysis = emit_adxi.Analysis(img, args.code_lo, args.code_hi)
        emitter = emit_adxi.Emitter(analysis, disasm=False, ns="adxi_native")
        text = emitter.emit()
    except Exception as exc:  # noqa: BLE001 - report any failure of a damaged or unexpected module
        print(f"[iop-native] cri_adxi could not be translated: {exc!r}", file=sys.stderr)
        return 1
    st = emitter.stats
    note = (f"{st['functions']} functions (refused {st['refused']}), {st['blocks']} blocks, "
            f"{st['translated_instructions']} instructions, {st['entries']} entry points")
    if args.out.is_file() and args.out.read_text() == text:
        print(f"[iop-native] {args.out} is up to date ({note})")
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text)
    print(f"[iop-native] wrote {args.out} ({len(text)} bytes; {note})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
