"""Reference model of the vector unit 0 instructions the main processor executes inline (COP2, "macro mode"), from the
reference emulator's sources third_party/pcsx2/pcsx2: VUops.cpp (one function per operation; the function and line are cited
below), VUflags.cpp (the MAC and status flags), VU0.cpp (QMFC2, QMTC2, CFC2, CTC2, LQC2, SQC2) and R5900OpcodeTables.cpp
(Int_COP2SPECIAL1PrintTable and Int_COP2SPECIAL2PrintTable: the function codes).

The arithmetic is fpu_ref's: exact, truncated toward zero to 24 bits, which is what the unit does and what the host does under
the game's control-register value 0xFFC0. The unit's own clamp (vuDouble, VUops.cpp:439) is fpu_ref.clamp: an exponent of 0 is a
zero of the same sign, an exponent of all ones is the largest finite number (vu0Overflow is on by default, Pcsx2Config.cpp:459).
The result of every arithmetic operation goes through VU_MAC_UPDATE (VUflags.cpp:15), which also builds the MAC flags.

State, in the pairs after the coprocessor's (families.default_regs, driver.cpp): VF0..31 at VF0, VI0..15 at VI0, then ACC, Q, P, I, R,
the status, MAC and clip flags, TPC, CMSAR0, FBRST, VPU_STAT and CMSAR1. A 128-bit register is a pair (lo, hi), lane x in the low 32
bits of lo and lane w in the high 32 bits of hi.
"""
from __future__ import annotations

import re

import fpu_ref
from fpu_ref import FMAX, M32, _add_bits, _fmax, _fmin, _mul_bits, _sub_bits, clamp, val, val_raw

VF0, VI0, ACC, Q, P, IREG, R, STAT, MAC, CLIP, TPC, CMSAR0, FBRST, VPU, CMSAR1 = (
    71, 103, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131)
M64 = (1 << 64) - 1

# Function codes. SPECIAL1 is the function field (bits 0-5); SPECIAL2 is function 0x3C-0x3F with the sa field: index = sa << 2 | function & 3.
S1 = {}
for _k, _n in enumerate(["vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini"]):
    for _j, _b in enumerate("xyzw"):
        S1[f"{_n}{_b}"] = _k * 4 + _j
for _j, _b in enumerate("xyzw"):
    S1[f"vmul{_b}"] = 0x18 + _j
S1.update(vmulq=0x1C, vmaxi=0x1D, vmuli=0x1E, vminii=0x1F, vaddq=0x20, vmaddq=0x21, vaddi=0x22, vmaddi=0x23, vsubq=0x24,
          vmsubq=0x25, vsubi=0x26, vmsubi=0x27, vadd=0x28, vmadd=0x29, vmul=0x2A, vmax=0x2B, vsub=0x2C, vmsub=0x2D,
          vopmsub=0x2E, vmini=0x2F, viadd=0x30, visub=0x31, viaddi=0x32, viand=0x34, vior=0x35, vcallms=0x38, vcallmsr=0x39)
S2_ORDER = """vaddax vadday vaddaz vaddaw vsubax vsubay vsubaz vsubaw
vmaddax vmadday vmaddaz vmaddaw vmsubax vmsubay vmsubaz vmsubaw
vitof0 vitof4 vitof12 vitof15 vftoi0 vftoi4 vftoi12 vftoi15
vmulax vmulay vmulaz vmulaw vmulaq vabs vmulai vclip
vaddaq vmaddaq vaddai vmaddai vsubaq vmsubaq vsubai vmsubai
vadda vmadda vmula - vsuba vmsuba vopmula vnop
vmove vmr32 - - vlqi vsqi vlqd vsqd
vdiv vsqrt vrsqrt vwaitq vmtir vmfir vilwr viswr
vrnext vrget vrinit vrxor""".split()
S2 = {n: i for i, n in enumerate(S2_ORDER) if n != "-"}
S1_BY_CODE = {v: k for k, v in S1.items()}


def encode(name: str, dest: int = 0xF, ft: int = 0, fs: int = 0, fd: int = 0) -> int:
    """A COP2 word of the macro set: dest is the xyzw mask (bit 3 = x), or for vdiv/vsqrt/vrsqrt the ftf (bits 3-2) and fsf (bits 1-0)."""
    base = (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11)
    if name in S1:
        return base | (fd << 6) | S1[name]
    idx = S2[name]
    return base | ((idx >> 2) << 6) | 0x3C | (idx & 3)


def encode_move(name: str, rt: int, rd: int, ni: bool = False) -> int:
    """qmfc2 / cfc2 / qmtc2 / ctc2 rt, rd (rd is the vector register or the control register); ni sets the interlock bit."""
    rs = {"qmfc2": 1, "cfc2": 2, "qmtc2": 5, "ctc2": 6}[name]
    return (0x12 << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (1 if ni else 0)


# ---- State access -------------------------------------------------------------------------------------------------------------
def lanes(m, r: int) -> list[int]:
    lo, hi = m.lo[r], m.hi[r]
    return [lo & M32, lo >> 32, hi & M32, hi >> 32]


def setlanes(m, r: int, v: list[int]) -> None:
    m.lo[r] = (v[0] & M32) | (v[1] & M32) << 32
    m.hi[r] = (v[2] & M32) | (v[3] & M32) << 32


# ---- Flags: VUflags.cpp ----------------------------------------------------------------------------------------------------------
def mac_update(m, lane: int, f: int) -> int:
    """VU_MAC_UPDATE (VUflags.cpp:15-47) for lane 0 = x .. 3 = w (shift 3 - lane); returns the bits to store."""
    shift = 3 - lane
    mac = m.lo[MAC] & M32
    s = f & 0x80000000
    exp = (f >> 23) & 0xFF
    mac = (mac | (0x10 << shift)) if s else (mac & ~(0x10 << shift))
    if f & 0x7FFFFFFF == 0:                                  # f == 0 as a float: both zeros
        m.lo[MAC] = ((mac & ~(0x1100 << shift)) | (1 << shift)) & M32
        return f
    if exp == 0:
        m.lo[MAC] = ((mac & ~(0x1000 << shift)) | (0x101 << shift)) & M32
        return s
    if exp == 255:
        m.lo[MAC] = ((mac & ~(0x101 << shift)) | (0x1000 << shift)) & M32
        return s | FMAX
    m.lo[MAC] = mac & ~(0x1101 << shift) & M32
    return f


def mac_clear(m, lane: int) -> None:
    m.lo[MAC] &= ~(0x1111 << (3 - lane)) & M32               # VU_MACx_CLEAR (:50-68)


def stat_update(m) -> None:
    mac = m.lo[MAC]                                          # VU_STAT_UPDATE (:70-79): the new flags only
    m.lo[STAT] = (1 if mac & 0x000F else 0) | (2 if mac & 0x00F0 else 0) | (4 if mac & 0x0F00 else 0) | (8 if mac & 0xF000 else 0)


# ---- Arithmetic: VUops.cpp:536-700 ---------------------------------------------------------------------------------------------
OPS = {
    "add": lambda acc, a, b: _add_bits(a, b),                                  # _vuOpADD :550
    "sub": lambda acc, a, b: _sub_bits(a, b),                                  # _vuOpSUB :605
    "mul": lambda acc, a, b: _mul_bits(a, b),                                  # _vuOpMUL :640
    "madd": lambda acc, a, b: _add_bits(acc, _mul_bits(a, b)),                 # _vuOpMADD :705
    "msub": lambda acc, a, b: _sub_bits(acc, _mul_bits(a, b)),                 # _vuOpMSUB :752
}
_FMAC = re.compile(r"v(add|sub|mul|madd|msub)(a?)([xyzwqi]?)")
_MINMAX = re.compile(r"v(max|mini)([xyzwi]?)")


def _fields(word: int):
    return (word >> 21) & 15, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31     # dest, ft, fs, fd


def _fmac(m, word: int, op: str, to_acc: bool, bc: str) -> None:
    """applyBinaryMACOp / applyTernaryMACOp and their Broadcast forms (VUops.cpp:517-564, 681-700)."""
    dest, ft, fs, fd = _fields(word)
    A, T, accv = lanes(m, VF0 + fs), lanes(m, VF0 + ft), lanes(m, ACC)
    b = None
    if bc in "xyzw" and bc:
        b = T["xyzw".index(bc)]                                                # the broadcast value is read before any lane is written
    elif bc == "q":
        b = m.lo[Q] & M32
    elif bc == "i":
        b = m.lo[IREG] & M32
    dst = lanes(m, ACC if to_acc else VF0 + fd)
    for i in range(4):
        if dest >> (3 - i) & 1:
            r = OPS[op](accv[i], A[i], T[i] if b is None else b)
            dst[i] = mac_update(m, i, r)
        else:
            mac_clear(m, i)
    stat_update(m)
    if to_acc:
        setlanes(m, ACC, dst)
    elif fd != 0:                                                              # _getDst: fd == 0 writes the dummy RDzero
        setlanes(m, VF0 + fd, dst)


def _minmax(m, word: int, which: str, bc: str) -> None:
    """applyMinMax (VUops.cpp:790-815): no flags, no clamp; fd == 0 returns before anything."""
    dest, ft, fs, fd = _fields(word)
    if fd == 0:
        return
    A, T = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
    b = m.lo[IREG] & M32 if bc == "i" else T["xyzw".index(bc)] if bc else None
    fn = _fmax if which == "max" else _fmin
    dst = lanes(m, VF0 + fd)
    for i in range(4):
        if dest >> (3 - i) & 1:
            dst[i] = fn(A[i], T[i] if b is None else b)
    setlanes(m, VF0 + fd, dst)


def _opmula(m, word: int) -> None:
    """_vuOPMULA (VUops.cpp:835-842)."""
    _, ft, fs, _ = _fields(word)
    A, T, accv = lanes(m, VF0 + fs), lanes(m, VF0 + ft), lanes(m, ACC)
    accv[0] = mac_update(m, 0, _mul_bits(A[1], T[2]))
    accv[1] = mac_update(m, 1, _mul_bits(A[2], T[0]))
    accv[2] = mac_update(m, 2, _mul_bits(A[0], T[1]))
    stat_update(m)
    setlanes(m, ACC, accv)


def _opmsub(m, word: int) -> None:
    """_vuOPMSUB (VUops.cpp:844-868)."""
    _, ft, fs, fd = _fields(word)
    A, T, accv = lanes(m, VF0 + fs), lanes(m, VF0 + ft), lanes(m, ACC)
    dst = lanes(m, VF0 + fd)
    dst[0] = mac_update(m, 0, _sub_bits(accv[0], _mul_bits(A[1], T[2])))
    dst[1] = mac_update(m, 1, _sub_bits(accv[1], _mul_bits(A[2], T[0])))
    dst[2] = mac_update(m, 2, _sub_bits(accv[2], _mul_bits(A[0], T[1])))
    stat_update(m)
    if fd != 0:
        setlanes(m, VF0 + fd, dst)


def _ftoi(x: int, off: int) -> int:
    """floatToInt<Offset> (VUops.cpp:873-884)."""
    e = (x >> 23) & 0xFF
    if e == 0:
        return 0
    if e + off >= 0x9E:                                                         # (bits & 0x7f800000) >= 0x4f000000 after the scale
        return 0x80000000 if x >> 31 else 0x7FFFFFFF
    v = val_raw(x) * (1 << off)
    return int(v) & M32


def _itof(x: int, off: int) -> int:
    """intToFloat<Offset> (VUops.cpp:891-896): s32 -> float (truncated), then scaled by 2^-Offset (exact)."""
    n = x - (1 << 32) if x >> 31 else x
    bits = fpu_ref.encode(fpu_ref.Fraction(n), 0)
    if off == 0 or bits & 0x7FFFFFFF == 0:
        return bits
    return fpu_ref.encode(val_raw(bits) / (1 << off), bits >> 31)


def _clip(m, word: int) -> None:
    """_vuCLIP (VUops.cpp:904-925)."""
    _, ft, fs, _ = _fields(word)
    A, T = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
    s = lambda x: x - (1 << 32) if x >> 31 else x                                # noqa: E731
    value = T[3]
    value = (value & 0x7FFFFFFF) if value & 0x7F800000 else 0x007FFFFF
    flag = (m.lo[CLIP] << 6) & M32
    for k, lane in enumerate(A[:3]):
        if s(lane ^ 0) > value:
            flag |= 1 << (2 * k)
        if s(lane ^ 0x80000000) > value:
            flag |= 2 << (2 * k)
    m.lo[CLIP] = flag & 0xFFFFFF


def _div(m, word: int, kind: str) -> None:
    """_vuDIV, _vuSQRT, _vuRSQRT (VUops.cpp:932-1010). The fields: ftf = bits 24-23, fsf = bits 22-21."""
    _, ft, fs, _ = _fields(word)
    ftf, fsf = (word >> 23) & 3, (word >> 21) & 3
    fb = lanes(m, VF0 + ft)[ftf]
    fa = lanes(m, VF0 + fs)[fsf]
    m.lo[STAT] &= ~0x30 & M32
    sign = (fa ^ fb) & 0x80000000
    b, a = clamp(fb), clamp(fa)
    bz = b & 0x7FFFFFFF == 0
    if kind == "div":
        if bz:
            m.lo[STAT] |= 0x10 if a & 0x7FFFFFFF == 0 else 0x20
            m.lo[Q] = 0xFF7FFFFF if sign else 0x7F7FFFFF
        else:
            m.lo[Q] = clamp(fpu_ref.encode(val(a) / val(b), a >> 31 ^ b >> 31))
    elif kind == "sqrt":
        if b >> 31 and not bz:
            m.lo[STAT] |= 0x10
        v = abs(val(b))
        m.lo[Q] = clamp(fpu_ref._sqrt_bits(v)) if v else 0
    else:
        if bz:
            m.lo[STAT] |= 0x20
            if a & 0x7FFFFFFF != 0:
                m.lo[Q] = 0xFF7FFFFF if sign else 0x7F7FFFFF
            else:
                m.lo[STAT] |= 0x10
                m.lo[Q] = 0x80000000 if sign else 0
        else:
            if b >> 31:
                m.lo[STAT] |= 0x10
            if a & 0x7FFFFFFF == 0:
                m.lo[Q] = (a >> 31) << 31 ^ 0 if True else 0
            else:
                m.lo[Q] = clamp(fpu_ref._rsqrt_bits(val(a), abs(val(b))))


# ---- Integer registers, moves, random register -----------------------------------------------------------------------------------
def _s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >> 15 else x


def _int_op(m, word: int, name: str) -> None:
    it, is_, fd = (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31             # id is the fd field
    a, b = m.lo[VI0 + is_] & 0xFFFF, m.lo[VI0 + it] & 0xFFFF
    if name == "viaddi":
        imm = fd & 0xF | (0xFFF0 if fd & 0x10 else 0)
        dst, val_ = it, a + _s16(imm)
    else:
        dst = fd
        val_ = {"viadd": a + b, "visub": a - b, "viand": a & b, "vior": a | b}[name]
    if dst:
        m.lo[VI0 + dst] = val_ & 0xFFFF


def exec_cop2(m, word: int, rd_mem=None, wr_mem=None) -> None:
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    if not word & (1 << 25):
        _move(m, word, rs, rt, rd)
        return
    funct = word & 63
    name = S2_ORDER[((word >> 6) & 31) << 2 | funct & 3] if funct >= 0x3C else S1_BY_CODE.get(funct, "?")
    dest, ft, fs, fd = _fields(word)
    if name == "vnop" or name == "vwaitq":
        return
    mm = _FMAC.fullmatch(name)
    if mm:
        _fmac(m, word, mm.group(1), bool(mm.group(2)), mm.group(3))
        return
    mm = _MINMAX.fullmatch(name)
    if mm:
        _minmax(m, word, mm.group(1), mm.group(2) if name != "vminii" else "i")
        return
    if name == "vminii":
        _minmax(m, word, "mini", "i")
    elif name == "vopmula":
        _opmula(m, word)
    elif name == "vopmsub":
        _opmsub(m, word)
    elif name == "vabs":                                                           # _vuABS via applyUnaryFunction (VUops.cpp:494-509)
        if ft:
            src, dst = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = src[i] & 0x7FFFFFFF
            setlanes(m, VF0 + ft, dst)
    elif name in ("vftoi0", "vftoi4", "vftoi12", "vftoi15", "vitof0", "vitof4", "vitof12", "vitof15"):
        off = int(name[5:] if name.startswith("vftoi") else name[5:])
        fn = _ftoi if name.startswith("vftoi") else _itof
        if ft:
            src, dst = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = fn(src[i], off)
            setlanes(m, VF0 + ft, dst)
    elif name == "vmove":                                                          # _vuMOVE (:1086)
        if ft:
            src, dst = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = src[i]
            setlanes(m, VF0 + ft, dst)
    elif name == "vmr32":                                                          # _vuMR32 (:1118)
        if ft:
            src, dst = lanes(m, VF0 + fs), lanes(m, VF0 + ft)
            rot = [src[1], src[2], src[3], src[0]]
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = rot[i]
            setlanes(m, VF0 + ft, dst)
    elif name == "vclip":
        _clip(m, word)
    elif name in ("vdiv", "vsqrt", "vrsqrt"):
        _div(m, word, name[1:])
    elif name in ("viadd", "visub", "viaddi", "viand", "vior"):
        _int_op(m, word, name)
    elif name == "vmfir":                                                          # _vuMFIR (:1097)
        if ft:
            v = _s16(m.lo[VI0 + fs]) & M32
            dst = lanes(m, VF0 + ft)
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = v
            setlanes(m, VF0 + ft, dst)
    elif name == "vmtir":                                                          # _vuMTIR (:1108): the field picked by fsf
        if ft:
            m.lo[VI0 + ft] = lanes(m, VF0 + fs)[(word >> 21) & 3] & 0xFFFF
    elif name == "vrinit":                                                         # _vuRINIT (:1311)
        m.lo[R] = 0x3F800000 | (lanes(m, VF0 + fs)[(word >> 21) & 3] & 0x7FFFFF)
    elif name == "vrxor":                                                          # _vuRXOR (:1339)
        m.lo[R] = 0x3F800000 | ((m.lo[R] ^ lanes(m, VF0 + fs)[(word >> 21) & 3]) & 0x7FFFFF)
    elif name == "vrget":                                                          # _vuRGET (:1316)
        if ft:
            dst = lanes(m, VF0 + ft)
            for i in range(4):
                if dest >> (3 - i) & 1:
                    dst[i] = m.lo[R] & M32
            setlanes(m, VF0 + ft, dst)
    else:
        raise NotImplementedError(f"COP2 {name} ({word:#010x})")


def _move(m, word: int, rs: int, rt: int, rd: int) -> None:
    """QMFC2, QMTC2, CFC2, CTC2 (VU0.cpp:116-208). rd is the vector or the control register (_Fs_)."""
    if rs == 1:                                                                    # QMFC2
        if rt:
            m.lo[rt], m.hi[rt] = m.lo[VF0 + rd], m.hi[VF0 + rd]
    elif rs == 5:                                                                  # QMTC2
        if rd:
            m.lo[VF0 + rd], m.hi[VF0 + rd] = m.lo[rt], m.hi[rt]
    elif rs == 2:                                                                  # CFC2
        if not rt:
            return
        if rd == 20:
            m.lo[rt] = (m.lo[rt] & ~M32 & M64) | (m.lo[R] & 0x7FFFFF)               # only UL[0] is written
        else:
            v = control_read(m, rd)
            m.lo[rt] = v | (0xFFFFFFFF00000000 if v >> 31 else 0)
    elif rs == 6:                                                                  # CTC2
        if rd == 0:
            return
        v = m.lo[rt] & M32
        if rd in (17, 26, 29):                                                     # read-only
            return
        if rd == 20:
            m.lo[R] = (v & 0x7FFFFF) | 0x3F800000
        elif rd == 28:
            m.lo[FBRST] = v & 0x0C0C
        else:
            control_write(m, rd, v)
    else:
        raise NotImplementedError(f"COP2 move {rs}")


def control_register(rd: int) -> int:
    """The index in the state of control register rd (0-15 are the integer registers)."""
    return {16: STAT, 17: MAC, 18: CLIP, 20: R, 21: IREG, 22: Q, 26: TPC, 27: CMSAR0, 28: FBRST, 29: VPU, 31: CMSAR1}.get(rd, -1) if rd >= 16 else VI0 + rd


def control_read(m, rd: int) -> int:
    return m.lo[control_register(rd)] & M32


def control_write(m, rd: int, v: int) -> None:
    m.lo[control_register(rd)] = v
