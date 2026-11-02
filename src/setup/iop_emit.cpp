// IRX relocation, code analysis (functions, blocks) and the whole-file text of adxi_native.cpp.
// Port of tools/iop_recomp/make_native.py (relocate) and emit_adxi.py (Analysis, Emitter.emit); output byte for byte.
#include "iop_emit.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace don_setup {

namespace {

constexpr uint32_t R_MIPS_32 = 2, R_MIPS_26 = 4, R_MIPS_HI16 = 5, R_MIPS_LO16 = 6;
constexpr uint32_t SHT_PROGBITS = 1, SHT_REL = 9;
constexpr uint32_t kStubWord = 0x03E00008;  // jr ra; its delay slot is addiu zero,zero,<ordinal>
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull, kFnvPrime = 0x100000001B3ull;

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("module is truncated");
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
uint32_t rd16(const std::vector<uint8_t>& b, size_t at) {
    if (at + 2 > b.size()) throw std::runtime_error("module is truncated");
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8;
}
void wr32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    if (at + 4 > b.size()) throw std::runtime_error("relocation outside the image");
    std::memcpy(b.data() + at, &v, 4);
}

struct Section {
    uint32_t type, addr, offset, size, info;
};

std::string hex(const char* fmt, unsigned long long v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

}  // namespace

std::vector<uint8_t> relocate_irx(const std::vector<uint8_t>& irx, uint32_t base) {
    const uint32_t shoff = rd32(irx, 32);
    const uint32_t shentsize = rd16(irx, 46), shnum = rd16(irx, 48);
    std::vector<Section> secs;
    for (uint32_t i = 0; i < shnum; ++i) {
        const size_t o = size_t(shoff) + size_t(i) * shentsize;  // name, type, flags, addr, offset, size, link, info
        secs.push_back({rd32(irx, o + 4), rd32(irx, o + 12), rd32(irx, o + 16), rd32(irx, o + 20), rd32(irx, o + 28)});
    }
    std::vector<uint8_t> img(kIopImageSize, 0);
    for (const Section& s : secs) {
        if (s.type != SHT_PROGBITS || !s.size || !s.offset) continue;
        if (uint64_t(s.addr) + s.size > img.size() || uint64_t(s.offset) + s.size > irx.size())
            throw std::runtime_error("section outside the module");
        std::memcpy(img.data() + s.addr, irx.data() + s.offset, s.size);
    }
    auto word = [&](uint32_t a) { return rd32(img, a); };
    for (const Section& s : secs) {
        if (s.type != SHT_REL) continue;
        if (s.info >= secs.size()) throw std::runtime_error("relocation section without a target");
        const Section& target = secs[s.info];
        std::vector<uint32_t> pending_hi;
        for (uint32_t i = 0; i < s.size / 8; ++i) {
            const uint32_t off = rd32(irx, s.offset + size_t(i) * 8), info = rd32(irx, s.offset + size_t(i) * 8 + 4);
            const uint32_t kind = info & 0xFF, a = target.addr + off;
            uint32_t w = word(a);
            if (kind == R_MIPS_32) {
                w += base;
            } else if (kind == R_MIPS_26) {
                w = (w & 0xFC000000u) | (((w & 0x3FFFFFFu) + (base >> 2)) & 0x3FFFFFFu);
            } else if (kind == R_MIPS_HI16) {
                pending_hi.push_back(a);
                continue;
            } else if (kind == R_MIPS_LO16 && !pending_hi.empty()) {
                int64_t lo = w & 0xFFFF;
                if (lo & 0x8000) lo -= 0x10000;
                const int64_t full = (int64_t(word(pending_hi.back()) & 0xFFFF) << 16) + lo + base;
                const int64_t new_lo = full & 0xFFFF;
                const int64_t new_lo_signed = (new_lo & 0x8000) ? new_lo - 0x10000 : new_lo;
                const uint32_t new_hi = uint32_t(((full - new_lo_signed) >> 16) & 0xFFFF);
                for (uint32_t h : pending_hi) wr32(img, h, (word(h) & 0xFFFF0000u) | new_hi);
                pending_hi.clear();
                w = (w & 0xFFFF0000u) | uint32_t(new_lo);
            }
            wr32(img, a, w);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------------------------------- analysis

Analysis::Analysis(std::vector<uint8_t> image, uint32_t base_, uint32_t code_lo, uint32_t code_hi)
    : data(std::move(image)), base(base_), lo(code_lo), hi(code_hi) {
    run();
}

std::optional<uint32_t> Analysis::word(uint32_t a) const {
    const uint32_t o = a - base;
    if (a < base || uint64_t(o) + 4 > data.size() || (a & 3)) return std::nullopt;
    uint32_t v;
    std::memcpy(&v, data.data() + o, 4);
    return v;
}

Insn Analysis::insn(uint32_t a) const {
    const auto w = word(a);
    return decode(a, w ? *w : 0xFFFFFFFFu);
}

// An IOP import stub: `jr ra` whose delay slot is `addiu zero,zero,ordinal` (the interpreter treats it as an HLE call).
bool Analysis::is_stub(uint32_t a) const {
    const auto w = word(a), n = word(a + 4);
    return w && *w == kStubWord && n && (*n >> 16) == 0x2400;
}

bool Analysis::looks_like_head(uint32_t a) const {
    if (!in_code(a)) return false;
    if (a == lo) return true;
    const Insn p = insn(a - 8);  // a jump, then its delay slot, then the head
    return p.name == "jr" || p.name == "j";
}

void Analysis::run() {
    std::vector<Insn> code;
    for (uint32_t a = lo; a < hi; a += 4) code.push_back(insn(a));
    for (uint32_t a = lo; a < hi; a += 4)
        if (is_stub(a)) stubs.insert(a);
    std::set<uint32_t> hd = {lo}, ld = {lo};
    for (const Insn& i : code) {
        const auto t = i.target();
        if (i.name == "jal" && t && in_code(*t)) hd.insert(*t);
        if (t && in_code(*t)) ld.insert(*t);
        if (i.has_slot()) ld.insert(i.pc + 8);
        if (i.name == "jr" && i.rs == 31 && !stubs.count(i.pc)) hd.insert(i.pc + 8);
        if (i.name == "break") ld.insert(i.pc + 4);
    }
    // code pointers built by lui + addiu/ori (callbacks, vtable-less indirect calls)
    for (size_t k = 0; k < code.size(); ++k) {
        if (code[k].name != "lui") continue;
        for (size_t j = k + 1; j < std::min(code.size(), k + 5); ++j) {
            const Insn& n = code[j];
            if ((n.name == "addiu" || n.name == "ori") && n.rs == code[k].rt) {
                const uint32_t v = (code[k].imm << 16) + (n.name == "addiu" ? uint32_t(n.simm) : n.imm);
                if (looks_like_head(v)) hd.insert(v);
                break;
            }
        }
    }
    // code pointers stored in data (vtables, tables of callbacks) and jump tables
    for (uint32_t a = hi; a < end();) {
        std::vector<uint32_t> run_;
        for (auto w = word(a); w && in_code(*w) && a < end(); w = word(a)) {
            run_.push_back(*w);
            a += 4;
        }
        for (uint32_t w : run_) {
            if (looks_like_head(w)) hd.insert(w);
            else if (run_.size() >= 3) ld.insert(w);  // jump-table entry in the middle of a function
        }
        a += 4;
    }
    for (uint32_t h : hd)
        if (lo <= h && h < hi) heads.insert(h);
    for (uint32_t x : ld)
        if (lo <= x && x < hi && !stubs.count(x)) leaders.insert(x);
    for (uint32_t h : heads)
        if (!stubs.count(h)) leaders.insert(h);
    // An import stub is not translated; the word after it starts a fresh block.
    for (uint32_t s : stubs)
        if (s + 8 < hi) leaders.insert(s + 8);
    std::set<uint32_t> cut_set(heads);
    cut_set.insert(stubs.begin(), stubs.end());
    for (uint32_t s : stubs)
        if (s + 8 < hi) cut_set.insert(s + 8);
    std::vector<uint32_t> cuts(cut_set.begin(), cut_set.end());
    cuts.push_back(hi);
    for (size_t k = 0; k + 1 < cuts.size(); ++k) {
        if (stubs.count(cuts[k])) continue;  // an import stub is the interpreter's (HLE), not translated
        Function f;
        f.start = cuts[k];
        f.end = cuts[k + 1];
        check(f);
        if (!f.refused) f.blocks = blocks(f);
        functions.push_back(std::move(f));
    }
}

void Analysis::check(Function& f) const {
    std::set<uint32_t> slots;
    for (uint32_t pc = f.start; pc < f.end; pc += 4)
        if (insn(pc).has_slot()) slots.insert(pc + 4);
    for (uint32_t pc = f.start; pc < f.end; pc += 4) {
        const Insn i = insn(pc);
        if (i.name == "bad" || i.name == "syscall") {
            f.refused = "unsupported opcode " + hex("%08llx", i.word) + " at " + hex("%llx", pc);
            return;
        }
        if (stubs.count(pc)) {
            f.refused = "import stub inside at " + hex("%llx", pc);
            return;
        }
        if (i.has_slot()) {
            const Insn s = insn(pc + 4);
            if (s.has_slot() || s.name == "break" || s.name == "bad" || s.name == "syscall") {
                f.refused = "control transfer or exception in the delay slot at " + hex("%llx", pc);
                return;
            }
        }
        if (i.is_load() && i.rt != 0) {
            const Insn n = insn(pc + 4);
            if (slots.count(pc)) f.refused = "load in a delay slot at " + hex("%llx", pc);
            else if (pc + 4 >= f.end) f.refused = "load at the end of the function at " + hex("%llx", pc);
            else if (reads_mask(n) & (1u << i.rt)) f.refused = "load-delay hazard at " + hex("%llx", pc);
            // n writing the same register is fine: the load is discarded, as in the interpreter
            if (f.refused) return;
        }
    }
}

std::vector<Block> Analysis::blocks(const Function& f) const {
    std::vector<Block> out;
    for (auto it = leaders.lower_bound(f.start); it != leaders.end() && *it < f.end; ++it) {
        Block b;
        b.start = *it;
        uint32_t pc = b.start;
        for (;;) {
            const Insn i = insn(pc);
            if (i.has_slot()) {
                b.insns.push_back(i);
                b.insns.push_back(insn(pc + 4));
                break;
            }
            b.insns.push_back(i);
            if (i.name == "break") {
                b.ends_in_break = true;
                break;
            }
            pc += 4;
            // A block never ends right after a load: the load is still pending at the boundary, so the
            // instruction after it (a leader with a block of its own) is copied into this block too.
            if ((leaders.count(pc) && !(i.is_load() && i.rt)) || pc >= f.end) {
                b.fallthrough = pc;
                break;
            }
        }
        out.push_back(std::move(b));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- whole file

std::string emit_adxi_native(const std::vector<uint8_t>& image, uint32_t base, uint32_t lo, uint32_t hi,
                             IopEmitStats* stats) {
    const Analysis an(image, base, lo, hi);
    std::set<uint32_t> emitted;
    for (const Function& f : an.functions)
        for (const Block& b : f.blocks) emitted.insert(b.start);
    uint64_t h = kFnvOffset;
    for (uint32_t a = lo; a < hi; a += 4) {
        const auto w = an.word(a);
        if (!w) throw std::runtime_error("code range outside the image");
        for (int k = 0; k < 4; ++k) h = (h ^ ((*w >> (8 * k)) & 0xFF)) * kFnvPrime;
    }
    std::vector<std::string> out;
    auto w = [&](const std::string& s) { out.push_back(s); };
    const std::string ns = "adxi_native";
    w("// Generated by tools/iop_recomp/emit_adxi.py from a game module image. NOT TO BE COMMITTED (derived from game code).");
    w("#include \"iop_cpu.h\"");
    w("#include \"iop_memory.h\"");
    w("#include <cstdint>");
    w("#include <cstring>");
    w("");
    w("namespace ps2x::iop::detail::" + ns);
    w("{");
    w("constexpr uint32_t kCodeLo = " + hexu(lo) + ";");
    w("constexpr uint32_t kCodeSize = " + hexu(hi - lo) + ";");
    w("constexpr uint64_t kCodeHash = " + hex("0x%016llx", h) + "ull; // FNV-1a of the code words, for the host's guard");
    w("constexpr uint32_t kRamSize = " + hexu(kIopRamSize) + ";");
    const std::vector<uint32_t> entries(emitted.begin(), emitted.end());
    const std::string ne = std::to_string(entries.size());
    w("extern const uint32_t kEntryPcs[" + ne + "];");
    w("constexpr uint32_t kNumEntryPcs = " + ne + "u;");
    w("const uint32_t kEntryPcs[" + ne + "] = {");
    for (size_t k = 0; k < entries.size(); k += 8) {
        std::string line = "    ";
        for (size_t j = k; j < std::min(entries.size(), k + 8); ++j) line += (j > k ? ", " : "") + hexu(entries[j]);
        w(line + ",");
    }
    w("};");
    w("");
    w("static inline uint32_t ld32(const uint8_t *ram, uint32_t pa) { uint32_t v; std::memcpy(&v, ram + pa, 4); return v; }");
    w("static inline uint16_t ld16(const uint8_t *ram, uint32_t pa) { uint16_t v; std::memcpy(&v, ram + pa, 2); return v; }");
    w("");
    w("static bool g_stale = false; // set when a store into the code range was handed to the interpreter");
    w("bool stale() { return g_stale; }");
    w("void resetStale() { g_stale = false; }");
    w("");
    w("uint32_t run(IopCpuState &cpu, IopMemory &mem, uint32_t budget)");
    w("{");
    w("    if (g_stale || cpu.branchPending || cpu.pendingLoad)");
    w("        return 0u;");
    w("    const uint8_t *ram = mem.ram().data();");
    w("    uint32_t left = budget;");
    w("    uint32_t xpc = 0u, xdyn = cpu.pc, xbt = 0u, xplreg = 0u, xplval = 0u, plold = 0u;");
    w("    bool xbp = false;");
    w("    uint32_t hi_ = cpu.hi, lo_ = cpu.lo;");
    for (int r = 1; r < 32; ++r) w("    uint32_t r" + std::to_string(r) + " = cpu.gpr[" + std::to_string(r) + "];");
    w("    goto dispatch;");
    w("dispatch:");
    w("    switch (xdyn)");
    w("    {");
    for (uint32_t e : entries) w("    case " + hexu(e) + ": goto L_" + hex("%llx", e) + ";");
    w("    default: xpc = xdyn; goto fin;");
    w("    }");
    IopEmitStats st;
    for (const Function& f : an.functions) {
        const std::string range = hex("%llx", f.start) + "-" + hex("%llx", f.end);
        if (f.blocks.empty()) {
            w("    // function " + range + ": NOT TRANSLATED (" + (f.refused ? *f.refused : "None") + ")");
            ++st.refused;
            continue;
        }
        ++st.functions;
        w("    // function " + range);
        std::vector<std::string> lines;
        for (const Block& b : f.blocks) {
            emit_block(b, emitted, lines);
            ++st.blocks;
            st.translated_instructions += b.insns.size();
        }
        for (const std::string& l : lines) w("    " + l);
    }
    st.entries = entries.size();
    w("fin:");
    for (int r = 1; r < 32; ++r) w("    cpu.gpr[" + std::to_string(r) + "] = r" + std::to_string(r) + ";");
    w("    cpu.gpr[0] = 0u;");
    w("    cpu.hi = hi_;");
    w("    cpu.lo = lo_;");
    w("    cpu.pc = xpc;");
    w("    if (left != budget) { cpu.exception = false; cpu.yielded = false; }");
    w("    cpu.branchPending = xbp;");
    w("    if (xbp) cpu.branchTarget = xbt;");
    w("    if (xplreg) { cpu.pendingLoad = true; cpu.pendingLoadReg = xplreg; cpu.pendingLoadValue = xplval; }");
    w("    else cpu.pendingLoad = false;");
    w("    return budget - left;");
    w("}");
    w("}");
    if (stats) *stats = st;
    std::string text;
    for (const std::string& l : out) text += l + "\n";
    return text;
}

}  // namespace don_setup
