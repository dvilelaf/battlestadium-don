#!/usr/bin/env python3
"""Ahead-of-time translation of an IOP (MIPS I, R3000) code range to C++ that runs on the real IopCpuState/IopMemory.

Input: a flat, relocated image of one IOP module (work/iop-native/adxi.img, base 0x6e000, made by loadimg.py).
Output: ONE C++ file (default work/iop-recomp/adxi_native.cpp, git-ignored: it is derived from game code) with
    uint32_t ps2x::iop::detail::adxi_native::run(IopCpuState&, IopMemory&, uint32_t budget)
which runs the module natively and returns the number of instructions executed (== cycles of the interpreter).

Contract (work/design/iop-recomp.md, section 3), all of it static:
  * One instruction == one cycle. Every basic block has a constant cost (a branch and its delay slot count 2);
    the block runs only if cost <= remaining budget, else run() returns with pc at the block's first instruction.
  * No C++ call stack for guest calls: jal is `ra = pc+8; goto block`, jr/jalr go through one `switch` over the
    block entry points. All guest state is in IopCpuState on return, so the interpreter simply continues.
  * run() leaves with pc at the next UNEXECUTED instruction (and the interpreter's own state around it) when:
    the budget is too small for the next block; the target of a jump is not an entry of the translated set (other
    module, import stub, sentinel, a refused function); `break`; div/divu by zero or INT_MIN/-1; add/addi/sub
    overflow; a load/store that is misaligned or not plain RAM; a store into the code range. An exit on the
    delay-slot instruction leaves branchPending/branchTarget set exactly as the interpreter would have; an exit on
    the instruction after a load leaves the load pending (pendingLoad*) exactly as the interpreter would have.
  * stale() becomes true when a store into the code range was handed to the interpreter; run() then refuses to run
    (returns 0) until resetStale(). The host should drop the module for good at that point.
  * Functions with a load-delay hazard (the instruction after a load reads the loaded register, a load in a delay
    slot, a load at the end of the function; a block that would end right after a load is extended over the next
    instruction instead), a control transfer in a delay slot, or an opcode outside
    the supported subset are refused: nothing is emitted for them, jumps to them exit.
Host preconditions (not checked here beyond branchPending/pendingLoad): no interrupt is deliverable and no DMA or
guest callback is due before `budget` instructions have run (the interpreter checks those between instructions;
native code changes none of their sources, since hardware accesses exit).

Usage: uv run --no-project --with rabbitizer python -I tools/iop_recomp/emit_adxi.py [--image F] [--out F]
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

RAM_SIZE = 0x200000

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x08: "jr", 0x09: "jalr",
    0x0C: "syscall", 0x0D: "break", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x18: "mult",
    0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}
REGIMM = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}
OPCODES = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz", 0x08: "addi", 0x09: "addiu",
    0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x20: "lb", 0x21: "lh",
    0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x28: "sb", 0x29: "sh", 0x2B: "sw",
}
# Names recognised but not translated (the function is refused): lwl/lwr/swl/swr, coprocessors, syscall.
KNOWN_UNSUPPORTED = {0x22: "lwl", 0x26: "lwr", 0x2A: "swl", 0x2E: "swr", 0x10: "cop0", 0x12: "cop2"}

BRANCHES_COND = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
LOADS = {"lb", "lbu", "lh", "lhu", "lw"}
STORES = {"sb", "sh", "sw"}
SLOTTED = BRANCHES_COND | {"j", "jal", "jr", "jalr"}


@dataclass(frozen=True)
class Insn:
    pc: int
    word: int
    name: str  # "bad" when not translatable
    rs: int
    rt: int
    rd: int
    sa: int
    imm: int
    simm: int

    @property
    def is_load(self) -> bool:
        return self.name in LOADS

    @property
    def has_slot(self) -> bool:
        return self.name in SLOTTED

    @property
    def target(self) -> int | None:
        """Static target of a branch or j/jal, else None."""
        if self.name in BRANCHES_COND:
            return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF
        if self.name in ("j", "jal"):
            return (((self.pc + 4) & 0xF0000000) | ((self.word & 0x03FFFFFF) << 2)) & 0xFFFFFFFF
        return None


def decode(pc: int, word: int) -> Insn:
    op = word >> 26
    rs, rt, rd, sa = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31
    imm = word & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 0:
        name = SPECIAL.get(word & 63, "bad")
    elif op == 1:
        name = REGIMM.get(rt, "bad")
    else:
        name = OPCODES.get(op, "bad")
    return Insn(pc, word, name, rs, rt, rd, sa, imm, simm)


def reads(i: Insn) -> set[int]:
    n = i.name
    if n in ("sll", "srl", "sra"):
        r = {i.rt}
    elif n in ("sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu",
               "mult", "multu", "div", "divu", "beq", "bne"):
        r = {i.rs, i.rt}
    elif n in ("jr", "jalr", "mthi", "mtlo", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal",
               "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori") or n in LOADS:
        r = {i.rs}
    elif n in STORES:
        r = {i.rs, i.rt}
    else:
        r = set()
    r.discard(0)
    return r


def writes(i: Insn) -> int | None:
    """Destination GPR (None when it writes none, or only r0)."""
    n = i.name
    if n in ("sll", "srl", "sra", "sllv", "srlv", "srav", "mfhi", "mflo", "add", "addu", "sub", "subu", "and", "or",
             "xor", "nor", "slt", "sltu"):
        d = i.rd
    elif n == "jalr":
        d = i.rd if i.rd else 31
    elif n in ("jal", "bltzal", "bgezal"):
        d = 31
    elif n in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui") or n in LOADS:
        d = i.rt
    else:
        d = 0
    return d or None


class Image:
    def __init__(self, data: bytes, base: int):
        self.data = data
        self.base = base

    @property
    def end(self) -> int:
        return self.base + len(self.data) // 4 * 4

    def word(self, a: int) -> int | None:
        o = a - self.base
        if o < 0 or o + 4 > len(self.data) or a & 3:
            return None
        return struct.unpack_from("<I", self.data, o)[0]

    def insn(self, a: int) -> Insn:
        w = self.word(a)
        return decode(a, 0xFFFFFFFF if w is None else w)


@dataclass
class Block:
    start: int
    insns: list[Insn]
    fallthrough: int | None  # next block when the block ends without a control transfer
    ends_in_break: bool = False


@dataclass
class Function:
    start: int
    end: int
    refused: str | None = None
    blocks: list[Block] | None = None


class Analysis:
    def __init__(self, img: Image, code_lo: int, code_hi: int):
        self.img, self.lo, self.hi = img, code_lo, code_hi
        self.stubs: set[int] = set()
        self.heads: set[int] = set()
        self.leaders: set[int] = set()
        self.functions: list[Function] = []
        self._run()

    def in_code(self, a: int) -> bool:
        return self.lo <= a < self.hi and not a & 3

    def is_stub(self, a: int) -> bool:
        """An IOP import stub: `jr ra` whose delay slot is `addiu zero,zero,ordinal` (the interpreter treats it as an HLE call)."""
        w, n = self.img.word(a), self.img.word(a + 4)
        return w == 0x03E00008 and n is not None and (n >> 16) == 0x2400

    def looks_like_head(self, a: int) -> bool:
        if not self.in_code(a):
            return False
        if a == self.lo:
            return True
        p = self.img.insn(a - 8)  # a jump, then its delay slot, then the head
        return p.name in ("jr", "j")

    def _run(self) -> None:
        img, lo, hi = self.img, self.lo, self.hi
        code = [img.insn(a) for a in range(lo, hi, 4)]
        self.stubs = {a for a in range(lo, hi, 4) if self.is_stub(a)}
        heads = {lo}
        leaders = {lo}
        for i in code:
            t = i.target
            if i.name == "jal" and t is not None and self.in_code(t):
                heads.add(t)
            if t is not None and self.in_code(t):
                leaders.add(t)
            if i.has_slot:
                leaders.add(i.pc + 8)
            if i.name == "jr" and i.rs == 31 and i.pc not in self.stubs:
                heads.add(i.pc + 8)
            if i.name == "break":
                leaders.add(i.pc + 4)
        # code pointers built by lui + addiu/ori (callbacks, vtable-less indirect calls)
        for k, i in enumerate(code):
            if i.name != "lui":
                continue
            for j in code[k + 1:k + 5]:
                if j.name in ("addiu", "ori") and j.rs == i.rt:
                    v = ((i.imm << 16) + (j.simm if j.name == "addiu" else j.imm)) & 0xFFFFFFFF
                    if self.looks_like_head(v):
                        heads.add(v)
                    break
        # code pointers stored in data (vtables, tables of callbacks) and jump tables
        a = hi
        while a < img.end:
            run = []
            while (w := img.word(a)) is not None and self.in_code(w) and a < img.end:
                run.append(w)
                a += 4
            if run:
                for w in run:
                    if self.looks_like_head(w):
                        heads.add(w)
                    elif len(run) >= 3:
                        leaders.add(w)  # jump-table entry in the middle of a function
            a += 4
        heads = {h for h in heads if lo <= h < hi}
        self.heads = heads
        self.leaders = {x for x in (leaders | heads) if lo <= x < hi} - self.stubs
        # An import stub is not translated; the word after it starts a fresh block.
        for s in self.stubs:
            if s + 8 < hi:
                self.leaders.add(s + 8)
        cuts = sorted(heads | self.stubs | {x + 8 for x in self.stubs if x + 8 < hi}) + [hi]
        for s, e in zip(cuts, cuts[1:]):
            if s in self.stubs:
                continue  # an import stub is the interpreter's (HLE), not translated
            f = Function(s, e)
            self.functions.append(f)
            self._check(f)
            if f.refused is None:
                f.blocks = self._blocks(f)

    def _check(self, f: Function) -> None:
        img = self.img
        slots = set()
        for pc in range(f.start, f.end, 4):
            if img.insn(pc).has_slot:
                slots.add(pc + 4)
        for pc in range(f.start, f.end, 4):
            i = img.insn(pc)
            if i.name in ("bad", "syscall"):
                f.refused = f"unsupported opcode {i.word:08x} at {pc:x}"
                return
            if pc in self.stubs:
                f.refused = f"import stub inside at {pc:x}"
                return
            if i.has_slot:
                s = img.insn(pc + 4)
                if s.has_slot or s.name in ("break", "bad", "syscall"):
                    f.refused = f"control transfer or exception in the delay slot at {pc:x}"
                    return
            if i.is_load and i.rt != 0:
                n = img.insn(pc + 4)
                if pc in slots:
                    f.refused = f"load in a delay slot at {pc:x}"
                elif pc + 4 >= f.end:
                    f.refused = f"load at the end of the function at {pc:x}"
                elif i.rt in reads(n):
                    f.refused = f"load-delay hazard at {pc:x}"
                # n writing the same register is fine: the load is discarded, as in the interpreter
                if f.refused:
                    return

    def _blocks(self, f: Function) -> list[Block]:
        img = self.img
        out = []
        for start in sorted(x for x in self.leaders if f.start <= x < f.end):
            pc, insns = start, []
            while True:
                i = img.insn(pc)
                if i.has_slot:
                    insns += [i, img.insn(pc + 4)]
                    out.append(Block(start, insns, None))
                    break
                insns.append(i)
                if i.name == "break":
                    out.append(Block(start, insns, None, True))
                    break
                pc += 4
                # A block never ends right after a load: the load is still pending at the boundary, so the
                # instruction after it (a leader with a block of its own) is copied into this block too.
                if (pc in self.leaders and not (i.is_load and i.rt)) or pc >= f.end:
                    out.append(Block(start, insns, pc))
                    break
        return out


# ----------------------------------------------------------------------------------------------------- emitter

def R(n: int) -> str:
    return f"r{n}" if n else "0u"


def hexu(v: int) -> str:
    return f"0x{v & 0xFFFFFFFF:x}u"


class Emitter:
    def __init__(self, an: Analysis, disasm: bool = True, ns: str = "adxi_native"):
        self.an = an
        self.ns = ns
        self.emitted: set[int] = set()
        for f in an.functions:
            if f.blocks:
                self.emitted.update(b.start for b in f.blocks)
        self.disasm = disasm
        self._rab = None
        if disasm:
            try:
                import rabbitizer
                self._rab = rabbitizer
            except ImportError:
                pass

    # -- small helpers ------------------------------------------------------------------------------------
    def comment(self, i: Insn) -> str:
        if self._rab is None:
            return f"/* {i.pc:x}: {i.word:08x} */"
        r = self._rab.Instruction(i.word, i.pc, self._rab.InstrCategory.CPU)
        return f"/* {i.pc:x}: {r.disassemble()} */"

    def jump(self, target: int) -> str:
        if target in self.emitted:
            return f"goto L_{target:x};"
        return f"{{ xpc = {hexu(target)}; goto fin; }}"

    def exit_at(self, i: Insn, ctx: dict) -> str:
        """Leave to the interpreter with pc at instruction i (not executed)."""
        p = [f"xpc = {hexu(i.pc)};"]
        give = ctx["N"] - ctx["k"]
        if give:
            p.append(f"left += {give}u;")
        if ctx.get("slot"):
            p.append(f"xbp = {ctx['slot'][0]}; xbt = {ctx['slot'][1]};")
        if ctx.get("pl"):
            rt = ctx["pl"]
            p.append(f"xplreg = {rt}u; xplval = r{rt}; r{rt} = plold;")
        p.append("goto fin;")
        return "{ " + " ".join(p) + " }"

    # -- one non-control instruction ----------------------------------------------------------------------
    def gen(self, i: Insn, ctx: dict) -> list[str]:
        n, a, b = i.name, R(i.rs), R(i.rt)
        ex = self.exit_at(i, ctx)
        d = R(i.rd)
        if n in ("sll", "srl", "sra", "sllv", "srlv", "srav", "addu", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                 "mfhi", "mflo"):
            e = {
                "sll": f"{b} << {i.sa}u", "srl": f"{b} >> {i.sa}u",
                "sra": f"(uint32_t)((int32_t){b} >> {i.sa})",
                "sllv": f"{b} << ({a} & 31u)", "srlv": f"{b} >> ({a} & 31u)",
                "srav": f"(uint32_t)((int32_t){b} >> ({a} & 31u))",
                "addu": f"{a} + {b}", "subu": f"{a} - {b}", "and": f"{a} & {b}", "or": f"{a} | {b}",
                "xor": f"{a} ^ {b}", "nor": f"~({a} | {b})",
                "slt": f"(uint32_t)((int32_t){a} < (int32_t){b})", "sltu": f"(uint32_t)({a} < {b})",
                "mfhi": "hi_", "mflo": "lo_",
            }[n]
            return [f"{d} = {e};"] if i.rd else []
        if n in ("addiu", "slti", "sltiu", "andi", "ori", "xori", "lui"):
            e = {
                "addiu": f"{a} + {hexu(i.simm)}",
                "slti": f"(uint32_t)((int32_t){a} < ({i.simm}))",
                "sltiu": f"(uint32_t)({a} < {hexu(i.simm)})",
                "andi": f"{a} & {hexu(i.imm)}", "ori": f"{a} | {hexu(i.imm)}", "xori": f"{a} ^ {hexu(i.imm)}",
                "lui": hexu(i.imm << 16),
            }[n]
            return [f"{R(i.rt)} = {e};"] if i.rt else []
        if n in ("add", "sub", "addi"):
            op = "__builtin_sub_overflow" if n == "sub" else "__builtin_add_overflow"
            rhs = f"(int32_t){b}" if n != "addi" else f"({i.simm})"
            dest = i.rd if n != "addi" else i.rt
            out = f"{{ int32_t t_; if ({op}((int32_t){a}, {rhs}, &t_)) {ex}"
            return [out + (f" {R(dest)} = (uint32_t)t_; }}" if dest else " }")]
        if n == "mthi":
            return [f"hi_ = {a};"]
        if n == "mtlo":
            return [f"lo_ = {a};"]
        if n == "mult":
            return [f"{{ int64_t p_ = (int64_t)(int32_t){a} * (int64_t)(int32_t){b}; lo_ = (uint32_t)p_; "
                    f"hi_ = (uint32_t)((uint64_t)p_ >> 32); }}"]
        if n == "multu":
            return [f"{{ uint64_t p_ = (uint64_t){a} * (uint64_t){b}; lo_ = (uint32_t)p_; hi_ = (uint32_t)(p_ >> 32); }}"]
        if n == "div":
            return [f"{{ int32_t n_ = (int32_t){a}, d_ = (int32_t){b}; "
                    f"if (d_ == 0 || (n_ == INT32_MIN && d_ == -1)) {ex} lo_ = (uint32_t)(n_ / d_); hi_ = (uint32_t)(n_ % d_); }}"]
        if n == "divu":
            return [f"{{ uint32_t n_ = {a}, d_ = {b}; if (d_ == 0) {ex} lo_ = n_ / d_; hi_ = n_ % d_; }}"]
        if n == "break":
            return [ex]
        if n in LOADS or n in STORES:
            size = {"lb": 1, "lbu": 1, "sb": 1, "lh": 2, "lhu": 2, "sh": 2, "lw": 4, "sw": 4}[n]
            g = [f"(pa_ >= kRamSize)"]
            if size > 1:
                g.insert(0, f"(a_ & {size - 1}u)")
            head = f"{{ uint32_t a_ = {a} + {hexu(i.simm)}; uint32_t pa_ = a_ & 0x1FFFFFFFu; if ({' || '.join(g)}) {ex}"
            if n in STORES:
                # The interpreter performs the store and the compiled code is stale from then on: stale() says so.
                head += f" if (pa_ - kCodeLo < kCodeSize) {{ g_stale = true; {ex[2:-2]} }}"
            if n in LOADS:
                if not i.rt:
                    return [head + " }"]
                v = {"lw": "ld32(ram, pa_)", "lbu": "(uint32_t)ram[pa_]",
                     "lb": "(uint32_t)(int32_t)(int8_t)ram[pa_]", "lhu": "(uint32_t)ld16(ram, pa_)",
                     "lh": "(uint32_t)(int32_t)(int16_t)ld16(ram, pa_)"}[n]
                pre = f" plold = r{i.rt};" if ctx.get("save_old") else ""
                return [head + f"{pre} r{i.rt} = {v}; }}"]
            w = {"sw": f"mem.write32(a_, {b});", "sh": f"mem.write16(a_, (uint16_t){b});",
                 "sb": f"mem.write8(a_, (uint8_t){b});"}[n]
            return [head + f" {w} }}"]
        raise ValueError(f"cannot translate {n}")

    # -- a block ------------------------------------------------------------------------------------------
    def block(self, b: Block, lines: list[str]) -> None:
        N = len(b.insns)
        lines.append(f"L_{b.start:x}: if (__builtin_expect(left < {N}u, 0)) {{ xpc = {hexu(b.start)}; goto fin; }} left -= {N}u;")
        body = b.insns
        ctrl = body[-2] if body[-1] is not None and len(body) >= 2 and body[-2].has_slot else None
        straight = body[:-2] if ctrl else body
        for k, i in enumerate(straight):
            prev = straight[k - 1] if k else None
            nxt = straight[k + 1] if k + 1 < len(straight) else (ctrl if ctrl else None)
            ctx = {"N": N, "k": k}
            if prev is not None and prev.is_load and prev.rt:
                ctx["pl"] = prev.rt
            # the old value of a load's register is needed only if the instruction after it can exit
            if i.is_load and i.rt and nxt is not None and (nxt.name in ("break", "div", "divu", "add", "addi", "sub") or
                                                           nxt.name in LOADS or nxt.name in STORES):
                ctx["save_old"] = True
            for s in self.gen(i, ctx):
                lines.append(f"  {s} {self.comment(i)}")
        if ctrl is None:
            if b.ends_in_break:
                return
            lines.append(f"  {self.jump(b.fallthrough)}")
            return
        slot = body[-1]
        k0 = len(straight)
        i = ctrl
        lines.append(f"  {{ {self.comment(i)}")
        n = i.name
        tgt = i.target
        # evaluate the branch exactly as the interpreter: link written first, then the condition/target read
        cond = None
        dyn = None
        if n == "jal":
            lines.append(f"  r31 = {hexu(i.pc + 8)};")
        elif n == "jalr":
            link = i.rd if i.rd else 31
            lines.append(f"  r{link} = {hexu(i.pc + 8)};")
            lines.append(f"  uint32_t jt_ = {R(i.rs)};")
            dyn = "jt_"
        elif n == "jr":
            lines.append(f"  uint32_t jt_ = {R(i.rs)};")
            dyn = "jt_"
        elif n in ("bltzal", "bgezal"):
            lines.append(f"  r31 = {hexu(i.pc + 8)};")
        if n in BRANCHES_COND:
            a, bb = R(i.rs), R(i.rt)
            c = {"beq": f"{a} == {bb}", "bne": f"{a} != {bb}", "blez": f"(int32_t){a} <= 0", "bgtz": f"(int32_t){a} > 0",
                 "bltz": f"(int32_t){a} < 0", "bgez": f"(int32_t){a} >= 0", "bltzal": f"(int32_t){a} < 0",
                 "bgezal": f"(int32_t){a} >= 0"}[n]
            lines.append(f"  bool c_ = {c};")
            cond = "c_"
        if cond:
            sctx = {"N": N, "k": k0 + 1, "slot": (cond, hexu(tgt))}
        elif dyn:
            sctx = {"N": N, "k": k0 + 1, "slot": ("true", dyn)}
        else:
            sctx = {"N": N, "k": k0 + 1, "slot": ("true", hexu(tgt))}
        for s in self.gen(slot, sctx):
            lines.append(f"  {s} {self.comment(slot)}")
        if cond:
            lines.append(f"  if ({cond}) {self.jump(tgt)}")
            lines.append(f"  {self.jump(i.pc + 8)}")
        elif dyn:
            lines.append(f"  xdyn = {dyn}; goto dispatch;")
        else:
            lines.append(f"  {self.jump(tgt)}")
        lines.append("  }")

    # -- whole file ---------------------------------------------------------------------------------------
    def emit(self) -> str:
        an = self.an
        img = an.img
        lo, hi = an.lo, an.hi
        h = 0xCBF29CE484222325
        for a in range(lo, hi, 4):
            for byte in struct.pack("<I", img.word(a)):
                h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        out: list[str] = []
        w = out.append
        w("// Generated by tools/iop_recomp/emit_adxi.py from a game module image. NOT TO BE COMMITTED (derived from game code).")
        w('#include "iop_cpu.h"')
        w('#include "iop_memory.h"')
        w("#include <cstdint>")
        w("#include <cstring>")
        w("")
        w(f"namespace ps2x::iop::detail::{self.ns}")
        w("{")
        w(f"constexpr uint32_t kCodeLo = {hexu(lo)};")
        w(f"constexpr uint32_t kCodeSize = {hexu(hi - lo)};")
        w(f"constexpr uint64_t kCodeHash = 0x{h:016x}ull; // FNV-1a of the code words, for the host's guard")
        w(f"constexpr uint32_t kRamSize = {hexu(RAM_SIZE)};")
        entries = sorted(self.emitted)
        w(f"extern const uint32_t kEntryPcs[{len(entries)}];")
        w(f"constexpr uint32_t kNumEntryPcs = {len(entries)}u;")
        w(f"const uint32_t kEntryPcs[{len(entries)}] = {{")
        for k in range(0, len(entries), 8):
            w("    " + ", ".join(hexu(e) for e in entries[k:k + 8]) + ",")
        w("};")
        w("")
        w("static inline uint32_t ld32(const uint8_t *ram, uint32_t pa) { uint32_t v; std::memcpy(&v, ram + pa, 4); return v; }")
        w("static inline uint16_t ld16(const uint8_t *ram, uint32_t pa) { uint16_t v; std::memcpy(&v, ram + pa, 2); return v; }")
        w("")
        w("static bool g_stale = false; // set when a store into the code range was handed to the interpreter")
        w("bool stale() { return g_stale; }")
        w("void resetStale() { g_stale = false; }")
        w("")
        w("uint32_t run(IopCpuState &cpu, IopMemory &mem, uint32_t budget)")
        w("{")
        w("    if (g_stale || cpu.branchPending || cpu.pendingLoad)")
        w("        return 0u;")
        w("    const uint8_t *ram = mem.ram().data();")
        w("    uint32_t left = budget;")
        w("    uint32_t xpc = 0u, xdyn = cpu.pc, xbt = 0u, xplreg = 0u, xplval = 0u, plold = 0u;")
        w("    bool xbp = false;")
        w("    uint32_t hi_ = cpu.hi, lo_ = cpu.lo;")
        for r in range(1, 32):
            w(f"    uint32_t r{r} = cpu.gpr[{r}];")
        w("    goto dispatch;")
        w("dispatch:")
        w("    switch (xdyn)")
        w("    {")
        for e in entries:
            w(f"    case {hexu(e)}: goto L_{e:x};")
        w("    default: xpc = xdyn; goto fin;")
        w("    }")
        nfun = nblk = ninsn = 0
        for f in an.functions:
            if not f.blocks:
                w(f"    // function {f.start:x}-{f.end:x}: NOT TRANSLATED ({f.refused})")
                continue
            nfun += 1
            w(f"    // function {f.start:x}-{f.end:x}")
            lines: list[str] = []
            for b in f.blocks:
                self.block(b, lines)
                nblk += 1
                ninsn += len(b.insns)
            out.extend("    " + l for l in lines)
        w("fin:")
        for r in range(1, 32):
            w(f"    cpu.gpr[{r}] = r{r};")
        w("    cpu.gpr[0] = 0u;")
        w("    cpu.hi = hi_;")
        w("    cpu.lo = lo_;")
        w("    cpu.pc = xpc;")
        w("    if (left != budget) { cpu.exception = false; cpu.yielded = false; }")
        w("    cpu.branchPending = xbp;")
        w("    if (xbp) cpu.branchTarget = xbt;")
        w("    if (xplreg) { cpu.pendingLoad = true; cpu.pendingLoadReg = xplreg; cpu.pendingLoadValue = xplval; }")
        w("    else cpu.pendingLoad = false;")
        w("    return budget - left;")
        w("}")
        w("}")
        self.stats = {"functions": nfun, "refused": sum(1 for f in an.functions if not f.blocks),
                      "blocks": nblk, "translated_instructions": ninsn, "entries": len(entries)}
        return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--image", default="work/iop-native/adxi.img")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x6E000)
    ap.add_argument("--code-lo", type=lambda s: int(s, 0), default=0x6E000)
    ap.add_argument("--code-hi", type=lambda s: int(s, 0), default=0x78500, help="exclusive end of the code (the IRX import table starts there)")
    ap.add_argument("--out", default="work/iop-recomp/adxi_native.cpp")
    ap.add_argument("--no-disasm", action="store_true")
    ap.add_argument("--namespace", default="adxi_native", help="C++ namespace (inside ps2x::iop::detail) of run()")
    args = ap.parse_args(argv)
    img = Image(Path(args.image).read_bytes(), args.base)
    an = Analysis(img, args.code_lo, args.code_hi)
    em = Emitter(an, disasm=not args.no_disasm, ns=args.namespace)
    text = em.emit()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"[iop-recomp] wrote {out} ({len(text)} bytes)", file=sys.stderr)
    print(f"functions: {len(an.functions)} (translated {em.stats['functions']}, refused {em.stats['refused']}), "
          f"blocks {em.stats['blocks']}, instructions {em.stats['translated_instructions']}, "
          f"entry points {em.stats['entries']}, import stubs {len(an.stubs)}")
    for f in an.functions:
        if f.refused:
            print(f"refused {f.start:x}-{f.end:x}: {f.refused}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
