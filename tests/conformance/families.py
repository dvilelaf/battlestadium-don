"""The instruction families under test: guest functions, operand classes and the comparison with ref.py.

To add a family: write a function `family_x(prog) -> list[Form]` that adds its guest functions to `prog`
(Program.add returns the function index) and lists the cases of each form; register it in FAMILIES.
A Form is one instruction (mnemonic) in one shape (variant); its cases are 32-register start states.
"""
from __future__ import annotations

import itertools
from dataclasses import dataclass, field

import asm
import harness
import ref
from asm import A0, A1, JR_RA, NOP, T0, V0, V1, ZERO

# Operand classes: 64-bit values, most of them NOT sign-extended 32-bit numbers.
VALUES = [
    0,
    1,
    0x7FFFFFFF,                 # largest positive 32-bit
    0x80000000,                 # bit 31 set, upper half zero (a positive 64-bit number)
    0xFFFFFFFF,                 # bits 0-31 set, upper half zero
    0x1_00000000,               # upper half set, lower half zero
    0x1_80000000,               # upper half 1, bit 31 set
    0xFFFFFFFF_80000000,        # sign-extended negative 32-bit number
    0xFFFFFFFF_FFFFFFFF,        # -1
    0x7FFFFFFF_FFFFFFFF,        # most positive 64-bit
    0x80000000_00000000,        # most negative 64-bit
    0x80000000_00000001,        # most negative + 1, low word positive
    0x12345678_9ABCDEF0,
]


def default_regs() -> list[tuple[int, int]]:
    """Start state of every case: distinct patterns in both halves, so an unwanted write shows."""
    regs = [(0xC0DE000000000000 | (r << 8) | 0x11, 0xA5A5A5A500000000 | (r << 4) | 0x5) for r in range(32)]
    regs[0] = (0, 0)                      # r0 is hard-wired to zero
    regs += [(0x0123456789ABCDEF, 0), (0xFEDCBA9876543210, 0), (0x0F1E2D3C4B5A6978, 0), (0x8796A5B4C3D2E1F0, 0), (5, 0)]  # HI LO HI1 LO1 SA (in bytes)
    regs += [(0x3F800000 + (i << 16) + i, 0) for i in range(32)] + [(0x40490FDB, 0), (0, 0)]   # F0..F31, ACC, FCR31
    regs += vu_default()
    return regs


def vu_default() -> list[tuple[int, int]]:
    """The vector unit's start state (index layout in vu_ref.py): four different normal numbers in every lane, VF0 = (0, 0, 0, 1)."""
    lane = lambda r, l: 0x3F000000 + ((r * 4 + l) << 14) + (r * 4 + l) * 3          # noqa: E731
    out = []
    for r in range(32):
        out.append((0, 0x3F800000 << 32) if r == 0 else (lane(r, 0) | lane(r, 1) << 32, lane(r, 2) | lane(r, 3) << 32))
    out += [(0 if i == 0 else 0x1000 + i * 0x11, 0) for i in range(16)]                    # VI0..15
    out += [(0x40100000 | 0x1111 | (0x40200000 | 0x2222) << 32, 0x40300000 | 0x3333 | (0x40400000 | 0x4444) << 32)]   # ACC
    out += [(0x40490FDB, 0), (0x3F000005, 0), (0x40000001, 0), (0x3F812345, 0)]          # Q P I R
    out += [(0x5, 0), (0x8421, 0), (0x123456, 0)]                                         # status MAC clip
    out += [(0x1234, 0), (0x55AA, 0), (0x0, 0), (0x0, 0), (0x0, 0)]                      # TPC CMSAR0 FBRST VPU_STAT CMSAR1
    return out


def state(**regs) -> list[tuple[int, int]]:
    """default_regs with the low half of named registers replaced: state(a0=1, a1=2) (names a0,a1,v0,t0,...)."""
    names = {"hi": 32, "lo": 33, "hi1": 34, "lo1": 35, "v0": 2, "v1": 3, "a0": 4, "a1": 5, "a2": 6, "t0": 8, "t1": 9, "ra": 31}
    out = default_regs()
    for n, v in regs.items():
        out[names[n]] = (v & ref.M64, out[names[n]][1])
    return out


@dataclass
class Case:
    desc: str
    start: list[tuple[int, int]]
    amb: bool = False        # the reference itself is ambiguous here (T-210 piece 5): a difference is counted apart
    cls: str = ""            # operand class of the case, for the per-class count of failures


@dataclass
class Form:
    mnemonic: str
    variant: str
    func: int
    cases: list[Case] = field(default_factory=list)
    flags_ignored: int = 0   # FCR31 bits not compared (the sticky exception flags: no site in the game reads them)
    ignore: tuple = ()       # register indexes not compared (the vector unit's flag registers: no site in the game reads them)
    alt: bool = False        # division-like: also try the two other readings of the reference (fpu_ref.READING)


def hx(v: int) -> str:
    return f"{v & ref.M64:#x}"


# ---- Family A: conditional branches -----------------------------------------------------------------------------
BRANCH2 = ["beq", "bne", "beql", "bnel"]
BRANCH1 = ["blez", "bgtz", "blezl", "bgtzl", "bltz", "bgez", "bltzl", "bgezl",
           "bltzal", "bgezal", "bltzall", "bgezall"]
DELAYS = {
    "mark": asm.i_type("addiu", ZERO, T0, 7),                 # the delay slot leaves a trace in t0
    "mod_rs": asm.r_type("nor", rd=A0, rs=A0, rt=ZERO),       # the delay slot changes the tested register a0
    "mod_rt": asm.r_type("nor", rd=A1, rs=A1, rt=ZERO),       # ... or a1 (two-register branches only)
}


def family_branches(prog: harness.Program) -> list[Form]:
    forms = []
    for name in BRANCH2 + BRANCH1:
        two = name in BRANCH2
        for dname, dword in DELAYS.items():
            if dname == "mod_rt" and not two:
                continue
            words = [
                asm.branch(name, A0, A1 if two else ZERO, 4),     # 0: target = 4 + 4 + 4*4 -> index 5... see below
                dword,                                              # 1: delay slot
                asm.i_type("addiu", ZERO, V1, 1),                   # 2: reached when not taken
                JR_RA, NOP,                                         # 3, 4
                asm.i_type("addiu", ZERO, V1, 2),                   # 5: the target
                JR_RA, NOP,
            ]
            f = Form(name, dname, prog.add(f"{name}_{dname}", words))
            pairs = itertools.product(VALUES, VALUES) if two else ((v, 0) for v in VALUES)
            for a, b in pairs:
                f.cases.append(Case(f"a0={hx(a)} a1={hx(b)}" if two else f"a0={hx(a)}", state(a0=a, a1=b)))
            forms.append(f)
    return forms


# ---- Family B: comparisons and conditional moves ------------------------------------------------------------------
IMMS = [-1, 0, 1, 0x7FFF, -0x8000]


def _three(name, rd=V0, rs=A0, rt=A1):
    return [asm.r_type(name, rd=rd, rs=rs, rt=rt), JR_RA, NOP]


def family_compare(prog: harness.Program) -> list[Form]:
    forms = []
    for name in ["slt", "sltu", "movz", "movn"]:
        f = Form(name, "rd=v0,rs=a0,rt=a1", prog.add(name, _three(name)))
        for a, b in itertools.product(VALUES, VALUES):
            f.cases.append(Case(f"a0={hx(a)} a1={hx(b)}", state(a0=a, a1=b)))
        forms.append(f)
    for name in ["slti", "sltiu"]:
        for imm in IMMS:
            words = [asm.i_type(name, A0, V0, imm), JR_RA, NOP]
            f = Form(name, f"imm={imm}", prog.add(f"{name}_{imm}", words))
            for a in VALUES + [(imm & ref.M64), (imm + 1) & ref.M64, (imm - 1) & ref.M64, imm & 0xFFFF]:
                f.cases.append(Case(f"a0={hx(a)}", state(a0=a)))
            forms.append(f)
    return forms


# ---- Family C: shifts ---------------------------------------------------------------------------------------------
SAS = [0, 1, 31]
AMOUNTS = [0, 1, 31, 32, 33, 63, 64, 65, 0x1_00000001, 0xFFFFFFFF_FFFFFFE1, 0x80000000_0000003F]
FIXED = ["sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"]
VARIABLE = ["sllv", "srlv", "srav", "dsllv", "dsrlv", "dsrav"]


def family_shifts(prog: harness.Program) -> list[Form]:
    forms = []
    for name in FIXED:
        for sa in SAS:
            words = [asm.r_type(name, rd=V0, rt=A1, sa=sa), JR_RA, NOP]
            f = Form(name, f"sa={sa}", prog.add(f"{name}_{sa}", words))
            for b in VALUES:
                f.cases.append(Case(f"a1={hx(b)}", state(a1=b)))
            forms.append(f)
    for name in VARIABLE:
        words = [asm.r_type(name, rd=V0, rs=A0, rt=A1), JR_RA, NOP]
        f = Form(name, "rd=v0,rt=a1,rs=a0", prog.add(name, words))
        for amount, b in itertools.product(AMOUNTS, VALUES):
            f.cases.append(Case(f"a0={hx(amount)} a1={hx(b)}", state(a0=amount, a1=b)))
        forms.append(f)
    return forms




# ---- Family D: arithmetic and logic ---------------------------------------------------------------------------------
ARITH3 = ["add", "addu", "sub", "subu", "dadd", "daddu", "dsub", "dsubu", "and", "or", "xor", "nor"]
# The register shapes: all distinct; rd = rs (accumulate in place); rs = rt (same operand twice); rd = $zero.
SHAPES3 = {"rd=v0,rs=a0,rt=a1": (V0, A0, A1), "rd=rs=a0,rt=a1": (A0, A0, A1), "rd=v0,rs=rt=a0": (V0, A0, A0),
           "rd=zero,rs=a0,rt=a1": (ZERO, A0, A1)}
ARITH_IMM = ["addi", "addiu", "daddi", "daddiu"]
LOGIC_IMM = ["andi", "ori", "xori"]
IMM16 = [0, 1, 0x7FFF, 0x8000, 0xFFFF]      # as the 16-bit field; the signed forms read 0x8000 as -32768, 0xFFFF as -1


def family_arith(prog: harness.Program) -> list[Form]:
    forms = []
    for name in ARITH3:
        for shape, (rd, rs, rt) in SHAPES3.items():
            f = Form(name, shape, prog.add(f"{name}_{shape}", [asm.r_type(name, rd=rd, rs=rs, rt=rt), JR_RA, NOP]))
            for a, b in itertools.product(VALUES, VALUES):
                f.cases.append(Case(f"a0={hx(a)} a1={hx(b)}", state(a0=a, a1=b)))
            forms.append(f)
    for name in ARITH_IMM + LOGIC_IMM:
        for imm in IMM16:
            for shape, rt in (("rt=v0,rs=a0", V0), ("rt=rs=a0", A0), ("rt=zero,rs=a0", ZERO)):
                words = [asm.i_type(name, A0, rt, imm), JR_RA, NOP]
                f = Form(name, f"imm={imm:#x},{shape}", prog.add(f"{name}_{imm:x}_{shape}", words))
                for a in VALUES:
                    f.cases.append(Case(f"a0={hx(a)}", state(a0=a)))
                forms.append(f)
    for imm in IMM16:
        f = Form("lui", f"imm={imm:#x}", prog.add(f"lui_{imm:x}", [asm.i_type("lui", ZERO, V0, imm), JR_RA, NOP]))
        f.cases.append(Case("-", state()))
        forms.append(f)
        f = Form("lui", f"imm={imm:#x},rt=zero", prog.add(f"lui0_{imm:x}", [asm.i_type("lui", ZERO, ZERO, imm), JR_RA, NOP]))
        f.cases.append(Case("-", state()))
        forms.append(f)
    return forms


# ---- Family E: multiplication, division and the result registers -----------------------------------------------------
# Accumulators (HI, LO) for the accumulating forms: low words only count; upper halves are garbage on purpose.
ACCS = [(0, 0), (0, 0xFFFFFFFF), (0x7FFFFFFF, 0xFFFFFFFF), (0xFFFFFFFF_80000000, 0x00000000_80000000),
        (0x1_00000005, 0x2_00000003), (0xFFFFFFFF_FFFFFFFF, 0xFFFFFFFF_FFFFFFFF)]
RESULT_START = dict(hi=0x1111222233334444, lo=0x5555666677778888, hi1=0x9999AAAABBBBCCCC, lo1=0xDDDDEEEEFFFF0001)
MULT2 = ["mult", "multu", "mult1", "multu1", "madd", "maddu", "madd1", "maddu1"]
DIV2 = ["div", "divu", "div1", "divu1"]


def _mw(name, rd=ZERO, rs=A0, rt=A1):
    return asm.mmi(name, rd=rd, rs=rs, rt=rt) if name in asm.MMI else asm.r_type(name, rd=rd, rs=rs, rt=rt)


def family_muldiv(prog: harness.Program) -> list[Form]:
    forms = []
    for name in MULT2:
        accumulate = name.startswith("madd")
        for shape, rd in (("rd=zero", ZERO), ("rd=v0", V0), ("rd=rs=a0", A0)):
            f = Form(name, shape, prog.add(f"{name}_{shape}", [_mw(name, rd=rd), JR_RA, NOP]))
            for a, b in itertools.product(VALUES, VALUES):
                for hi, lo in (ACCS if accumulate else ACCS[:1]):
                    start = dict(RESULT_START)
                    if accumulate:
                        start.update(hi=hi, lo=lo, hi1=hi, lo1=lo)
                    f.cases.append(Case(f"a0={hx(a)} a1={hx(b)}" + (f" acc={hx(hi)}:{hx(lo)}" if accumulate else ""),
                                        state(a0=a, a1=b, **start)))
            forms.append(f)
    for name in DIV2:
        f = Form(name, "rs=a0,rt=a1", prog.add(name, [_mw(name), JR_RA, NOP]))
        for a, b in itertools.product(VALUES + [0x80000000, 0xFFFFFFFF, 0x7FFFFFFF_00000005, 0xFFFFFFFF_FFFFFFFB],
                                      VALUES + [0xFFFFFFFF, 0xFFFFFFFF_FFFFFFFF, 0x1_00000000 | 3]):
            f.cases.append(Case(f"a0={hx(a)} a1={hx(b)}", state(a0=a, a1=b, **RESULT_START)))
        forms.append(f)
    for name in ["mfhi", "mflo", "mfhi1", "mflo1"]:
        for shape, rd in (("rd=v0", V0), ("rd=zero", ZERO)):
            f = Form(name, shape, prog.add(f"{name}_{shape}", [_mw(name, rd=rd), JR_RA, NOP]))
            for hi, lo in ACCS:
                f.cases.append(Case(f"hi={hx(hi)} lo={hx(lo)}", state(hi=hi, lo=lo, hi1=lo, lo1=hi)))
            forms.append(f)
    for name in ["mthi", "mtlo", "mthi1", "mtlo1"]:
        f = Form(name, "rs=a0", prog.add(name, [_mw(name), JR_RA, NOP]))
        for a in VALUES:
            f.cases.append(Case(f"a0={hx(a)}", state(a0=a, **RESULT_START)))
        forms.append(f)
    return forms


# ---- Family F: loads and stores ---------------------------------------------------------------------------------------
# a0 is the base register, a1 the data register. The effective address is base + imm; each case picks the address
# inside the 4 KB test block (ref.BLOCK_BASE) and derives the base register from it, so every immediate (positive,
# negative, the extremes) reaches the same bytes. The upper half of the base register is garbage on purpose: the
# address is a 32-bit sum of the low half.
LOADS = ["lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lq"]
STORES = ["sb", "sh", "sw", "sd", "sq"]
WIDTH = {"lb": 1, "lbu": 1, "sb": 1, "lh": 2, "lhu": 2, "sh": 2, "lw": 4, "lwu": 4, "sw": 4, "ld": 8, "sd": 8,
         "lq": 16, "sq": 16, "lwl": 4, "lwr": 4, "swl": 4, "swr": 4, "ldl": 8, "ldr": 8, "sdl": 8, "sdr": 8}
UNALIGNED = ["lwl", "lwr", "swl", "swr", "ldl", "ldr", "sdl", "sdr"]
# the idiom for an unaligned access: the "right" part at the address, the "left" part at address + width - 1
PAIRS = [("lwr", "lwl"), ("lwl", "lwr"), ("ldr", "ldl"), ("ldl", "ldr"),
         ("swr", "swl"), ("swl", "swr"), ("sdr", "sdl"), ("sdl", "sdr")]
MEM_IMMS = [0, 7, -7, 0x7FFF, -0x8000]
UPPERS = [0, 0xFFFFFFFF_00000000, 0x12345678_00000000]
HI_VALUES = [0x0, 0xFEDCBA98_76543210]


def _is_load(name):
    return name.startswith("l")


def _mem_start(base_ea, imm, a1=None, a1hi=None, rt_shape="a1", n=0):
    base = ((ref.BLOCK_BASE + base_ea - imm) & ref.M32) | UPPERS[n % len(UPPERS)]
    st = state(a0=base)
    if a1 is not None:
        st[5] = (a1 & ref.M64, st[5][1] if a1hi is None else a1hi)
    return st


def family_memory(prog: harness.Program) -> list[Form]:
    forms = []
    shapes = (("rt=a1", A1), ("rt=base", A0), ("rt=zero", ZERO))
    # aligned forms
    for name in LOADS + STORES:
        w = WIDTH[name]
        eas = sorted({0, 0x1000 - w, 0x7F0} | {0x30 + w * j for j in range(3)})
        for shape, rt in shapes:
            for imm in MEM_IMMS:
                f = Form(name, f"{shape},imm={imm:#x}", prog.add(f"{name}_{rt}_{imm & 0xFFFF:x}",
                                                                  [asm.mem(name, rt, A0, imm), JR_RA, NOP]))
                vals = VALUES if (not _is_load(name) and rt == A1) else [VALUES[0], VALUES[8], VALUES[12]]
                n = 0
                for ea in eas:
                    for v in vals:
                        for hi in (HI_VALUES if name == "sq" and rt == A1 else [None]):
                            f.cases.append(Case(f"ea={ea:#x} a1={hx(v)}" + (f" hi={hi:#x}" if hi else ""),
                                                _mem_start(ea, imm, v, hi, n=n)))
                            n += 1
                forms.append(f)
    # the quadword forms with an address that is not a multiple of 16
    for name in ("lq", "sq"):
        f = Form(name, "unaligned address", prog.add(f"{name}_un", [asm.mem(name, A1, A0, 0), JR_RA, NOP]))
        for k in (1, 4, 8, 15):
            for v in VALUES[:6]:
                f.cases.append(Case(f"ea=0x40+{k} a1={hx(v)}", _mem_start(0x40 + k, 0, v, 0xFEDCBA98_76543210)))
        forms.append(f)
    # the unaligned halves, single, at every byte offset of the address
    for name in UNALIGNED:
        w = WIDTH[name]
        eas = [b + k for b in (0x40, 0x800, 0x1000 - w) for k in range(w)]
        for shape, rt in shapes:
            for imm in MEM_IMMS:
                f = Form(name, f"{shape},imm={imm:#x}", prog.add(f"{name}_{rt}_{imm & 0xFFFF:x}",
                                                                  [asm.mem(name, rt, A0, imm), JR_RA, NOP]))
                n = 0
                for ea in eas:
                    for v in VALUES:
                        f.cases.append(Case(f"ea={ea:#x} a1={hx(v)}", _mem_start(ea, imm, v, n=n)))
                        n += 1
                forms.append(f)
    # the pairs, as the compiler emits them: both halves on the same register
    for first, second in PAIRS:
        w = WIDTH[first]
        eas = [b + k for b in (0x40, 0x800, 0xFF0) for k in range(w)]
        for imm in (0, -9, 0x7F00):
            fn = prog.add(f"pair_{first}_{second}_{imm & 0xFFFF:x}",
                          [asm.mem(first, A1, A0, imm), asm.mem(second, A1, A0, imm + w - 1), JR_RA, NOP])
            f = Form(first, f"pair {first}+{second},imm={imm:#x}", fn)
            n = 0
            for ea in eas:
                for v in VALUES:
                    f.cases.append(Case(f"ea={ea:#x} a1={hx(v)}", _mem_start(ea, imm, v, n=n)))
                    n += 1
            forms.append(f)
    return forms



def family_mmi(prog):
    import family_mmi as fm
    return fm.family_mmi(prog, Form, Case, default_regs, hx)


def family_fpu(prog):
    import family_fpu as ff
    return ff.family_fpu(prog, Form, Case, default_regs, hx, _mem_start)


def family_vu0(prog):
    import family_vu0 as fv
    return fv.family_vu0(prog, Form, Case, default_regs, hx, _mem_start)


FAMILIES = {"branches": family_branches, "compare": family_compare, "shifts": family_shifts,
            "arith": family_arith, "muldiv": family_muldiv, "memory": family_memory, "mmi": family_mmi,
            "fpu": family_fpu, "vu0": family_vu0}


# ---- Running ------------------------------------------------------------------------------------------------------
@dataclass
class Result:
    mnemonic: str
    cases: int = 0
    failures: list[str] = field(default_factory=list)   # first one = the smallest failing case
    overflow: int = 0                                   # cases the instruction answers with an exception: not run
    ambiguous: list[str] = field(default_factory=list)  # differences where the reference is ambiguous (not failures)
    flags_only: list[str] = field(default_factory=list) # differences only in the exception flags of FCR31 (not failures)
    classes: dict[str, int] = field(default_factory=dict)  # failing cases per operand class
    fail_cls: list[str] = field(default_factory=list)      # the operand class of each entry of failures


REGNAMES = [f"r{i}" for i in range(32)] + ["HI", "LO", "HI1", "LO1", "SA"] + [f"f{i}" for i in range(32)] + ["ACC", "FCR31"] \
    + [f"vf{i}" for i in range(32)] + [f"vi{i}" for i in range(16)] + ["vACC", "Q", "P", "I", "R", "STATUS", "MAC", "CLIP", "TPC", "CMSAR0", "FBRST", "VPU_STAT", "CMSAR1"]


def diff(got, want, ignore=()):
    if isinstance(got[0], str):
        return [f"not translated: the generated code throws ({got[0][6:70]})"]
    (got, gmem), (want, wmem) = got, want
    out = []
    if gmem != wmem:
        bad = sorted(o for o in set(gmem) | set(wmem) if gmem.get(o, ref.pattern(o)) != wmem.get(o, ref.pattern(o)))
        out.append("memory: " + ", ".join(f"[{o:#x}] got {gmem.get(o, ref.pattern(o)):02x} want {wmem.get(o, ref.pattern(o)):02x}" for o in bad[:4])
                   + f" ({len(bad)} bytes differ)")
    for r in range(harness.NREG):
        if r not in ignore and got[r] != want[r]:
            out.append(f"{REGNAMES[r]}: got {got[r][1]:016x}:{got[r][0]:016x} want {want[r][1]:016x}:{want[r][0]:016x}")
    return out


def run_family(name: str) -> tuple[list[Result], harness.Program]:
    prog = harness.Program(name)
    forms = FAMILIES[name](prog)
    workdir = harness.ROOT / "build" / "conformance" / name
    exe = harness.build(prog, workdir)
    results: dict[str, Result] = {}
    flat = []
    for f in forms:
        res = results.setdefault(f.mnemonic, Result(f.mnemonic))
        for c in f.cases:
            try:
                wregs, wblock = ref.run(prog.funcs[f.func].words, c.start, base=prog.funcs[f.func].addr)
                want = (wregs, {i: b for i, b in enumerate(wblock) if b != ref.pattern(i)})
            except ref.Overflow:
                res.overflow += 1       # the port calls runtime->SignalException here; see journal 025
                continue
            flat.append((f, c, want))
    got = harness.run_cases(exe, [(f.func, c.start) for f, c, _ in flat], workdir)
    import fpu_ref
    for (f, c, want), regs in zip(flat, got):
        res = results[f.mnemonic]
        res.cases += 1
        d = diff(regs, want, f.ignore)
        if not d:
            if f.ignore and diff(regs, want):
                res.flags_only.append(f"{f.variant} {c.desc}: " + "; ".join(diff(regs, want)[:3]))
            continue
        msg = f"{f.variant} {c.desc}: " + "; ".join(d[:3])
        if c.amb:
            res.ambiguous.append(msg)
            continue
        if f.alt and not isinstance(regs[0], str):
            for reading in (1, 2):
                fpu_ref.READING = reading
                try:
                    w2, b2 = ref.run(prog.funcs[f.func].words, c.start, base=prog.funcs[f.func].addr)
                finally:
                    fpu_ref.READING = 0
                if not diff(regs, (w2, {i: b for i, b in enumerate(b2) if b != ref.pattern(i)})):
                    res.ambiguous.append(f"reading {reading}: " + msg)
                    break
            else:
                reading = 0
            if reading:
                continue
        if f.flags_ignored and not isinstance(regs[0], str):
            m = ~f.flags_ignored & 0xFFFFFFFF
            g, w = regs[0], want[0]
            if all(g[i] == w[i] for i in range(70)) and g[71:] == w[71:] and regs[1] == want[1] and (g[70][0] & m) == (w[70][0] & m):
                res.flags_only.append(msg)
                continue
        res.failures.append(msg)
        res.fail_cls.append(c.cls)
        res.classes[c.cls] = res.classes.get(c.cls, 0) + 1
    return list(results.values()), prog
