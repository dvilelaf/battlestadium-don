"""Family I: the vector unit 0 instructions the main processor runs inline (COP2, "macro mode"): the arithmetic (add, sub, mul,
madd, msub and their accumulator and broadcast forms), min/max, abs, move, the outer product, the square root, the moves between
the two processors (qmfc2, qmtc2, cfc2, ctc2) and the quadword loads and stores (lqc2, sqc2). The reference is vu_ref.py.

Not run, on purpose: microprograms (vcallms, vcallmsr, ctc2 to CMSAR1), and vlqi/vsqi/vlqd/vsqd, which act on the unit's data
memory (not part of the harness state). The MAC, status and clip flags are compared apart: the game reads them at one site each
(a context save) with no consumer, so a difference only in them is counted as "flags nobody reads" (Form.ignore).

Operand classes are fpu_ref's (family_fpu.OPS): all four lanes of every register differ.
"""
from __future__ import annotations

import itertools

import asm
import vu_ref
from asm import A0, A1, JR_RA, NOP
from family_fpu import ACCS_CLAMPED, GPRS, OPS, SMALL, klass
from vu_ref import ACC, CLIP, CMSAR0, CMSAR1, FBRST, IREG, MAC, Q, STAT, TPC, VF0, VI0, VPU

FS, FT, FD = 1, 2, 3
FLAGS = (STAT, MAC, CLIP)
# the masks the game uses for an operation (census over the executable); everything else it uses only with xyzw
GAME_MASKS = {"vadd": [0xE, 0x1], "vmsubw": [0x3, 0xC], "vabs": [0xC], "vmini": [0x3], "vmax": [0x3], "vaddi": [0x8, 0x4, 0x2]}
CENSUS = set("""vadd vsub vmul vmulax vmulz vmsubw vmulx vmuly vmaddw vaddax vadday vmadda vmaddax vmaddz vmadday vmulay vmulw vmaddy
vmaddaz vmadd vabs vaddaz vmini vmax vmula vaddi vmulaw vmuli vmsubz vmulaz vmr32 vmove vaddy vaddz vaddw vmaddx vmaxx vminii
vopmula vopmsub""".split())
MASKS = [0xE, 0x8, 0x4, 0x2, 0x1, 0x3, 0xC, 0xA, 0x5, 0x7]


def cases_pairs(vals):
    """A bijection of the n*n (a, b) pairs onto n*n/4 cases of four lanes."""
    n = len(vals)
    for c in range((n * n + 3) // 4):
        a, b = [], []
        for l in range(4):
            p = (4 * c + l) % (n * n)
            a.append(vals[p % n])
            b.append(vals[(p // n + p % n * 7) % n])
        yield c, a, b


def cases_bc(vals):
    """One broadcast value per case, four different fs lanes; every (fs, b) pair is met."""
    n = len(vals)
    for c in range(13 * n):
        yield c, [vals[(c // n * 4 + l) % n] for l in range(4)], vals[c % n]


def family_vu0(prog, Form, Case, default_regs, hx, mem_start):
    base = default_regs()
    forms = []

    def st(vf=None, acc=None, gpr=None, **named):
        regs = list(base)
        for r, ls in (vf or {}).items():
            regs[VF0 + r] = (ls[0] | ls[1] << 32, ls[2] | ls[3] << 32)
        if acc is not None:
            regs[ACC] = (acc[0] | acc[1] << 32, acc[2] | acc[3] << 32)
        for r, v in (gpr or {}).items():
            regs[r] = (v & 0xFFFFFFFFFFFFFFFF, regs[r][1])
        for k, v in named.items():
            regs[{"q": Q, "i": IREG}.get(k, k if isinstance(k, int) else 0)] = (v, 0)
        return regs

    def add(name, variant, words, cases, **kw):
        f = Form(name, variant, prog.add(f"{name}_{len(prog.funcs)}", words + [JR_RA, NOP]), **kw)
        f.cases = cases
        forms.append(f)

    def cls(*lanes):
        return "+".join(sorted({klass(x) for ls in lanes for x in (ls if isinstance(ls, list) else [ls])}))

    def desc(**kw):
        return " ".join(f"{k}=" + (",".join(f"{x:08x}" for x in v) if isinstance(v, list) else f"{v:08x}") for k, v in kw.items())

    # ---- arithmetic -----------------------------------------------------------------------------------------------
    def fmac_cases(name, vals, fd, fs, ft, bc, ternary):
        out = []
        gen = cases_bc(vals) if bc else cases_pairs(vals)
        for c, a, b in gen:
            acc = [ACCS_CLAMPED[(c + 3 * l) % len(ACCS_CLAMPED)] for l in range(4)] if ternary else None
            if bc in ("x", "y", "z", "w"):
                tl = [0x3F000000 + 0x111 * (k + 1) for k in range(4)]
                tl["xyzw".index(bc)] = b
                regs = {ft: tl} if ft != fs else {}
                fsl = a
                if ft == fs:
                    fsl = list(tl)             # fs = ft: the register is both
                regs[fs] = fsl
                if ft != fs:
                    regs[ft] = tl
                s = st(vf=regs, acc=acc)
                d = desc(fs=fsl, **{"b": b})
            elif bc in ("q", "i"):
                s = st(vf={fs: a}, acc=acc, **{bc: b})
                d = desc(fs=a, b=b)
            else:
                regs = {fs: a, ft: b}
                if ft == fs:
                    regs = {fs: a}
                s = st(vf=regs, acc=acc)
                d = desc(fs=a, ft=a if ft == fs else b)
            if acc:
                d += " " + desc(acc=acc)
            out.append(Case(d, s, cls=cls(a, b if isinstance(b, list) else [b], acc or [])))
        return out

    ops = []
    for mn in ("add", "sub", "mul", "madd", "msub"):
        for ac in ("", "a"):
            for bc in ("", "x", "y", "z", "w", "q", "i"):
                n = f"v{mn}{ac}{bc}"
                if n in vu_ref.S1 or n in vu_ref.S2:
                    ops.append((n, mn, ac == "a", bc))
    for n, mn, to_acc, bc in ops:
        ternary = mn in ("madd", "msub")
        census = n in CENSUS
        full = OPS if census else SMALL
        masks = ([0xF] + GAME_MASKS.get(n, []) + (MASKS if census else [0xA]))
        for mask in dict.fromkeys(masks):
            vals = full if mask == 0xF else SMALL
            if to_acc:
                add(n, f"mask={mask:#x}", [vu_ref.encode(n, mask, FT, FS)], fmac_cases(n, vals, 0, FS, FT, bc, ternary), ignore=FLAGS)
            else:
                add(n, f"mask={mask:#x} distinct", [vu_ref.encode(n, mask, FT, FS, FD)], fmac_cases(n, vals, FD, FS, FT, bc, ternary), ignore=FLAGS)
        if to_acc:
            add(n, "fs=ft", [vu_ref.encode(n, 0xF, FS, FS)], fmac_cases(n, SMALL, 0, FS, FS, bc, ternary), ignore=FLAGS)
        else:
            add(n, "fd=fs", [vu_ref.encode(n, 0xF, FT, FS, FS)], fmac_cases(n, SMALL, FS, FS, FT, bc, ternary), ignore=FLAGS)
            add(n, "fd=ft", [vu_ref.encode(n, 0xF, FT, FS, FT)], fmac_cases(n, SMALL, FT, FS, FT, bc, ternary), ignore=FLAGS)
            add(n, "fs=ft", [vu_ref.encode(n, 0xF, FS, FS, FD)], fmac_cases(n, SMALL, FD, FS, FS, bc, ternary), ignore=FLAGS)
            add(n, "fd=vf0", [vu_ref.encode(n, 0xF, FT, FS, 0)], fmac_cases(n, SMALL, 0, FS, FT, bc, ternary), ignore=FLAGS)

    # ---- min, max -------------------------------------------------------------------------------------------------
    for n, bc in (("vmax", ""), ("vmaxx", "x"), ("vmaxy", "y"), ("vmaxz", "z"), ("vmaxw", "w"), ("vmaxi", "i"),
                  ("vmini", ""), ("vminix", "x"), ("vminiy", "y"), ("vminiz", "z"), ("vminiw", "w"), ("vminii", "i")):
        n = {"vminix": "vminix", "vmini": "vmini"}.get(n, n)
        if n not in vu_ref.S1:
            continue
        census = n in CENSUS or n in ("vmax", "vmini")
        for mask in dict.fromkeys([0xF] + GAME_MASKS.get(n, []) + ([0xA] if census else [])):
            vals = OPS if (mask == 0xF and census) else SMALL
            add(n, f"mask={mask:#x} distinct", [vu_ref.encode(n, mask, FT, FS, FD)], fmac_cases(n, vals, FD, FS, FT, bc, False))
        add(n, "fd=fs", [vu_ref.encode(n, 0xF, FT, FS, FS)], fmac_cases(n, SMALL, FS, FS, FT, bc, False))
        add(n, "fd=vf0", [vu_ref.encode(n, 0xF, FT, FS, 0)], fmac_cases(n, SMALL, 0, FS, FT, bc, False))

    # ---- outer product ----------------------------------------------------------------------------------------------
    for n in ("vopmula", "vopmsub"):
        shapes = [("distinct", FD)] + ([("fd=fs", FS), ("fd=vf0", 0)] if n == "vopmsub" else [])
        for variant, fd in shapes:
            cs = []
            for c, a, b in cases_pairs(OPS if variant == "distinct" else SMALL):
                acc = [ACCS_CLAMPED[(c + 3 * l) % len(ACCS_CLAMPED)] for l in range(4)]
                cs.append(Case(desc(fs=a, ft=b, acc=acc), st(vf={FS: a, FT: b}, acc=acc), cls=cls(a, b, acc)))
            add(n, variant, [vu_ref.encode(n, 0xE, FT, FS, fd)], cs, ignore=FLAGS)

    # ---- unary ------------------------------------------------------------------------------------------------------
    def unary_cases(vals, ft):
        out = []
        for c, a, b in cases_pairs(vals):
            out.append(Case(desc(fs=a), st(vf={FS: a} if ft != FS else {FS: a}), cls=cls(a)))
        return out

    for n in ("vabs", "vmove", "vmr32"):
        census = n in CENSUS
        for mask in dict.fromkeys([0xF] + GAME_MASKS.get(n, []) + ([0xE, 0x1, 0x8, 0x4, 0x2, 0xA] if census else [0xA])):
            add(n, f"mask={mask:#x} distinct", [vu_ref.encode(n, mask, FT, FS)], unary_cases(OPS if mask == 0xF else SMALL, FT))
        add(n, "ft=fs", [vu_ref.encode(n, 0xF, FS, FS)], unary_cases(SMALL, FS))
        add(n, "ft=vf0", [vu_ref.encode(n, 0xF, 0, FS)], unary_cases(SMALL, 0))
    for off in (0, 4, 12, 15):
        for n in (f"vftoi{off}", f"vitof{off}"):
            add(n, "mask=0xf", [vu_ref.encode(n, 0xF, FT, FS)], unary_cases(sorted(set(OPS)), FT))
            add(n, "mask=0xa", [vu_ref.encode(n, 0xA, FT, FS)], unary_cases(SMALL, FT))
    add("vnop", "none", [vu_ref.encode("vnop")], [Case("start state", st())])

    # ---- square root, division -------------------------------------------------------------------------------------
    def div_cases(vals, kind):
        out = []
        for ftf, fsf in itertools.product(range(4), range(4)):
            for c, a, b in cases_pairs(vals):
                if (c + ftf * 4 + fsf) % 5:
                    continue
                out.append(Case(desc(fs=a, ft=b) + f" ftf={ftf} fsf={fsf}", st(vf={FS: a, FT: b}), cls=cls(b[ftf], a[fsf])))
        return out

    for n in ("vsqrt", "vdiv", "vrsqrt"):
        # one form per (ftf, fsf): the field is part of the word
        for ftf, fsf in ((0, 0), (1, 2), (3, 3)):
            cs = []
            for c, a, b in cases_pairs(SMALL if n != "vsqrt" else OPS):
                if n != "vsqrt" and c % 2:
                    continue
                cs.append(Case(desc(fs=a, ft=b), st(vf={FS: a, FT: b}), cls=cls(b[ftf]) if n == "vsqrt" else cls(b[ftf], a[fsf])))
            add(n, f"ftf={ftf} fsf={fsf}", [vu_ref.encode(n, ftf << 2 | fsf, FT, FS)], cs, ignore=FLAGS)

    # ---- the moves between the processors -------------------------------------------------------------------------------
    vfv = lambda c: [OPS[(c * 4 + l) % len(OPS)] for l in range(4)]               # noqa: E731
    for rt, shape in ((A1, "rt=a1"), (0, "rt=zero")):
        for rd in (FS, 0, 31):
            add("qmfc2.ni", f"{shape},vf={rd}", [vu_ref.encode_move("qmfc2", rt, rd)],
                [Case(desc(vf=vfv(c)), st(vf={rd: vfv(c)} if rd else {}), cls=cls(vfv(c))) for c in range(14)])
            add("qmtc2.ni", f"{shape},vf={rd}", [vu_ref.encode_move("qmtc2", rt, rd)],
                [Case(f"a1={hx(g)}", st(gpr={A1: g})) for g in GPRS] +
                [Case(f"a1={hx(g)} hi", [(g, 0x0123456789ABCDEF) if i == A1 else r for i, r in enumerate(st())]) for g in GPRS[:3]])
    # control registers: (index in the state, bits the console's register has, values)
    LEGAL = {16: 0xFFF, 17: 0xFFFF, 18: 0xFFFFFF, 20: 0x7FFFFF, 21: M32, 22: M32, 26: 0xFFF, 27: 0xFFFF, 28: 0x0C0C, 29: 0x1}
    VALS = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x12345, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x3F800000, 0xBF800000, 0x00FFFFFF,
            0x555, 0xAAA, 0x0C0C, 0x0303, 0x0004]
    for rd in list(range(16)) + [16, 17, 18, 19, 20, 21, 22, 26, 27, 28, 29]:
        if rd == 19:
            continue                                   # no register there (the reference's own bits: ambiguous), no site
        reg = vu_ref.control_register(rd)
        legal = 0xFFFF if rd < 16 else LEGAL[rd]
        for rt, shape in ((A1, "rt=a1"), (0, "rt=zero")):
            if rd not in (0, 1, 5, 16, 17, 18, 20, 21, 22, 26, 28, 29) and shape == "rt=zero":
                continue
            add("cfc2.ni", f"{shape},cr={rd}", [vu_ref.encode_move("cfc2", rt, rd)],
                [Case(f"cr={v:#x}", [(v, 0) if i == reg else r for i, r in enumerate(st())], amb=bool(v & ~legal) or rd == 26 or (rd == 20 and v >> 23 & 0x1FF != 0x7F))   # TPC: the port keeps the address in bytes of the program counter (>> 3), no program runs
                 for v in VALS] if rd else [Case("cr0", st())])
            if rd in (26, 29, 17, 20, 21, 22, 28, 16, 18, 0, 1, 15, 27):
                add("ctc2.ni", f"{shape},cr={rd}", [vu_ref.encode_move("ctc2", rt, rd)],
                    [Case(f"a1={hx(g)}", st(gpr={A1: g}), amb=(bool((g & 0xFFFFFFFF) & ~legal) and rd not in (17, 26, 29)) or rd == 20)   # R: the stored form differs, only vrget/vrnext/vrxor could see it (no site)
                     for g in GPRS + [0x3F800000, 0xFFFF, 0x7FFFFF, 0x0C0C, 0xFFFFFF, 0xFFF, 0x8000]])

    # ---- loads and stores -----------------------------------------------------------------------------------------
    for nme, opc in (("lqc2", 0x36), ("sqc2", 0x3E)):
        for ft in (FT, 0):
            for imm in (0, -16, 0x70):
                cs = []
                for ea in (0x30, 0x40, 0x7F0, 0xFF0):
                    for k in range(10):
                        s = mem_start(ea, imm, n=k)
                        v = vfv(k + ea // 16)
                        s[VF0 + ft] = (v[0] | v[1] << 32, v[2] | v[3] << 32) if ft else s[VF0]
                        cs.append(Case(f"ea={ea:#x} vf{ft}={','.join(f'{x:08x}' for x in v)}", s))
                for ea in (0x34, 0x3F):                                    # unaligned: the reference reads and writes the quadword, the hardware faults
                    s = mem_start(ea, imm)
                    cs.append(Case(f"unaligned ea={ea:#x}", s, amb=True))
                add(nme, f"vf={ft},imm={imm:#x}", [(opc << 26) | (A0 << 21) | (ft << 16) | (imm & 0xFFFF)], cs)
    return forms


M32 = 0xFFFFFFFF
