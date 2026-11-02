"""Family H: the floating-point coprocessor (COP1): the arithmetic, the accumulator forms, compares and branches, the
conversions, the moves and the loads and stores. The reference is fpu_ref.py (FPU.cpp of the reference emulator).

The driver runs every case under the game's host mode (SSE control register 0xFFC0: round toward zero, flush-to-zero,
denormals-are-zero; src/host/fp_env.cpp), because that is how the game's code runs.

Operands are bit patterns of singles. The names are only for the failure messages.
"""
from __future__ import annotations

import struct

import asm
import fpu_ref
from asm import A0, A1, JR_RA, NOP, V0, V1

F0, ACC, FCR = fpu_ref.F0, fpu_ref.ACC, fpu_ref.FCR
FS, FT, FD = 1, 2, 3


def _bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


OPS = [
    0x00000000, 0x80000000,                       # +0, -0
    0x3F800000, 0xBF800000, 0x40000000, 0xC0000000, 0x40400000, 0x40800000, 0x41100000, 0x3F000000, 0x3FC00000,   # 1 -1 2 -2 3 4 9 .5 1.5
    0x3EAAAAAB, 0xC0490FDB, 0x42C80000, 0x3C23D70A,                      # ~1/3, -pi, 100, 0.01
    0x437F0000, _bits(0.00197001174),             # 255.0 and the factor that exposed the rounding mode (journal 016)
    0x3F800001, 0xBF800001, 0x3F7FFFFF,           # 1 + ulp, -(1 + ulp), 1 - ulp
    0x00800000, 0x80800000, 0x01000000,           # smallest normal, its negative, twice it
    0x007FFFFF, 0x00000001, 0x807FFFFF,           # patterns that would be denormal
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F000000, 0x7E800000, 0x7F7FFFFE,       # largest finite, its negative, large
    0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFFFFFFF,         # exponent all ones (infinity, NaN patterns)
    0x4B000000, 0x4B800000, 0x4B800001,           # 2^23, 2^24, 2^24+2
    0x4EFFFFFF, 0xCEFFFFFF, 0x4F000000, 0xCF000000, 0x4F000001, 0x4F800000, 0xCF800000, 0x5F000000,   # around +-2^31
]
SMALL = [0, 0x80000000, 0x3F800000, 0xBF800000, 0x40400000, 0x3EAAAAAB, 0x00800000, 0x007FFFFF, 0x7F7FFFFF, 0xFF7FFFFF,
         0x7F800000, 0x3F800001, 0x437F0000, 0x4F000000]
ACCS = [0x40490FDB, 0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0x00800000, 0x3F800001, 0x007FFFFF, 0x3EAAAAAB]
ACCS_CLAMPED = ACCS + [0x7F800000, 0xFF800000]    # exponent all ones: only the forms that clamp the accumulator
INTS = [0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x01000001, 0xFEFFFFFF, 0x7FFFFF80, 0x00FFFFFF, 0x80000001, 0x12345678]
FCRS = [0, 0x0083C078, 0x800000, 0x38, 0x3C000, 0x1]           # start values of FCR31: legal bits (and the rounding bit)
JUNK = [0x01000000, 0xFF7C3F87, 0xFFFFFFFF, 0x0000003F]       # bits the interpreter keeps and the recompiler does not
GPRS = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x1_00000000, 0xFFFFFFFF_80000000, 0xFFFFFFFF_FFFFFFFF, 0x12345678_9ABCDEF0,
        0x3F800000, 0x7F800000]


def klass(x: int) -> str:
    e = (x >> 23) & 0xFF
    return "zero" if x & 0x7FFFFFFF == 0 else "denormal" if e == 0 else "exp255" if e == 255 else "normal"


def family_fpu(prog, Form, Case, default_regs, hx, mem_start):
    base = default_regs()
    forms = []

    def st(**kw):
        regs = list(base)
        for name, v in kw.items():
            if name == "acc":
                regs[ACC] = (v, 0)
            elif name == "fcr":
                regs[FCR] = (v, 0)
            elif name == "gpr":
                for r, val in v.items():
                    regs[r] = (val & 0xFFFFFFFFFFFFFFFF, regs[r][1])
            else:
                regs[F0 + int(name[1:])] = (v, 0)
        return regs

    def add(name, variant, words, cases, **kw):
        f = Form(name, variant, prog.add(f"{name.replace('.', '_')}_{len(prog.funcs)}", words + [JR_RA, NOP]), **kw)
        f.cases = cases
        forms.append(f)

    IGN = 0x0003C078          # the sticky exception flags I D O U SI SD SO SU (no site in the game reads them)
    n = [0]

    def fcr():
        n[0] += 1
        return FCRS[n[0] % len(FCRS)]

    def binary_cases(a_vals, b_vals, accs=None, hasacc=False):
        out = []
        for a in a_vals:
            for b in b_vals:
                kw = {"f1": a, "f2": b, "fcr": fcr()}
                desc = f"fs={a:#010x} ft={b:#010x}"
                if hasacc:
                    kw["acc"] = accs[n[0] % len(accs)]
                    desc += f" acc={kw['acc']:#010x}"
                cl = {klass(a), klass(b)} | ({"acc:" + klass(kw["acc"])} if hasacc else set())
                out.append(Case(desc, st(**kw), cls="+".join(sorted(cl))))
        return out

    def unary_cases(vals):
        return [Case(f"fs={a:#010x}", st(f1=a, fcr=fcr()), cls=klass(a)) for a in vals]

    BIN = ["add.s", "sub.s", "mul.s", "div.s", "max.s", "min.s"]
    ACCW = ["adda.s", "suba.s", "mula.s"]          # write the accumulator
    ACCR = ["madd.s", "msub.s"]                    # read it, write fd
    ACCRW = ["madda.s", "msuba.s"]                 # read and write it
    ALT = {"div.s", "sqrt.s", "rsqrt.s"}
    for nme in BIN + ["rsqrt.s"]:
        kw = dict(flags_ignored=IGN, alt=nme in ALT)
        add(nme, "distinct", [asm.cop1(nme, FD, FS, FT)], binary_cases(OPS, OPS), **kw)
        add(nme, "fd=fs", [asm.cop1(nme, FS, FS, FT)], binary_cases(SMALL, SMALL), **kw)
        add(nme, "fd=ft", [asm.cop1(nme, FT, FS, FT)], binary_cases(SMALL, SMALL), **kw)
        add(nme, "fs=ft", [asm.cop1(nme, FD, FS, FS)], [Case(f"fs=ft={a:#010x}", st(f1=a, f2=a, fcr=fcr()), cls=klass(a)) for a in OPS], **kw)
    for nme in ACCW:
        add(nme, "distinct", [asm.cop1(nme, 0, FS, FT)], binary_cases(OPS, OPS), flags_ignored=IGN)
        add(nme, "fs=ft", [asm.cop1(nme, 0, FS, FS)], [Case(f"fs=ft={a:#010x}", st(f1=a, f2=a, fcr=fcr()), cls=klass(a)) for a in OPS], flags_ignored=IGN)
    for nme in ACCR:
        add(nme, "distinct", [asm.cop1(nme, FD, FS, FT)], binary_cases(OPS, OPS, ACCS_CLAMPED, True), flags_ignored=IGN)
        add(nme, "fd=fs", [asm.cop1(nme, FS, FS, FT)], binary_cases(SMALL, SMALL, ACCS, True), flags_ignored=IGN)
    for nme in ACCRW:
        add(nme, "distinct", [asm.cop1(nme, 0, FS, FT)], binary_cases(OPS, OPS, ACCS, True), flags_ignored=IGN)
    for nme in ("sqrt.s",):
        add(nme, "distinct", [asm.cop1(nme, FD, 0, FT)], [Case(f"ft={a:#010x}", st(f2=a, fcr=fcr()), cls=klass(a)) for a in OPS],
            flags_ignored=IGN, alt=True)
        add(nme, "fd=ft", [asm.cop1(nme, FT, 0, FT)], [Case(f"ft={a:#010x}", st(f2=a, fcr=fcr()), cls=klass(a)) for a in SMALL],
            flags_ignored=IGN, alt=True)
    for nme in ("abs.s", "neg.s", "mov.s"):
        add(nme, "distinct", [asm.cop1(nme, FD, FS)], unary_cases(OPS), flags_ignored=IGN)
        add(nme, "fd=fs", [asm.cop1(nme, FS, FS)], unary_cases(SMALL), flags_ignored=IGN)
    add("cvt.w.s", "distinct", [asm.cop1("cvt.w.s", FD, FS)], unary_cases(OPS), flags_ignored=IGN)
    add("cvt.w.s", "fd=fs", [asm.cop1("cvt.w.s", FS, FS)], unary_cases(SMALL), flags_ignored=IGN)
    add("cvt.s.w", "distinct", [asm.cop1("cvt.s.w", FD, FS)], unary_cases(sorted(set(INTS + OPS))), flags_ignored=IGN)
    add("cvt.s.w", "fd=fs", [asm.cop1("cvt.s.w", FS, FS)], unary_cases(INTS), flags_ignored=IGN)
    # compares: the condition bit; the start values of FCR31 have it clear and set
    for nme in ("c.f.s", "c.eq.s", "c.lt.s", "c.le.s"):
        cs = [Case(f"fs={a:#010x} ft={b:#010x} fcr={c:#x}", st(f1=a, f2=b, fcr=c), cls="+".join(sorted({klass(a), klass(b)})))
              for a in OPS for b in OPS for c in (0, 0x800000)]
        add(nme, "distinct", [asm.cop1(nme, 0, FS, FT)], cs, flags_ignored=IGN)
    # branches: the delay slot runs when taken (and, for the non-likely ones, always); v0 counts it, v1 the fall-through
    for nme in ("bc1f", "bc1t", "bc1fl", "bc1tl"):
        add(nme, "taken or not", [asm.bc1(nme, 2), asm.i_type("addiu", V0, V0, 1), asm.i_type("addiu", V1, V1, 1)],
            [Case(f"fcr={c:#x}", st(fcr=c)) for c in FCRS + [0x7FFFFF, 0x1F800000 | 0x800000]])
    # moves
    for rt, shape in ((A1, "rt=a1"), (0, "rt=zero")):
        for fs in (FS, 0, 31):
            add("mfc1", f"{shape},fs={fs}", [asm.cop1_move("mfc1", rt, fs)],
                [Case(f"f{fs}={a:#010x}", st(**{f"f{fs}": a}), cls=klass(a)) for a in OPS])
        add("mtc1", f"{shape},fd={FD}", [asm.cop1_move("mtc1", rt, FD)],
            [Case(f"a1={g:#x}", st(gpr={A1: g})) for g in GPRS])
    add("mtc1", "rt=a1,fs=f0", [asm.cop1_move("mtc1", A1, 0)], [Case(f"a1={g:#x}", st(gpr={A1: g})) for g in GPRS])
    # cfc1: fs = 31 is the control register; the others differ between the reference's interpreter (0, and 0x2E00 for fs = 0)
    # and its recompiler (see the journal): ambiguous. Start values with bits outside the defined ones: ambiguous too.
    for fs in (31, 0, 1, 16, 30):
        add("cfc1", f"fs={fs}", [asm.cop1_move("cfc1", A1, fs)],
            [Case(f"fcr={c:#x}", st(fcr=c), amb=(fs not in (31,)) or c in JUNK) for c in FCRS + JUNK])
    add("cfc1", "rt=zero", [asm.cop1_move("cfc1", 0, 31)], [Case(f"fcr={c:#x}", st(fcr=c)) for c in FCRS])
    for fs in (31, 0, 1):
        add("ctc1", f"fs={fs}", [asm.cop1_move("ctc1", A1, fs)],
            [Case(f"a1={g:#x} fcr={c:#x}", st(gpr={A1: g}, fcr=c), amb=(fs == 31 and (g & 0xFFFFFFFF) & ~0x0083C078 != 0))
             for g in GPRS + [0x0083C078, 0x800000, 0x1FFFFFFF, 0x01000000] for c in (0, 0x800000)])
    # loads and stores of a single
    for nme in ("lwc1", "swc1"):
        for imm in (0, -8, 0x7C):
            cs = []
            for ea in (0x30, 0x34, 0x7F0, 0xFFC):
                for k, a in enumerate(OPS[:20]):
                    s = mem_start(ea, imm, n=k)
                    s[F0 + FD] = (a, 0)
                    cs.append(Case(f"ea={ea:#x} f{FD}={a:#010x}", s))
            add(nme, f"imm={imm:#x}", [asm.mem(nme, FD, A0, imm)], cs)
    return forms
