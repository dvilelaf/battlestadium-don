"""Tests of tools/iop_recomp/emit_adxi.py with synthetic instruction words only (no game data).

The unit tests look at what the analysis decides and at the text it emits. The differential test (skipped when no
g++ or no ps2xIOP sources are found) compiles the emitted code of random programs against the real IopCpuCore /
IopMemory and checks, for random budgets, that the native run leaves exactly the state the interpreter has after
the same number of instructions, including the exits (guards, pending loads, delay slots). Set DON_IOP_SRC to the
ps2xIOP directory (default: work/hygiene5/ps2recomp/ps2xIOP).
"""
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "iop_recomp"))
import emit_adxi as ea  # noqa: E402

BASE = 0x1000


# ---------------------------------------------------------------------------------------------- tiny assembler
def r_(rs, rt, rd, sa, fn):
    return rs << 21 | rt << 16 | rd << 11 | sa << 6 | fn


def i_(op, rs, rt, imm):
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF)


def j_(op, target):
    return op << 26 | ((target >> 2) & 0x3FFFFFF)


NOP = 0
JR_RA = r_(31, 0, 0, 0, 8)


def addiu(rt, rs, imm): return i_(9, rs, rt, imm)
def addu(rd, rs, rt): return r_(rs, rt, rd, 0, 0x21)
def lui(rt, imm): return i_(0xF, 0, rt, imm)
def lw(rt, off, rs): return i_(0x23, rs, rt, off)
def lbu(rt, off, rs): return i_(0x24, rs, rt, off)
def sw(rt, off, rs): return i_(0x2B, rs, rt, off)
def beq(rs, rt, off): return i_(4, rs, rt, off)
def bne(rs, rt, off): return i_(5, rs, rt, off)
def jal(t): return j_(3, t)
def jmp(t): return j_(2, t)
def jr(rs): return r_(rs, 0, 0, 0, 8)
def div(rs, rt): return r_(rs, rt, 0, 0, 0x1A)
def brk(): return 0x0D
def add(rd, rs, rt): return r_(rs, rt, rd, 0, 0x20)
def mfc0(rt, rd): return 0x40000000 | rt << 16 | rd << 11


def image(words, base=BASE):
    return ea.Image(b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in words), base)


def analyse(words, base=BASE, code_words=None):
    n = len(words) if code_words is None else code_words
    return ea.Analysis(image(words, base), base, base + 4 * n)


def emit(words, **kw):
    an = analyse(words, **kw)
    em = ea.Emitter(an, disasm=False)
    return an, em, em.emit()


# --------------------------------------------------------------------------------------------------- analysis
def test_straight_function_is_one_block():
    an, em, text = emit([addiu(2, 0, 1), JR_RA, NOP])
    assert [f.refused for f in an.functions] == [None]
    (blk,) = an.functions[0].blocks
    assert len(blk.insns) == 3  # addiu, jr, delay slot
    assert "left < 3u" in text
    assert "goto dispatch" in text and "r2 = 0u + 0x1u;" in text


def test_branch_and_slot_cost_two_and_both_paths():
    words = [beq(4, 5, 2), addiu(2, 0, 1), addiu(2, 0, 2), addiu(3, 0, 3), JR_RA, NOP]
    an, em, text = emit(words)
    blocks = {b.start: b for b in an.functions[0].blocks}
    assert len(blocks[BASE].insns) == 2  # beq + slot
    assert BASE + 8 in blocks and BASE + 12 in blocks  # fall-through and target are block entries
    assert "if (c_) goto L_100c;" in text
    assert "goto L_1008;" in text


def test_jal_targets_are_function_heads_and_return_points_are_entries():
    words = [jal(BASE + 16), NOP, JR_RA, NOP, addiu(2, 0, 7), JR_RA, NOP]
    an, em, text = emit(words)
    assert BASE + 16 in an.heads
    assert BASE + 8 in em.emitted  # the return point of the jal
    assert "r31 = 0x1008u;" in text
    assert "case 0x1008u: goto L_1008;" in text


def test_data_pointer_after_a_jump_is_a_head_and_stray_pointer_is_not():
    code = [addiu(2, 0, 1), JR_RA, NOP, addiu(2, 0, 2), JR_RA, NOP, addiu(2, 0, 3), JR_RA, NOP]
    data = [BASE + 12, BASE + 4]  # the first follows `jr; nop`: a head, the second is the middle of a function
    an = ea.Analysis(image(code + data), BASE, BASE + 4 * len(code))
    assert BASE + 12 in an.heads
    assert BASE + 4 not in an.heads


def test_jump_table_entries_become_leaders():
    code = [NOP] * 12
    table = [BASE + 8, BASE + 20, BASE + 32]
    an = ea.Analysis(image(code + table), BASE, BASE + 48)
    assert {BASE + 8, BASE + 20, BASE + 32} <= an.leaders


def test_import_stub_is_not_translated():
    words = [jal(BASE + 16), NOP, JR_RA, NOP, JR_RA, addiu(0, 0, 5), NOP]
    an, em, text = emit(words)
    assert BASE + 16 in an.stubs
    assert BASE + 16 not in em.emitted
    assert all(f.refused is None for f in an.functions)
    assert "xpc = 0x1010u; goto fin;" in text  # jal to the stub leaves with ra set and pc at the stub


# ------------------------------------------------------------------------------------------------- refusals
@pytest.mark.parametrize("words, why", [
    ([lw(2, 0, 4), addu(3, 2, 2), JR_RA, NOP], "hazard"),
    ([beq(0, 0, 1), lw(2, 0, 4), JR_RA, NOP], "delay slot"),
    ([beq(0, 0, 1), jmp(BASE), JR_RA, NOP], "delay slot"),
    ([beq(0, 0, 1), brk(), JR_RA, NOP], "delay slot"),
    ([mfc0(2, 12), JR_RA, NOP], "unsupported"),
    ([0x0000000C, JR_RA, NOP], "unsupported"),  # syscall
    ([JR_RA, NOP, lw(2, 0, 4)], "end of the function"),
])
def test_function_refusals(words, why):
    an = analyse(words)
    assert why in " ".join(f.refused or "" for f in an.functions)


def test_refused_function_is_not_emitted_and_jumps_to_it_exit():
    words = [jal(BASE + 12), NOP, JR_RA, lw(2, 0, 4), addu(3, 2, 2), JR_RA, NOP]
    an, em, text = emit(words)
    assert any(f.refused for f in an.functions)
    assert BASE + 12 not in em.emitted
    assert "xpc = 0x100cu; goto fin;" in text


def test_load_followed_by_overwrite_is_not_a_hazard():
    an = analyse([lw(2, 0, 4), addiu(2, 0, 1), JR_RA, NOP])
    assert all(f.refused is None for f in an.functions)


def test_load_before_a_branch_target_extends_the_block():
    # beq target = the jr; the lw falls into it: the block of the lw also carries jr + slot (3 words + lw = 4)
    words = [beq(4, 0, 2), NOP, lw(2, 0, 5), JR_RA, NOP]
    an, em, text = emit(words)
    assert all(f.refused is None for f in an.functions)
    blk = {b.start: b for b in an.functions[0].blocks}
    assert [i.name for i in blk[BASE + 8].insns] == ["lw", "jr", "sll"]
    assert [i.name for i in blk[BASE + 12].insns] == ["jr", "sll"]


# ------------------------------------------------------------------------------------------------- exits
def test_exit_conditions_are_in_the_text():
    words = [div(4, 5), lw(2, 0, 6), sw(3, 4, 6), brk(), JR_RA, NOP]
    _, _, text = emit(words)
    assert "d_ == 0 || (n_ == INT32_MIN && d_ == -1)" in text
    assert "(a_ & 3u) || (pa_ >= kRamSize)" in text
    assert "g_stale = true;" in text  # only on the store
    assert text.count("(pa_ - kCodeLo < kCodeSize)") == 1
    assert "xpc = 0x100cu;" in text  # the break leaves with pc at the break
    assert "add" not in text.split("run(")[1].split("fin:")[0].replace("__builtin_add", "")  # no overflow guard for addu/lw


def test_overflow_ops_guard():
    _, _, text = emit([add(2, 4, 5), JR_RA, NOP])
    assert "__builtin_add_overflow" in text


def test_slot_exit_carries_the_branch_and_load_exit_carries_the_pending_load():
    words = [lw(2, 0, 6), sw(2, 4, 6) if False else sw(3, 4, 6), beq(4, 5, 2), sw(3, 8, 6), JR_RA, NOP, NOP]
    _, _, text = emit(words)
    assert "xplreg = 2u; xplval = r2; r2 = plold;" in text  # exit at the store that follows the load
    assert "plold = r2;" in text
    assert "xbp = c_; xbt = 0x1014u;" in text  # exit in the delay slot of the beq


def test_cost_given_back_on_an_exit_in_the_middle_of_a_block():
    words = [addiu(2, 0, 1), div(4, 5), addiu(3, 0, 1), JR_RA, NOP]
    _, _, text = emit(words)
    # block cost 5; the div is the 2nd instruction, 1 executed before it: 4 given back
    assert "xpc = 0x1004u; left += 4u;" in text


def test_code_hash_and_entry_table():
    _, em, text = emit([addiu(2, 0, 1), JR_RA, NOP])
    assert "kCodeHash = 0x" in text and "kNumEntryPcs = 1u" in text and "0x1000u," in text


# ---------------------------------------------------------------------------------------- differential test
IOP_SRC = Path(os.environ.get("DON_IOP_SRC", ROOT / "work/hygiene5/ps2recomp/ps2xIOP"))
CORE = IOP_SRC / "src/emulator/core"
needs_cpp = pytest.mark.skipif(shutil.which("g++") is None or not (CORE / "iop_cpu.h").exists(),
                               reason="needs g++ and the ps2xIOP sources (DON_IOP_SRC)")


@needs_cpp
def test_emitted_code_compiles_syntax_only(tmp_path):
    _, _, text = emit([addiu(2, 0, 1), lw(3, 0, 4), sw(3, 4, 4), div(4, 5), beq(2, 3, -4), NOP, JR_RA, NOP])
    f = tmp_path / "p.cpp"
    f.write_text(text)
    subprocess.run(["g++", "-std=c++20", "-O2", "-fsyntax-only", "-Wall", f"-I{CORE}", f"-I{IOP_SRC / 'include'}", str(f)],
                   check=True)


def gen_program(rng, n):
    """Random hazard-free MIPS I code. Registers 1-15 are data, 16-19 pointers (RAM, misaligned, hardware, code)."""
    W: list[int] = []
    fix: list[tuple[int, str, int]] = []  # (index, kind, target index)

    def reg(avoid, lo=0):
        return rng.choice([r for r in range(lo, 16) if r not in avoid])

    def ptr():
        return rng.choices([16, 17, 18, 19], [10, 1, 1, 1])[0]

    def off(size):
        return rng.choice([0, 4, 8, 12, 64]) + (rng.choice([0, 0, 0, 1]) if size == 1 else rng.choice([0, 0, 0, 2]) if size == 2 else 0)

    def alu(avoid):
        k = rng.randrange(22)
        a, b, d = reg(avoid), reg(avoid), reg(set())
        imm = rng.choice([0, 1, -1, 0x7FFF, -0x8000, rng.randrange(0x10000)])
        fn = {0: 0x21, 1: 0x23, 2: 0x24, 3: 0x25, 4: 0x26, 5: 0x27, 6: 0x2A, 7: 0x2B}
        if k < 8:
            return r_(a, b, d, 0, fn[k])
        if k < 12:
            return r_(0, b, d, rng.randrange(32), [0, 2, 3, 0][k - 8])
        if k < 15:
            return r_(a, b, d, 0, [4, 6, 7][k - 12])
        if k < 20:
            return i_([9, 0xA, 0xB, 0xC, 0xD][k - 15], a, d, imm)
        if k == 20:
            return i_(0xE, a, d, imm)
        return i_(0xF, 0, d, imm)

    def hilo(avoid):
        k = rng.randrange(8)
        a, b, d = reg(avoid), reg(avoid), reg(set())
        return [r_(a, b, 0, 0, 0x18), r_(a, b, 0, 0, 0x19), r_(a, b, 0, 0, 0x1A), r_(a, b, 0, 0, 0x1B),
                r_(0, 0, d, 0, 0x10), r_(0, 0, d, 0, 0x12), r_(a, 0, 0, 0, 0x11), r_(a, 0, 0, 0, 0x13)][k]

    def ovf(avoid):
        a, b, d = reg(avoid), reg(avoid), reg(set())
        return rng.choice([r_(a, b, d, 0, 0x20), r_(a, b, d, 0, 0x22), i_(8, a, d, rng.choice([1, -1, 0x7FFF]))])

    def load(avoid):
        op = rng.choice([0x20, 0x21, 0x23, 0x24, 0x25, 0x23])
        size = {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4}[op]
        rt = reg(set(), 0) if rng.random() < 0.9 else 0
        return i_(op, ptr(), rt, off(size)), rt

    def store(avoid):
        op = rng.choice([0x28, 0x29, 0x2B, 0x2B])
        size = {0x28: 1, 0x29: 2, 0x2B: 4}[op]
        return i_(op, ptr(), reg(avoid), off(size))

    avoid: set = set()
    while len(W) < n:
        idx = len(W)
        kind = rng.choices(["alu", "hilo", "ovf", "load", "store", "br", "jal", "j", "jr", "jalr", "brk", "nop"],
                           [30, 6, 2, 12, 9, 7, 1, 1, 1, 1, 1, 2])[0]
        av, avoid = avoid, set()
        if kind == "alu":
            W.append(alu(av))
        elif kind == "hilo":
            W.append(hilo(av))
        elif kind == "ovf":
            W.append(ovf(av))
        elif kind == "load":
            w, rt = load(av)
            W.append(w)
            if rt:
                avoid = {rt}
        elif kind == "store":
            W.append(store(av))
        elif kind == "br":
            a, b = reg(av), reg(av)
            op = rng.choice(["beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"])
            enc = {"beq": (4, b), "bne": (5, b), "blez": (6, 0), "bgtz": (7, 0), "bltz": (1, 0), "bgez": (1, 1),
                   "bltzal": (1, 0x10), "bgezal": (1, 0x11)}[op]
            rt = enc[1] if enc[0] in (4, 5, 1) else 0
            W.append(i_(enc[0], a, rt, 0))
            fix.append((idx, "b", rng.randrange(n)))
            W.append(alu(set()) if rng.random() < 0.8 else NOP)
        elif kind in ("jal", "j"):
            W.append(0)
            fix.append((idx, kind, rng.randrange(n)))
            W.append(alu(set()))
        elif kind == "jr":
            W.append(jr(rng.choice([20, 21, 31])))
            W.append(alu(set()))
        elif kind == "jalr":
            W.append(r_(rng.choice([20, 21]), 0, rng.choice([31, 22, 20]), 0, 9))
            W.append(alu(set()))
        elif kind == "brk":
            W.append(brk())
        else:
            W.append(NOP)
    W = W[:n]
    while len(W) < n:
        W.append(NOP)
    W[-2:] = [NOP, NOP]
    for idx, k, t in fix:
        if idx + 1 >= n:
            continue
        if k == "b":
            W[idx] |= (t - (idx + 1)) & 0xFFFF
        else:
            W[idx] = j_(3 if k == "jal" else 2, BASE + 4 * t)
    return W


HARNESS = r"""
#include "iop_cpu.h"
#include "iop_memory.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
%(includes)s
using namespace ps2x::iop::detail;
struct Prog { const uint32_t *w; uint32_t n; uint32_t (*run)(IopCpuState &, IopMemory &, uint32_t); void (*reset)(); };
%(progs)s
static const uint32_t kInit[%(nseeds)d][32] = { %(inits)s };
static const uint32_t kData[%(nseeds)d][64] = { %(datas)s };
static const uint32_t kBudgets[] = { 1, 2, 3, 5, 8, 13, 40, 1000, 1000, 200, 1000, 7, 1000, 90, 1000, 1000 };
static const uint32_t kBase = %(base)du;

static bool same(const IopCpuState &a, const IopCpuState &b, const IopMemory &ma, const IopMemory &mb, const char *what, int p, int s, uint32_t step)
{
    bool ok = true;
    auto bad = [&](const char *f) { std::printf("MISMATCH %%s prog %%d seed %%d step %%u: %%s\n", what, p, s, step, f); ok = false; };
    for (int r = 0; r < 32; ++r) if (a.gpr[r] != b.gpr[r]) { std::printf("gpr[%%d] %%08x vs %%08x\n", r, a.gpr[r], b.gpr[r]); bad("gpr"); }
    if (a.hi != b.hi) bad("hi");
    if (a.lo != b.lo) bad("lo");
    if (a.pc != b.pc) { std::printf("pc %%x vs %%x\n", a.pc, b.pc); bad("pc"); }
    if (a.branchPending != b.branchPending) bad("branchPending");
    if (a.branchPending && a.branchTarget != b.branchTarget) bad("branchTarget");
    if (a.pendingLoad != b.pendingLoad) bad("pendingLoad");
    if (a.pendingLoad && (a.pendingLoadReg != b.pendingLoadReg || a.pendingLoadValue != b.pendingLoadValue)) bad("pendingLoad value");
    if (a.exception != b.exception || a.yielded != b.yielded) bad("flags");
    // the program, its data window and the stack-like region the tests never touch
    if (std::memcmp(ma.ram().data() + kBase, mb.ram().data() + kBase, 0x400) != 0) bad("ram (code)");
    if (std::memcmp(ma.ram().data() + 0x20000, mb.ram().data() + 0x20000, 0x400) != 0) bad("ram (data)");
    return ok;
}

int main()
{
    uint64_t nativeInsns = 0, exits = 0, runs = 0;
    for (int p = 0; p < %(nprogs)d; ++p)
        for (int s = 0; s < %(nseeds)d; ++s)
        {
            IopMemory mA, mB;
            IopCpuCore cA(mA), cB(mB);
            IopCpuState sA{}, sB{};
            const uint32_t lo = kBase, hi = kBase + progs[p].n * 4;
            progs[p].reset();
            for (IopMemory *m : { &mA, &mB })
            {
                (void)m->writeRam(lo, progs[p].w, progs[p].n * 4);
                (void)m->writeRam(0x20000, kData[s], sizeof(kData[s]));
            }
            for (int r = 1; r < 32; ++r) sA.gpr[r] = sB.gpr[r] = kInit[s][r];
            sA.pc = sB.pc = lo;
            uint32_t step = 0;
            while (step < 4000 && sA.pc >= lo && sA.pc < hi)
            {
                // the host's guard: native code is only used while the code range holds the translated words
                if (std::memcmp(mA.ram().data() + lo, progs[p].w, progs[p].n * 4) != 0) break;
                const uint32_t budget = kBudgets[(step + s + p) %% 16];
                const IopCpuState before = sA;
                const uint32_t n = progs[p].run(sA, mA, budget);
                ++runs;
                if (n > budget) { std::printf("MISMATCH over budget prog %%d\n", p); return 1; }
                nativeInsns += n;
                if (n == 0 && (before.branchPending || before.pendingLoad || false)) {}
                for (uint32_t k = 0; k < n; ++k) (void)cB.executeInstruction(sB);
                if (n && !same(sA, sB, mA, mB, "after run", p, s, step)) return 1;
                if (n == 0) { /* the interpreter must not have been able to continue natively: state untouched */
                    if (!same(sA, before, mA, mA, "untouched", p, s, step)) return 1;
                }
                step += n;
                ++exits;
                if (sA.pc < lo || sA.pc >= hi) break;
                (void)cA.executeInstruction(sA);
                (void)cB.executeInstruction(sB);
                ++step;
                if (!same(sA, sB, mA, mB, "after interpreter step", p, s, step)) return 1;
            }
        }
    std::printf("OK runs=%%llu native_insns=%%llu exits=%%llu\n", (unsigned long long)runs, (unsigned long long)nativeInsns, (unsigned long long)exits);
    return 0;
}
"""


@pytest.fixture(scope="session")
def iop_objs(tmp_path_factory):
    d = tmp_path_factory.mktemp("iop_objs")
    objs = []
    for f in ("iop_cpu", "iop_memory"):
        o = d / f"{f}.o"
        subprocess.run(["g++", "-std=c++20", "-O1", "-c", f"-I{CORE}", f"-I{IOP_SRC / 'include'}", str(CORE / f"{f}.cpp"), "-o", str(o)],
                       check=True)
        objs.append(o)
    return objs


@needs_cpp
@pytest.mark.parametrize("seed", range(1, 1 + int(os.environ.get("DON_IOP_FUZZ", "3"))))
def test_native_matches_interpreter_on_random_programs(tmp_path, iop_objs, seed):
    rng = random.Random(seed)
    nprogs, nseeds, n = 6, 6, 70
    progs = []
    while len(progs) < nprogs:
        words = gen_program(rng, n)
        an = ea.Analysis(image(words), BASE, BASE + 4 * n)
        refused = [f for f in an.functions if f.refused]
        if refused:
            # a generator hole (hazard created by a jump into the middle of a pair is possible): try another
            continue
        progs.append(words)
    includes, plist = [], []
    for k, words in enumerate(progs):
        an = ea.Analysis(image(words), BASE, BASE + 4 * n)
        em = ea.Emitter(an, disasm=False, ns=f"p{k}")
        (tmp_path / f"p{k}.cpp").write_text(em.emit())
        includes.append(f'#include "{tmp_path / f"p{k}.cpp"}"')
        plist.append((k, words))
    progs_txt = "\n".join(f"static const uint32_t w{k}[] = {{{', '.join(hex(x) + 'u' for x in w)}}};" for k, w in plist)
    progs_txt += "\nstatic const Prog progs[] = {\n" + ",\n".join(
        f"  {{w{k}, {n}, &ps2x::iop::detail::p{k}::run, &ps2x::iop::detail::p{k}::resetStale}}" for k, _ in plist) + "\n};"
    edge = [0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 2, 0xFFFFFFFE, 0x20000, 0x12345678]
    inits, datas = [], []
    for _ in range(nseeds):
        regs = [0] + [rng.choice(edge) if rng.random() < 0.6 else rng.getrandbits(32) for _ in range(15)]
        regs += [rng.choice([0x20000, 0x80020000, 0xA0020000]) + rng.choice([0, 0x10, 0x20]), 0x20001, 0x1F801070, BASE + 0x40,
                 BASE + 4 * (n // 2), BASE + 12] + [rng.getrandbits(32) for _ in range(9)] + [BASE + 4 * rng.randrange(n)]
        assert len(regs) == 32
        inits.append("{" + ", ".join(f"{x}u" for x in regs) + "}")
        datas.append("{" + ", ".join(f"{rng.getrandbits(32)}u" for _ in range(64)) + "}")
    src = HARNESS % dict(includes="\n".join(includes), progs=progs_txt, nseeds=nseeds, nprogs=nprogs,
                         inits=", ".join(inits), datas=", ".join(datas), base=BASE)
    main = tmp_path / "harness.cpp"
    main.write_text(src)
    exe = tmp_path / "harness"
    subprocess.run(["g++", "-std=c++20", "-O1", f"-I{CORE}", f"-I{IOP_SRC / 'include'}", str(main), *map(str, iop_objs), "-o", str(exe)],
                   check=True)
    res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    assert res.returncode == 0, res.stdout[-2000:]
    assert res.stdout.startswith("OK"), res.stdout[-2000:]
    native = int(res.stdout.split("native_insns=")[1].split()[0])
    assert native > 500, res.stdout  # the native code really ran, not only exited at once


# --------------------------------------------------------------------------- through the real emulator (patch 0085)
# Needs the ps2xIOP sources with patch 0085 applied and their static library built (cmake, PS2X_IOP_BUILD_TESTS=ON):
# DON_IOP_NATIVE_SRC = the ps2xIOP directory, DON_IOP_NATIVE_LIB = the libps2_iop.a built from it.
NATIVE_SRC = Path(os.environ.get("DON_IOP_NATIVE_SRC", ROOT / "work/iopnative/ps2recomp/ps2xIOP"))
NATIVE_LIB = Path(os.environ.get("DON_IOP_NATIVE_LIB", ROOT / "work/iopnative/build-iop/libps2_iop.a"))
needs_native = pytest.mark.skipif(
    shutil.which("g++") is None or not (NATIVE_SRC / "include/ps2x/iop/iop_native_module.h").exists() or not NATIVE_LIB.exists(),
    reason="needs g++, ps2xIOP with patch 0085 (DON_IOP_NATIVE_SRC) and its built library (DON_IOP_NATIVE_LIB)")

EMULATOR_HARNESS = r"""
#include "iop_compat_test_support.h"
#include "ps2x/iop/iop_native_module.h"
#include "%(gen)s"
#include <cstdio>
#include <cstdlib>
using namespace ps2x::iop;
namespace g = ps2x::iop::detail::mod0;
static void run(const char *label, bool native, bool verify)
{
    if (native) setenv("DON_IOP_NATIVE", "1", 1); else setenv("DON_IOP_NATIVE", "0", 1);
    if (verify) setenv("DON_IOP_NATIVE_VERIFY", "1", 1); else unsetenv("DON_IOP_NATIVE_VERIFY");
    iop_test::Host host;
    IopSubsystem iop(host);
    iop_test::Irx image;
    image.words(0u, {%(words)s});
    image.install(host);
    const ModuleLoadResult r = iop.loadModuleBuffer(0x1000u);
    const IopNativeStats s = nativeModuleStats();
    std::printf("%%s start=%%d instructions=%%llu native_calls=%%llu native_instructions=%%llu diffs=%%llu\n", label,
                (int)r.startResult, (unsigned long long)iop.debugSnapshot().emulatorInstructions,
                (unsigned long long)s.calls, (unsigned long long)s.instructions, (unsigned long long)s.verifyDifferences);
}
int main()
{
    static IopNativeModule m;
    m.name = "synthetic"; m.codeLo = g::kCodeLo; m.codeSize = g::kCodeSize; m.codeHash = g::kCodeHash;
    m.entryPcs = g::kEntryPcs; m.numEntryPcs = g::kNumEntryPcs; m.run = &g::run; m.stale = &g::stale; m.resetStale = &g::resetStale;
    registerNativeModule(&m);
    run("interp", false, false);
    run("native", true, false);
    run("verify", true, true);
    return 0;
}
"""


@needs_native
def test_generated_module_through_the_emulator_matches_the_interpreter(tmp_path):
    n = 2000
    words = [addiu(2, 0, 0), addiu(8, 0, n), addu(2, 2, 8), addiu(8, 8, -1), bne(8, 0, -3), NOP, JR_RA, NOP]
    base = 0x10000
    an = ea.Analysis(image(words, base), base, base + 4 * len(words))
    em = ea.Emitter(an, disasm=False, ns="mod0")
    (tmp_path / "gen.cpp").write_text(em.emit())
    src = EMULATOR_HARNESS % dict(gen=tmp_path / "gen.cpp", words=", ".join(hex(w & 0xFFFFFFFF) + "u" for w in words))
    main = tmp_path / "harness.cpp"
    main.write_text(src)
    exe = tmp_path / "harness"
    subprocess.run(["g++", "-std=c++20", "-O1", f"-I{NATIVE_SRC / 'tests'}", f"-I{NATIVE_SRC / 'include'}", f"-I{NATIVE_SRC / 'src'}",
                    f"-I{NATIVE_SRC / 'src/emulator/core'}", str(main), str(NATIVE_LIB), "-o", str(exe)], check=True)
    res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr[-2000:]
    rows = {}
    for line in res.stdout.splitlines():
        label, *kv = line.split()
        rows[label] = dict(x.split("=") for x in kv)
    assert int(rows["interp"]["start"]) == n * (n + 1) // 2
    assert rows["interp"]["native_calls"] == "0"
    for label in ("native", "verify"):
        assert rows[label]["start"] == rows["interp"]["start"], res.stdout
        assert rows[label]["instructions"] == rows["interp"]["instructions"], res.stdout
        assert int(rows[label]["native_instructions"]) > 3 * n, res.stdout  # the loop ran natively
    assert rows["verify"]["diffs"] == "0", res.stdout
