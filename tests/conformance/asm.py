"""A tiny MIPS (R5900 integer subset) assembler: just enough to build the guest functions of the tests."""
from __future__ import annotations

# Register numbers used by the test programs.
ZERO, V0, V1, A0, A1, A2, T0, T1, T2, RA = 0, 2, 3, 4, 5, 6, 8, 9, 10, 31

SPECIAL = {
    "sll": 0x00, "srl": 0x02, "sra": 0x03, "sllv": 0x04, "srlv": 0x06, "srav": 0x07,
    "jr": 0x08, "movz": 0x0A, "movn": 0x0B, "nor": 0x27, "slt": 0x2A, "sltu": 0x2B,
    "dsllv": 0x14, "dsrlv": 0x16, "dsrav": 0x17,
    "mfhi": 0x10, "mthi": 0x11, "mflo": 0x12, "mtlo": 0x13, "mult": 0x18, "multu": 0x19, "div": 0x1A, "divu": 0x1B,
    "add": 0x20, "addu": 0x21, "sub": 0x22, "subu": 0x23, "and": 0x24, "or": 0x25, "xor": 0x26,
    "dadd": 0x2C, "daddu": 0x2D, "dsub": 0x2E, "dsubu": 0x2F,
    "dsll": 0x38, "dsrl": 0x3A, "dsra": 0x3B, "dsll32": 0x3C, "dsrl32": 0x3E, "dsra32": 0x3F,
}
ITYPE = {
    "beq": 0x04, "bne": 0x05, "blez": 0x06, "bgtz": 0x07, "addiu": 0x09, "slti": 0x0A, "sltiu": 0x0B,
    "addi": 0x08, "andi": 0x0C, "ori": 0x0D, "xori": 0x0E, "lui": 0x0F, "daddi": 0x18, "daddiu": 0x19,
    "beql": 0x14, "bnel": 0x15, "blezl": 0x16, "bgtzl": 0x17,
}
REGIMM = {  # opcode 1, the branch kind is in the rt field
    "bltz": 0x00, "bgez": 0x01, "bltzl": 0x02, "bgezl": 0x03,
    "bltzal": 0x10, "bgezal": 0x11, "bltzall": 0x12, "bgezall": 0x13,
}


def r_type(name: str, rd: int = 0, rs: int = 0, rt: int = 0, sa: int = 0) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | SPECIAL[name]


def i_type(name: str, rs: int, rt: int, imm: int) -> int:
    return (ITYPE[name] << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def regimm(name: str, rs: int, offset_words: int) -> int:
    return (1 << 26) | (rs << 21) | (REGIMM[name] << 16) | (offset_words & 0xFFFF)


def branch(name: str, rs: int, rt: int, offset_words: int) -> int:
    """A conditional branch; offset_words is counted from the delay slot (target = pc + 4 + 4*offset)."""
    if name in REGIMM:
        return regimm(name, rs, offset_words)
    return i_type(name, rs, rt, offset_words)


NOP = 0
JR_RA = r_type("jr", rs=RA)

# MMI (opcode 0x1C): the R5900's own multiply/divide forms, the function field says which.
MMI = {
    "madd": 0x00, "maddu": 0x01, "mfhi1": 0x10, "mthi1": 0x11, "mflo1": 0x12, "mtlo1": 0x13,
    "mult1": 0x18, "multu1": 0x19, "div1": 0x1A, "divu1": 0x1B, "madd1": 0x20, "maddu1": 0x21,
}


def mmi(name: str, rd: int = 0, rs: int = 0, rt: int = 0) -> int:
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | MMI[name]

# Loads and stores (I-type: rs = base register, rt = data register).
MEMOPS = {
    "lb": 0x20, "lh": 0x21, "lwl": 0x22, "lw": 0x23, "lbu": 0x24, "lhu": 0x25, "lwr": 0x26, "lwu": 0x27,
    "sb": 0x28, "sh": 0x29, "swl": 0x2A, "sw": 0x2B, "sdl": 0x2C, "sdr": 0x2D, "swr": 0x2E,
    "ldl": 0x1A, "ldr": 0x1B, "lq": 0x1E, "sq": 0x1F, "ld": 0x37, "sd": 0x3F, "lwc1": 0x31, "swc1": 0x39,
}


def mem(name: str, rt: int, base: int, offset: int) -> int:
    return (MEMOPS[name] << 26) | (base << 21) | (rt << 16) | (offset & 0xFFFF)


# The multimedia set (opcode 0x1C): name -> (function field, sa field). MMI0/MMI1/MMI2/MMI3 are the function fields
# 0x08/0x28/0x09/0x29, and the sa field picks the instruction inside them.
MMI_OPS = {
    "plzcw": (0x04, 0),
    "paddw": (0x08, 0x00), "psubw": (0x08, 0x01), "pcgtw": (0x08, 0x02), "pmaxw": (0x08, 0x03),
    "paddh": (0x08, 0x04), "psubh": (0x08, 0x05), "pcgth": (0x08, 0x06), "pmaxh": (0x08, 0x07),
    "paddb": (0x08, 0x08), "psubb": (0x08, 0x09), "pcgtb": (0x08, 0x0A),
    "paddsw": (0x08, 0x10), "psubsw": (0x08, 0x11), "pextlw": (0x08, 0x12), "ppacw": (0x08, 0x13),
    "paddsh": (0x08, 0x14), "psubsh": (0x08, 0x15), "pextlh": (0x08, 0x16), "ppach": (0x08, 0x17),
    "paddsb": (0x08, 0x18), "psubsb": (0x08, 0x19), "pextlb": (0x08, 0x1A), "ppacb": (0x08, 0x1B),
    "pext5": (0x08, 0x1E), "ppac5": (0x08, 0x1F),
    "pabsw": (0x28, 0x01), "pceqw": (0x28, 0x02), "pminw": (0x28, 0x03), "padsbh": (0x28, 0x04),
    "pabsh": (0x28, 0x05), "pceqh": (0x28, 0x06), "pminh": (0x28, 0x07), "pceqb": (0x28, 0x0A),
    "padduw": (0x28, 0x10), "psubuw": (0x28, 0x11), "pextuw": (0x28, 0x12),
    "padduh": (0x28, 0x14), "psubuh": (0x28, 0x15), "pextuh": (0x28, 0x16),
    "paddub": (0x28, 0x18), "psubub": (0x28, 0x19), "pextub": (0x28, 0x1A), "qfsrv": (0x28, 0x1B),
    "pmaddw": (0x09, 0x00), "psllvw": (0x09, 0x02), "psrlvw": (0x09, 0x03), "pmsubw": (0x09, 0x04),
    "pmfhi": (0x09, 0x08), "pmflo": (0x09, 0x09), "pinth": (0x09, 0x0A), "pmultw": (0x09, 0x0C),
    "pdivw": (0x09, 0x0D), "pcpyld": (0x09, 0x0E), "pmaddh": (0x09, 0x10), "phmadh": (0x09, 0x11),
    "pand": (0x09, 0x12), "pxor": (0x09, 0x13), "pmsubh": (0x09, 0x14), "phmsbh": (0x09, 0x15),
    "pexeh": (0x09, 0x1A), "prevh": (0x09, 0x1B), "pmulth": (0x09, 0x1C), "pdivbw": (0x09, 0x1D),
    "pexew": (0x09, 0x1E), "prot3w": (0x09, 0x1F),
    "pmadduw": (0x29, 0x00), "psravw": (0x29, 0x03), "pmthi": (0x29, 0x08), "pmtlo": (0x29, 0x09),
    "pinteh": (0x29, 0x0A), "pmultuw": (0x29, 0x0C), "pdivuw": (0x29, 0x0D), "pcpyud": (0x29, 0x0E),
    "por": (0x29, 0x12), "pnor": (0x29, 0x13), "pexch": (0x29, 0x1A), "pcpyh": (0x29, 0x1B), "pexcw": (0x29, 0x1E),
    "pmfhl.lw": (0x30, 0), "pmfhl.uw": (0x30, 1), "pmfhl.slw": (0x30, 2), "pmfhl.lh": (0x30, 3), "pmfhl.sh": (0x30, 4),
    "pmthl.lw": (0x31, 0),
}
MMI_SHIFTS = {"psllh": 0x34, "psrlh": 0x36, "psrah": 0x37, "psllw": 0x3C, "psrlw": 0x3E, "psraw": 0x3F}


def mmx(name: str, rd: int = 0, rs: int = 0, rt: int = 0, sa: int = 0) -> int:
    """A multimedia instruction; `sa` is the shift amount of the immediate shifts."""
    if name in MMI_SHIFTS:
        funct, sub = MMI_SHIFTS[name], sa
    else:
        funct, sub = MMI_OPS[name]
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | funct


def sa_op(name: str, rd: int = 0, rs: int = 0, imm: int = 0) -> int:
    """mfsa rd / mtsa rs / mtsab rs, imm / mtsah rs, imm."""
    if name == "mfsa":
        return (rd << 11) | 0x28
    if name == "mtsa":
        return (rs << 21) | 0x29
    return (1 << 26) | (rs << 21) | ({"mtsab": 0x18, "mtsah": 0x19}[name] << 16) | (imm & 0xFFFF)


# COP1 (opcode 0x11). Format S (rs field 0x10): name -> function field; W (0x14): cvt.s.w. The moves use the rs field.
COP1_S = {
    "add.s": 0x00, "sub.s": 0x01, "mul.s": 0x02, "div.s": 0x03, "sqrt.s": 0x04, "abs.s": 0x05, "mov.s": 0x06, "neg.s": 0x07,
    "rsqrt.s": 0x16, "adda.s": 0x18, "suba.s": 0x19, "mula.s": 0x1A, "madd.s": 0x1C, "msub.s": 0x1D, "madda.s": 0x1E,
    "msuba.s": 0x1F, "cvt.w.s": 0x24, "max.s": 0x28, "min.s": 0x29, "c.f.s": 0x30, "c.eq.s": 0x32, "c.lt.s": 0x34, "c.le.s": 0x36,
}
COP1_MOVE = {"mfc1": 0x00, "cfc1": 0x02, "mtc1": 0x04, "ctc1": 0x06}
BC1 = {"bc1f": 0, "bc1t": 1, "bc1fl": 2, "bc1tl": 3}


def cop1(name: str, fd: int = 0, fs: int = 0, ft: int = 0) -> int:
    """Arithmetic and compare forms: add.s fd, fs, ft (for the compares and the unary forms, the unused fields are zero)."""
    if name == "cvt.s.w":
        return (0x11 << 26) | (0x14 << 21) | (fs << 11) | (fd << 6) | 0x20
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | COP1_S[name]


def cop1_move(name: str, rt: int, fs: int) -> int:
    return (0x11 << 26) | (COP1_MOVE[name] << 21) | (rt << 16) | (fs << 11)


def bc1(name: str, offset_words: int) -> int:
    return (0x11 << 26) | (8 << 21) | (BC1[name] << 16) | (offset_words & 0xFFFF)
