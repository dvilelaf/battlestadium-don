"""Family G: the 128-bit multimedia set (opcode 0x1C: MMI0..MMI3, PMFHL, PMTHL, PLZCW, the parallel shifts) and the
shift-amount register (MFSA, MTSA, MTSAB, MTSAH) with QFSRV. The reference is mmi_ref.py (MMI.cpp).

Operands are 128-bit: lanes with the sign bit set and clear, the saturation boundaries of each lane width, zero,
all ones, and vectors in which every lane is different (a lane moved to the wrong place shows).
"""
from __future__ import annotations

import asm
from asm import A0, A1, A2, JR_RA, NOP, V0
import mmi_ref

M128 = (1 << 128) - 1
BOUND = {8: [0, 1, 0x7F, 0x80, 0xFF, 0x81, 0xFE], 16: [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FFE, 0x8001],
         32: [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFE, 0x80000001]}


def _fill(w: int, v: int) -> int:
    return mmi_ref.pack([v] * (128 // w), w)


def _mix(w: int, shift: int) -> int:
    n = 128 // w
    return mmi_ref.pack([BOUND[w][(i + shift) % len(BOUND[w])] for i in range(n)], w)


NAMED = {                                    # name -> 128-bit value; the names are only for the failure messages
    "zero": 0, "ones": M128,
    **{f"b{w}_{v:x}": _fill(w, v) for w in (8, 16, 32) for v in BOUND[w][2:5]},
    "mix8a": _mix(8, 0), "mix8b": _mix(8, 3), "mix16a": _mix(16, 0), "mix16b": _mix(16, 4),
    "mix32a": _mix(32, 0), "mix32b": _mix(32, 3), "mix32c": _mix(32, 5),
    "seq": 0x0F0E0D0C0B0A09080706050403020100,
    "sign": 0x8F7E6D5C4B3A29181F2E3D4C5B6A7988,
    "h": 0x0008_0007_0006_0005_0004_0003_0002_0001,
    "w": 0x00000004_00000003_00000002_00000001,
    "shamt": 0xFFFFFFE1_00000001_00000020_0000001F,           # shift counts for the variable shifts: 31, 32, 1, 33 (&31)
    "shamt2": 0x0000003F_00000005_00000041_00000000,
    "pm": 0x7FFF8000_0001FFFF_8000_7FFF_0002_FFFE,
}
NAMES = list(NAMED)
V = [NAMED[n] for n in NAMES]
SMALL = ["zero", "ones", "b8_80", "b16_8000", "b32_80000000", "mix8a", "mix16a", "mix32a", "mix32b", "seq", "sign", "shamt"]
R = [NAMED[n] for n in SMALL]
HILO = [                                     # (HI, LO) 128-bit start states of the multiplies and the result moves
    (0x0123456789ABCDEF_0F1E2D3C4B5A6978, 0xFEDCBA9876543210_8796A5B4C3D2E1F0),
    (0, 0),
    (_mix(32, 0), _mix(32, 3)),
    (_fill(32, 0x7FFFFFFF), _fill(32, 0x80000000)),
]


def _split(v: int) -> tuple[int, int]:
    return v & mmi_ref.M64, v >> 64


def mstate(base, a0=None, a1=None, a2=None, hilo=None, sa=None):
    regs = list(base)
    if a0 is not None:
        regs[A0] = _split(a0)
    if a1 is not None:
        regs[A1] = _split(a1)
    if a2 is not None:
        regs[A2] = (a2, 0)
    if hilo is not None:
        hi, lo = hilo
        regs[32], regs[34] = (hi & mmi_ref.M64, 0), (hi >> 64, 0)
        regs[33], regs[35] = (lo & mmi_ref.M64, 0), (lo >> 64, 0)
    if sa is not None:
        regs[36] = (sa, 0)
    return regs


def family_mmi(prog, Form, Case, default_regs, hx):
    base = default_regs()
    forms = []

    def add(name, variant, words, cases):
        f = Form(name, variant, prog.add(f"{name.replace('.', '_')}_{len(prog.funcs)}", words + [JR_RA, NOP]))
        f.cases = cases
        forms.append(f)

    def pairs(vals_a, vals_b, **kw):
        return [Case(f"a0={hx(a & mmi_ref.M64)}:{hx(a >> 64)} a1={hx(b & mmi_ref.M64)}:{hx(b >> 64)}",
                     mstate(base, a0=a, a1=b, **kw)) for a in vals_a for b in vals_b]

    def singles(vals, **kw):
        return [Case(f"a1={hx(b & mmi_ref.M64)}:{hx(b >> 64)}", mstate(base, a1=b, **kw)) for b in vals]

    BINARY = ["paddw", "psubw", "pcgtw", "pmaxw", "paddh", "psubh", "pcgth", "pmaxh", "paddb", "psubb", "pcgtb",
              "paddsw", "psubsw", "pextlw", "ppacw", "paddsh", "psubsh", "pextlh", "ppach", "paddsb", "psubsb",
              "pextlb", "ppacb", "pceqw", "pminw", "padsbh", "pceqh", "pminh", "pceqb", "padduw", "psubuw",
              "pextuw", "padduh", "psubuh", "paddub", "psubub", "pextub", "psllvw", "psrlvw", "psravw", "pinth",
              "pcpyld", "pand", "pxor", "por", "pnor", "pinteh", "pcpyud"]
    UNARY = ["pext5", "ppac5", "pabsw", "pabsh", "pexeh", "prevh", "pexew", "prot3w", "pexch", "pcpyh", "pexcw"]
    for n in BINARY:
        add(n, "distinct", [asm.mmx(n, V0, A0, A1)], pairs(V, V))
        add(n, "rd=rs", [asm.mmx(n, A0, A0, A1)], pairs(R, R))
        add(n, "rd=rt", [asm.mmx(n, A1, A0, A1)], pairs(R, R))
        add(n, "rs=rt", [asm.mmx(n, V0, A1, A1)], singles(V))
        add(n, "rd=zero", [asm.mmx(n, 0, A0, A1)], pairs(R, R))
    for n in UNARY:
        add(n, "distinct", [asm.mmx(n, V0, 0, A1)], singles(V))
        add(n, "rd=rt", [asm.mmx(n, A1, 0, A1)], singles(R))
        add(n, "rd=zero", [asm.mmx(n, 0, 0, A1)], singles(R))
    # the immediate shifts: every shift amount
    for n, count in (("psllh", 16), ("psrlh", 16), ("psrah", 16), ("psllw", 32), ("psrlw", 32), ("psraw", 32)):
        for sa in range(count):
            add(n, f"sa={sa}", [asm.mmx(n, V0, 0, A1, sa)], singles(V))
        add(n, "rd=rt,sa=3", [asm.mmx(n, A1, 0, A1, 3)], singles(R))
        add(n, "rd=zero,sa=3", [asm.mmx(n, 0, 0, A1, 3)], singles(R))
    # the multiplies, accumulating into HI/LO (128-bit), with and without the destination register
    for n in ["pmaddw", "pmsubw", "pmultw", "pmultuw", "pmadduw", "pmaddh", "pmsubh", "pmulth", "phmadh", "phmsbh"]:
        cases = pairs(V, V, hilo=HILO[0])
        for hl in HILO[1:]:
            cases += pairs(R, R, hilo=hl)
        add(n, "distinct", [asm.mmx(n, V0, A0, A1)], cases)
        add(n, "rd=zero", [asm.mmx(n, 0, A0, A1)], pairs(R, R, hilo=HILO[0]) + pairs(R, R, hilo=HILO[3]))
    for n in ["pdivw", "pdivuw", "pdivbw"]:
        add(n, "distinct", [asm.mmx(n, 0, A0, A1)], pairs(V, V, hilo=HILO[0]))
    # the result registers
    states = [Case(f"hi={hx(h & mmi_ref.M64)}:{hx(h >> 64)} lo={hx(l & mmi_ref.M64)}:{hx(l >> 64)}",
                   mstate(base, hilo=(h, l))) for h in V for l in R]
    for n in ["pmfhi", "pmflo", "pmfhl.lw", "pmfhl.uw", "pmfhl.slw", "pmfhl.lh", "pmfhl.sh"]:
        add(n, "distinct", [asm.mmx(n, V0)], states)
        add(n, "rd=zero", [asm.mmx(n, 0)], states[:40])
    for n in ["pmthi", "pmtlo"]:
        add(n, "distinct", [asm.mmx(n, 0, A0)], [Case(f"a0={hx(v & mmi_ref.M64)}:{hx(v >> 64)}", mstate(base, a0=v)) for v in V])
    add("pmthl.lw", "distinct", [asm.mmx("pmthl.lw", 0, A0)],
        [Case(f"a0={hx(v & mmi_ref.M64)}:{hx(v >> 64)} hilo", mstate(base, a0=v, hilo=hl)) for v in V for hl in HILO])
    # plzcw: both words, and every position of the first sign change
    lz = V + [(0x00000001 << k) | (((~(1 << k)) & 0xFFFFFFFF) << 32) | (NAMED["seq"] << 64) for k in range(32)]
    add("plzcw", "distinct", [asm.mmx("plzcw", V0, A0)], [Case(f"a0={hx(v & mmi_ref.M64)}:{hx(v >> 64)}", mstate(base, a0=v)) for v in lz])
    add("plzcw", "rd=rs", [asm.mmx("plzcw", A0, A0)], [Case(f"a0={hx(v & mmi_ref.M64)}:{hx(v >> 64)}", mstate(base, a0=v)) for v in lz])
    add("plzcw", "rd=zero", [asm.mmx("plzcw", 0, A0)], [Case(f"a0={hx(v & mmi_ref.M64)}:{hx(v >> 64)}", mstate(base, a0=v)) for v in R])
    # the shift-amount register (in bytes in the reference: 0..15); qfsrv with every amount
    for sa in range(16):
        add("qfsrv", f"sa={sa}", [asm.mmx("qfsrv", V0, A0, A1)], pairs(R, R, sa=sa))
    add("qfsrv", "rd=zero,sa=5", [asm.mmx("qfsrv", 0, A0, A1)], pairs(R, R, sa=5))
    add("mfsa", "distinct", [asm.sa_op("mfsa", rd=V0)], [Case(f"sa={s}", mstate(base, sa=s)) for s in range(16)])
    add("mfsa", "rd=zero", [asm.sa_op("mfsa", rd=0)], [Case(f"sa={s}", mstate(base, sa=s)) for s in range(4)])
    sa_vals = [0, 1, 7, 8, 15, 16, 0x7F, 0x80, 0x100, 0xFFFFFFFF]
    add("mtsa", "then mfsa", [asm.sa_op("mtsa", rs=A2), asm.sa_op("mfsa", rd=V0)],
        [Case(f"a2={v:#x}", mstate(base, a2=v)) for v in sa_vals])
    add("mtsa", "then qfsrv", [asm.sa_op("mtsa", rs=A2), asm.mmx("qfsrv", V0, A0, A1)],
        [c for v in range(16) for c in [Case(f"a2={v}", mstate(base, a0=a, a1=b, a2=v)) for a in R[:5] for b in R[5:9]]])
    for n in ("mtsab", "mtsah"):
        for imm in (0, 1, 5, 7, 8, 15, 16, -1):
            add(n, f"imm={imm},then mfsa", [asm.sa_op(n, rs=A2, imm=imm), asm.sa_op("mfsa", rd=V0)],
                [Case(f"a2={v:#x}", mstate(base, a2=v)) for v in (0, 1, 3, 5, 7, 8, 9, 15, 16, 0xFFFFFFF9)])
        add(n, "imm=5,then qfsrv", [asm.sa_op(n, rs=A2, imm=5), asm.mmx("qfsrv", V0, A0, A1)],
            [Case(f"a2={v:#x}", mstate(base, a0=a, a1=b, a2=v)) for v in range(16) for a in R[:4] for b in R[5:8]])
    return forms
