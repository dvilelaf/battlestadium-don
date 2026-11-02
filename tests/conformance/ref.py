"""Reference model of the R5900 integer instructions the conformance tests use.

Written from the reference emulator's interpreter (third_party/pcsx2/pcsx2):
  R5900OpcodeImpl.cpp   Interpreter::OpcodeImpl::{SLL,SRL,SRA,SLLV,SRLV,SRAV,DSLL,DSRL,DSRA,DSLL32,DSRL32,DSRA32,
                        DSLLV,DSRLV,DSRAV,SLT,SLTU,SLTI,SLTIU,MOVZ,MOVN,ADDIU,NOR}
  Interpreter.cpp       Interpreter::OpcodeImpl::{BEQ,BNE,BLEZ,BGTZ,BLTZ,BGEZ,BEQL,BNEL,BLEZL,BGTZL,BLTZL,BGEZL,
                        BLTZAL,BGEZAL,BLTZALL,BGEZALL,JR} and the macros _SetLink / _BranchTarget_ in R5900.h
A register is a pair (lo, hi) of unsigned 64-bit numbers; like the reference, the 64-bit instructions write only
the low half (UD[0]) and leave the high half (UD[1]) alone.
"""
from __future__ import annotations

import fpu_ref
import mmi_ref
import vu_ref

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


def s64(x: int) -> int:
    x &= M64
    return x - (1 << 64) if x >> 63 else x


def s32(x: int) -> int:
    x &= M32
    return x - (1 << 32) if x >> 31 else x


def sext32(x: int) -> int:
    """(s64)(s32)x as an unsigned 64-bit number."""
    return s32(x) & M64


class Halt(Exception):
    pass


class Overflow(Exception):
    """The instruction raises an integer-overflow exception; the harness reports such cases apart."""


HI, LO, HI1, LO1, SA = 32, 33, 34, 35, 36   # the result registers and the shift-amount register (in bytes) follow r0..r31 in the state


class Machine:
    def __init__(self, regs: list[tuple[int, int]]):
        self.lo = [r[0] & M64 for r in regs]
        self.hi = [r[1] & M64 for r in regs]
        self.lo[0] = self.hi[0] = 0
        self.pc = 0
        self.mem = fresh_block()

    def set(self, rd: int, value: int) -> None:
        if rd:  # every reference implementation starts with "if (!_Rd_) return;"
            self.lo[rd] = value & M64

    def regs(self) -> list[tuple[int, int]]:
        return list(zip(self.lo, self.hi))


def _mult_div(m: Machine, funct: int, rd: int, rs_l: int, rt_l: int, hi: int, lo: int) -> None:
    """MULT, MULTU, DIV, DIVU into the pair (hi, lo): R5900OpcodeImpl.cpp:386-443 for (HI, LO); MMI.cpp:77-130
    (MULT1, MULTU1, DIV1, DIVU1) is the same code on (HI1, LO1)."""
    a, b = s32(rs_l), s32(rt_l)
    ua, ub = rs_l & M32, rt_l & M32
    if funct == 0x18:                    # MULT: LO, HI = low and high word of the signed product, sign-extended
        p = a * b                        # (:423-433). The low word also goes to rd (UD[0] = LO, :432)
        m.lo[lo], m.lo[hi] = sext32(p), sext32(p >> 32)
        m.set(rd, m.lo[lo])
    elif funct == 0x19:                  # MULTU: same, unsigned product, still sign-extended (:435-445)
        p = ua * ub
        m.lo[lo], m.lo[hi] = sext32(p), sext32(p >> 32)
        m.set(rd, m.lo[lo])
    elif funct == 0x1A:                  # DIV (:386-404)
        if ua == 0x80000000 and ub == 0xFFFFFFFF:      # :388: LO = 0x80000000 (sign-extended), HI = 0
            m.lo[lo], m.lo[hi] = sext32(0x80000000), 0
        elif b != 0:                                   # :393: C division, truncated toward zero
            q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
            m.lo[lo], m.lo[hi] = sext32(q), sext32(a - q * b)
        else:                                          # :399: by zero: LO = 1 if rs < 0 else -1, HI = rs
            m.lo[lo], m.lo[hi] = (1 if a < 0 else M64), sext32(a)
    else:                                # DIVU (:406-421)
        if ub != 0:                                    # :410: both results sign-extended from 32 bits
            m.lo[lo], m.lo[hi] = sext32(ua // ub), sext32(ua % ub)
        else:                                          # :417: by zero: LO = -1, HI = rs (sign-extended)
            m.lo[lo], m.lo[hi] = M64, sext32(ua)


def _mmi(m: Machine, rd: int, rs: int, rt: int, funct: int) -> None:
    """MMI.cpp:19-130: MADD, MADDU, MADD1, MADDU1, MFHI1, MFLO1, MTHI1, MTLO1, MULT1, MULTU1, DIV1, DIVU1."""
    rs_l, rt_l = m.lo[rs], m.lo[rt]
    if funct in (0x00, 0x01, 0x20, 0x21):            # accumulate: the accumulator is (HI.UL[0] << 32 | LO.UL[0])
        hi, lo = (HI, LO) if funct < 0x20 else (HI1, LO1)
        acc = (m.lo[lo] & M32) | ((m.lo[hi] & M32) << 32)               # :20, :30, :40, :50
        if funct in (0x00, 0x20):
            t = (acc + s32(rs_l) * s32(rt_l)) & M64
        else:
            t = (acc + (rs_l & M32) * (rt_l & M32)) & M64
        m.lo[lo], m.lo[hi] = sext32(t), sext32(t >> 32)
        m.set(rd, m.lo[lo])                           # rd gets the sign-extended low word
    elif funct == 0x10: m.set(rd, m.lo[HI1])          # MFHI1 (:59)
    elif funct == 0x12: m.set(rd, m.lo[LO1])          # MFLO1 (:64)
    elif funct == 0x11: m.lo[HI1] = rs_l              # MTHI1 (:69)
    elif funct == 0x13: m.lo[LO1] = rs_l              # MTLO1 (:73)
    elif funct in (0x18, 0x19, 0x1A, 0x1B):
        _mult_div(m, funct, rd, rs_l, rt_l, HI1, LO1)
    else:
        raise NotImplementedError(f"MMI funct {funct:#x}")


# ---- Loads and stores ----------------------------------------------------------------------------------------------
# The guest memory of the tests is one block; it is initialised with pattern(i) (driver.cpp has the same formula).
BLOCK_BASE, BLOCK_SIZE = 0x00300000, 0x1000


def pattern(i: int) -> int:
    return (i * 131 + (i >> 8) * 17 + 7) & 0xFF


def fresh_block() -> bytearray:
    return bytearray(pattern(i) for i in range(BLOCK_SIZE))


def _rd(m: Machine, addr: int, n: int) -> int:
    o = (addr & M32) - BLOCK_BASE
    if not 0 <= o <= BLOCK_SIZE - n:
        raise IndexError(f"access outside the test block: {addr:#x}")
    return int.from_bytes(m.mem[o:o + n], "little")


def _wr(m: Machine, addr: int, n: int, value: int) -> None:
    o = (addr & M32) - BLOCK_BASE
    if not 0 <= o <= BLOCK_SIZE - n:
        raise IndexError(f"access outside the test block: {addr:#x}")
    m.mem[o:o + n] = (value & ((1 << (8 * n)) - 1)).to_bytes(n, "little")


# Masks and shifts of R5900OpcodeImpl.cpp: LWL_MASK/LWL_SHIFT/LWR_* :625-628, LDL_*/LDR_* :710-721,
# SWL_*/SWR_* :782-786, SDL_*/SDR_* :838-849.
LWL_MASK = [0xFFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000]
LWR_MASK = [0x000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00]
LWL_SHIFT, LWR_SHIFT = [24, 16, 8, 0], [0, 8, 16, 24]
LDL_MASK = [0x00FFFFFFFFFFFFFF, 0x0000FFFFFFFFFFFF, 0x000000FFFFFFFFFF, 0x00000000FFFFFFFF,
            0x0000000000FFFFFF, 0x000000000000FFFF, 0x00000000000000FF, 0x0000000000000000]
LDR_MASK = [0x0000000000000000, 0xFF00000000000000, 0xFFFF000000000000, 0xFFFFFF0000000000,
            0xFFFFFFFF00000000, 0xFFFFFFFFFF000000, 0xFFFFFFFFFFFF0000, 0xFFFFFFFFFFFFFF00]
LDR_SHIFT, LDL_SHIFT = [0, 8, 16, 24, 32, 40, 48, 56], [56, 48, 40, 32, 24, 16, 8, 0]
SWL_MASK = [0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000]
SWR_MASK = [0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF]
SWR_SHIFT, SWL_SHIFT = [0, 8, 16, 24], [24, 16, 8, 0]
SDL_MASK = [0xFFFFFFFFFFFFFF00, 0xFFFFFFFFFFFF0000, 0xFFFFFFFFFF000000, 0xFFFFFFFF00000000,
            0xFFFFFF0000000000, 0xFFFF000000000000, 0xFF00000000000000, 0x0000000000000000]
SDR_MASK = [0x0000000000000000, 0x00000000000000FF, 0x000000000000FFFF, 0x0000000000FFFFFF,
            0x00000000FFFFFFFF, 0x000000FFFFFFFFFF, 0x0000FFFFFFFFFFFF, 0x00FFFFFFFFFFFFFF]
SDL_SHIFT, SDR_SHIFT = [56, 48, 40, 32, 24, 16, 8, 0], [0, 8, 16, 24, 32, 40, 48, 56]


def _memop(m: Machine, op: int, rs: int, rt: int, simm: int) -> None:
    addr = (m.lo[rs] + simm) & M32                       # u32 addr = Rs.UL[0] + _Imm_ in every function
    reg = m.lo[rt]
    if op == 0x20:   m.set(rt, (s32(_rd(m, addr, 1) << 24) >> 24) & M64)       # LB  :520 sign-extended byte
    elif op == 0x24: m.set(rt, _rd(m, addr, 1))                                 # LBU :536
    elif op == 0x21: m.set(rt, (s32(_rd(m, addr, 2) << 16) >> 16) & M64)       # LH  :552 (aligned addresses only)
    elif op == 0x25: m.set(rt, _rd(m, addr, 2))                                 # LHU :572
    elif op == 0x23: m.set(rt, sext32(_rd(m, addr, 4)))                         # LW  :592
    elif op == 0x27: m.set(rt, _rd(m, addr, 4))                                 # LWU :612 zero-extended
    elif op == 0x37: m.lo[rt] = _rd(m, addr, 8) if rt else 0                    # LD  :700 (it even writes $zero; not tested)
    elif op == 0x22:                                                            # LWL :630 result sign-extended
        k = addr & 3
        if rt:
            m.lo[rt] = sext32((reg & M32 & LWL_MASK[k]) | ((_rd(m, addr & ~3, 4) << LWL_SHIFT[k]) & M32))
    elif op == 0x26:                                                            # LWR :654
        k = addr & 3
        if rt:
            v = ((reg & M32 & LWR_MASK[k]) | (_rd(m, addr & ~3, 4) >> LWR_SHIFT[k])) & M32
            m.lo[rt] = sext32(v) if k == 0 else (reg & ~M32 & M64) | v          # k = 0 sign-extends; else the upper half stays
    elif op == 0x1A:                                                            # LDL :723
        k = addr & 7
        if rt:
            m.lo[rt] = ((reg & LDL_MASK[k]) | ((_rd(m, addr & ~7, 8) << LDL_SHIFT[k]) & M64)) & M64
    elif op == 0x1B:                                                            # LDR :735
        k = addr & 7
        if rt:
            m.lo[rt] = ((reg & LDR_MASK[k]) | (_rd(m, addr & ~7, 8) >> LDR_SHIFT[k])) & M64
    elif op == 0x1E:                                                            # LQ :747 the address is aligned down to 16
        if rt:
            v = _rd(m, addr & ~15, 16)
            m.lo[rt], m.hi[rt] = v & M64, v >> 64
    elif op == 0x28: _wr(m, addr, 1, reg)                                       # SB  :756
    elif op == 0x29: _wr(m, addr, 2, reg)                                       # SH  :762
    elif op == 0x2B: _wr(m, addr, 4, reg)                                       # SW  :772
    elif op == 0x3F: _wr(m, addr, 8, reg)                                       # SD  :829
    elif op == 0x2A:                                                            # SWL :788
        k = addr & 3
        _wr(m, addr & ~3, 4, ((reg & M32) >> SWL_SHIFT[k]) | (_rd(m, addr & ~3, 4) & SWL_MASK[k]))
    elif op == 0x2E:                                                            # SWR :809
        k = addr & 3
        _wr(m, addr & ~3, 4, ((reg << SWR_SHIFT[k]) & M32) | (_rd(m, addr & ~3, 4) & SWR_MASK[k]))
    elif op == 0x2C:                                                            # SDL :851
        k = addr & 7
        _wr(m, addr & ~7, 8, (reg >> SDL_SHIFT[k]) | (_rd(m, addr & ~7, 8) & SDL_MASK[k]))
    elif op == 0x2D:                                                            # SDR :862
        k = addr & 7
        _wr(m, addr & ~7, 8, ((reg << SDR_SHIFT[k]) & M64) | (_rd(m, addr & ~7, 8) & SDR_MASK[k]))
    elif op == 0x1F:                                                            # SQ :872 aligned down to 16
        _wr(m, addr & ~15, 16, reg | (m.hi[rt] << 64))
    else:
        raise NotImplementedError(op)


MEM_OPCODES = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x1A, 0x1B,
               0x1E, 0x1F, 0x37, 0x3F}


def _decode(word: int):
    return (word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31, word & 63,
            word & 0xFFFF)


# SPECIAL, three-operand and shifts. Each takes (machine, rd, rs, rt, sa).
def _special(m: Machine, rd, rs, rt, sa, funct):
    L, rl = m.lo, m.lo
    rs_l, rt_l = L[rs], L[rt]
    if funct == 0x28:    m.set(rd, m.lo[SA])                               # MFSA (R5900OpcodeImpl.cpp:1213)
    elif funct == 0x29:  m.lo[SA] = rs_l & M32                            # MTSA (:1218)
    elif funct == 0x00:    m.set(rd, sext32(rt_l << sa))                    # SLL
    elif funct == 0x02:  m.set(rd, sext32((rt_l & M32) >> sa))            # SRL
    elif funct == 0x03:  m.set(rd, sext32(s32(rt_l) >> sa))               # SRA
    elif funct == 0x04:  m.set(rd, sext32(rt_l << (rs_l & 0x1F)))         # SLLV
    elif funct == 0x06:  m.set(rd, sext32((rt_l & M32) >> (rs_l & 0x1F))) # SRLV
    elif funct == 0x07:  m.set(rd, sext32(s32(rt_l) >> (rs_l & 0x1F)))    # SRAV
    elif funct == 0x14:  m.set(rd, rt_l << (rs_l & 0x3F))                 # DSLLV
    elif funct == 0x16:  m.set(rd, rt_l >> (rs_l & 0x3F))                 # DSRLV
    elif funct == 0x17:  m.set(rd, s64(rt_l) >> (rs_l & 0x3F))            # DSRAV
    elif funct == 0x38:  m.set(rd, rt_l << sa)                            # DSLL
    elif funct == 0x3A:  m.set(rd, rt_l >> sa)                            # DSRL
    elif funct == 0x3B:  m.set(rd, s64(rt_l) >> sa)                       # DSRA
    elif funct == 0x3C:  m.set(rd, rt_l << (sa + 32))                     # DSLL32
    elif funct == 0x3E:  m.set(rd, rt_l >> (sa + 32))                     # DSRL32
    elif funct == 0x3F:  m.set(rd, s64(rt_l) >> (sa + 32))                # DSRA32
    elif funct == 0x0A:                                                   # MOVZ
        if rt_l == 0:
            m.set(rd, rs_l)
    elif funct == 0x0B:                                                   # MOVN
        if rt_l != 0:
            m.set(rd, rs_l)
    elif funct == 0x27:  m.set(rd, ~(rs_l | rt_l))                        # NOR
    elif funct == 0x2A:  m.set(rd, 1 if s64(rs_l) < s64(rt_l) else 0)     # SLT
    elif funct == 0x2B:  m.set(rd, 1 if rs_l < rt_l else 0)               # SLTU
    elif funct in (0x20, 0x22):                                           # ADD, SUB (exception on overflow)
        # R5900OpcodeImpl.cpp:330 ADD / :347 SUB via _add32_Overflow (:44): the operands are the low 32 bits.
        # The reference computes SUB as rs + (-rt) on the truncated value, which misses the overflow of
        # 0 - 0x80000000; the model uses the architecture's exact rule (the difference leaves 32 bits).
        a, b = s32(rs_l), s32(rt_l)
        r = a + b if funct == 0x20 else a - b
        if not -(1 << 31) <= r < (1 << 31):
            raise Overflow(("add" if funct == 0x20 else "sub") + " 32-bit")
        m.set(rd, r)
    elif funct == 0x21:  m.set(rd, sext32(rs_l + rt_l))                   # ADDU  (R5900OpcodeImpl.cpp:364)
    elif funct == 0x23:  m.set(rd, sext32(rs_l - rt_l))                   # SUBU  (:366)
    elif funct in (0x2C, 0x2E):                                           # DADD, DSUB (:338, :356 via _add64_Overflow :22)
        a, b = s64(rs_l), s64(rt_l)
        r = a + b if funct == 0x2C else a - b
        if not -(1 << 63) <= r < (1 << 63):
            raise Overflow(("dadd" if funct == 0x2C else "dsub") + " 64-bit")
        m.set(rd, r)
    elif funct == 0x2D:  m.set(rd, rs_l + rt_l)                           # DADDU (:365)
    elif funct == 0x2F:  m.set(rd, rs_l - rt_l)                           # DSUBU (:367)
    elif funct == 0x24:  m.set(rd, rs_l & rt_l)                           # AND   (:368)
    elif funct == 0x25:  m.set(rd, rs_l | rt_l)                           # OR
    elif funct == 0x26:  m.set(rd, rs_l ^ rt_l)                           # XOR
    elif funct in (0x10, 0x12):                                           # MFHI, MFLO (:459, :460)
        m.set(rd, m.lo[HI if funct == 0x10 else LO])
    elif funct in (0x11, 0x13):                                           # MTHI, MTLO (:466, :467)
        m.lo[HI if funct == 0x11 else LO] = rs_l
    elif funct in (0x18, 0x19, 0x1A, 0x1B):
        _mult_div(m, funct, rd, rs_l, rt_l, HI, LO)
    else:
        raise NotImplementedError(f"SPECIAL funct {funct:#x}")


def _branch_cond(m: Machine, op, rs, rt):
    a, b = s64(m.lo[rs]), s64(m.lo[rt])
    if op in (0x04, 0x14): return a == b      # BEQ, BEQL
    if op in (0x05, 0x15): return a != b      # BNE, BNEL
    if op in (0x06, 0x16): return a <= 0      # BLEZ, BLEZL
    if op in (0x07, 0x17): return a > 0       # BGTZ, BGTZL
    raise NotImplementedError(op)


def run(words: list[int], regs: list[tuple[int, int]], base: int = 0, limit: int = 1000):
    """Execute the function `words` (placed at `base`) until its `jr`; return (registers, the memory block)."""
    m = Machine(regs)
    m.pc = base
    steps = 0
    while True:
        steps += 1
        if steps > limit:
            raise RuntimeError("reference model: runaway")
        word = words[(m.pc - base) // 4]
        op, rs, rt, rd, sa, funct, imm = _decode(word)
        nxt = m.pc + 4
        is_branch = False
        if op == 0 and funct == 0x08:                    # JR: run the delay slot, then leave
            _exec_one(m, words[(nxt - base) // 4])
            return m.regs(), m.mem
        if op == 1 and rt in (0x18, 0x19):               # MTSAB (:1271), MTSAH (:1275)
            if rt == 0x18:
                m.lo[SA] = (m.lo[rs] & 0xF) ^ (imm & 0xF)
            else:
                m.lo[SA] = ((m.lo[rs] & 0x7) ^ (imm & 0x7)) << 1
            m.pc = nxt
            continue
        if op == 0x11 and rs == 8:                       # BC1F, BC1T, BC1FL, BC1TL (FPU.cpp:BC1, BC1L)
            taken = fpu_ref.condition(m) == bool(rt & 1)
            likely = bool(rt & 2)
            target = nxt + 4 * s32(imm if imm < 0x8000 else imm - 0x10000)
            if taken:
                _exec_one(m, words[(nxt - base) // 4])
                m.pc = target
            elif likely:
                m.pc = nxt + 4
            else:
                _exec_one(m, words[(nxt - base) // 4])
                m.pc = nxt + 4
            continue
        if op in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or op == 1:
            is_branch = True
            target = nxt + 4 * s32(imm if imm < 0x8000 else imm - 0x10000)
            likely = False
            link = False
            if op == 1:
                kind = rt
                likely = kind in (0x02, 0x03, 0x12, 0x13)
                link = kind >= 0x10
                if link:
                    m.lo[31] = (m.pc + 8) & M64            # _SetLink(31): before the condition, always
                a = s64(m.lo[rs])
                cond = a < 0 if kind in (0x00, 0x02, 0x10, 0x12) else a >= 0
            else:
                likely = op >= 0x14
                cond = _branch_cond(m, op, rs, rt)
            if cond:
                _exec_one(m, words[(nxt - base) // 4])   # the delay slot, after the condition was decided
                m.pc = target
            elif likely:
                m.pc = nxt + 4                            # a not-taken likely branch skips its delay slot
            else:
                _exec_one(m, words[(nxt - base) // 4])
                m.pc = nxt + 4
            continue
        _exec_one(m, word)
        m.pc = nxt


def _exec_one(m: Machine, word: int) -> None:
    op, rs, rt, rd, sa, funct, imm = _decode(word)
    simm = imm - 0x10000 if imm >= 0x8000 else imm
    if op == 0:
        _special(m, rd, rs, rt, sa, funct)
    elif op == 0x09:                                      # ADDIU
        m.set(rt, sext32(m.lo[rs] + simm))
    elif op == 0x0A:                                      # SLTI: Rs < sign-extended immediate, signed
        m.set(rt, 1 if s64(m.lo[rs]) < simm else 0)
    elif op == 0x08:                                      # ADDI (R5900OpcodeImpl.cpp:282): exception on overflow
        r = s32(m.lo[rs]) + simm
        if not -(1 << 31) <= r < (1 << 31):
            raise Overflow("addi")
        m.set(rt, r)
    elif op == 0x18:                                      # DADDI (:302)
        r = s64(m.lo[rs]) + simm
        if not -(1 << 63) <= r < (1 << 63):
            raise Overflow("daddi")
        m.set(rt, r)
    elif op == 0x19:  m.set(rt, m.lo[rs] + (simm & M64))  # DADDIU (:313)
    elif op == 0x0C:  m.set(rt, m.lo[rs] & imm)           # ANDI: the immediate is zero-extended (:318)
    elif op == 0x0D:  m.set(rt, m.lo[rs] | imm)           # ORI (:319)
    elif op == 0x0E:  m.set(rt, m.lo[rs] ^ imm)           # XORI (:320)
    elif op == 0x0F:  m.set(rt, sext32(imm << 16))        # LUI: sign-extended from bit 31 (:450)
    elif op == 0x11:                                      # COP1 (FPU.cpp)
        fpu_ref.exec_cop1(m, word)
    elif op == 0x12:                                      # COP2 (VU0.cpp, VUops.cpp)
        vu_ref.exec_cop2(m, word)
    elif op == 0x36:                                      # LQC2 (VU0.cpp:LQC2): the address is rounded down to 16 bytes
        v = _rd(m, (m.lo[rs] + simm) & ~15 & M32, 16)
        if rt:
            m.lo[vu_ref.VF0 + rt], m.hi[vu_ref.VF0 + rt] = v & M64, v >> 64
    elif op == 0x3E:                                      # SQC2
        _wr(m, (m.lo[rs] + simm) & ~15 & M32, 16, m.lo[vu_ref.VF0 + rt] | m.hi[vu_ref.VF0 + rt] << 64)
    elif op == 0x31:                                      # LWC1 (FPU.cpp:LWC1)
        m.lo[fpu_ref.F0 + rt] = _rd(m, m.lo[rs] + simm, 4)
    elif op == 0x39:                                      # SWC1
        _wr(m, m.lo[rs] + simm, 4, m.lo[fpu_ref.F0 + rt])
    elif op == 0x1C:                                      # MMI.cpp
        if funct in (0x00, 0x01, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x20, 0x21):
            _mmi(m, rd, rs, rt, funct)
        elif not mmi_ref.exec_mmi(m, funct, sa, rd, rs, rt, sa):
            raise NotImplementedError(f"MMI funct {funct:#x} sub {sa:#x}")
    elif op in MEM_OPCODES:
        _memop(m, op, rs, rt, simm)
    elif op == 0x0B:                                      # SLTIU: the immediate is (u64)(s16), compared unsigned
        m.set(rt, 1 if m.lo[rs] < (simm & M64) else 0)
    else:
        raise NotImplementedError(f"opcode {op:#x} in a delay slot")
