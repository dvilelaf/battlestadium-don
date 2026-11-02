// Decoding of the R3000 subset the IOP emitter translates, and the C++ text of one instruction / one basic block.
// Port of tools/iop_recomp/emit_adxi.py (decode, reads, Emitter.gen, Emitter.block); the text is byte for byte its.
#include <cstdio>
#include <map>

#include "iop_emit.h"

namespace don_setup {

namespace {

const std::map<unsigned, std::string> kSpecial = {
    {0x00, "sll"},  {0x02, "srl"},  {0x03, "sra"},  {0x04, "sllv"}, {0x06, "srlv"}, {0x07, "srav"}, {0x08, "jr"},
    {0x09, "jalr"}, {0x0C, "syscall"}, {0x0D, "break"}, {0x10, "mfhi"}, {0x11, "mthi"}, {0x12, "mflo"},
    {0x13, "mtlo"}, {0x18, "mult"}, {0x19, "multu"}, {0x1A, "div"},  {0x1B, "divu"}, {0x20, "add"},  {0x21, "addu"},
    {0x22, "sub"},  {0x23, "subu"}, {0x24, "and"},  {0x25, "or"},   {0x26, "xor"},  {0x27, "nor"},  {0x2A, "slt"},
    {0x2B, "sltu"}};
const std::map<unsigned, std::string> kRegimm = {{0x00, "bltz"}, {0x01, "bgez"}, {0x10, "bltzal"}, {0x11, "bgezal"}};
const std::map<unsigned, std::string> kOpcodes = {
    {0x02, "j"},    {0x03, "jal"},  {0x04, "beq"},  {0x05, "bne"},  {0x06, "blez"}, {0x07, "bgtz"}, {0x08, "addi"},
    {0x09, "addiu"}, {0x0A, "slti"}, {0x0B, "sltiu"}, {0x0C, "andi"}, {0x0D, "ori"},  {0x0E, "xori"}, {0x0F, "lui"},
    {0x20, "lb"},   {0x21, "lh"},   {0x23, "lw"},   {0x24, "lbu"},  {0x25, "lhu"},  {0x28, "sb"},   {0x29, "sh"},
    {0x2B, "sw"}};

bool one_of(const std::string& n, std::initializer_list<const char*> names) {
    for (const char* s : names)
        if (n == s) return true;
    return false;
}
bool is_cond_branch(const std::string& n) {
    return one_of(n, {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"});
}
bool is_load_name(const std::string& n) { return one_of(n, {"lb", "lbu", "lh", "lhu", "lw"}); }
bool is_store_name(const std::string& n) { return one_of(n, {"sb", "sh", "sw"}); }

std::string R(uint32_t n) { return n ? "r" + std::to_string(n) : "0u"; }
std::string num(long long v) { return std::to_string(v); }

}  // namespace

bool Insn::is_load() const { return is_load_name(name); }
bool Insn::has_slot() const { return is_cond_branch(name) || one_of(name, {"j", "jal", "jr", "jalr"}); }

std::optional<uint32_t> Insn::target() const {
    if (is_cond_branch(name)) return uint32_t(pc + 4 + (uint32_t(simm) << 2));
    if (name == "j" || name == "jal") return ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
    return std::nullopt;
}

Insn decode(uint32_t pc, uint32_t word) {
    Insn i;
    i.pc = pc;
    i.word = word;
    const unsigned op = word >> 26;
    i.rs = (word >> 21) & 31;
    i.rt = (word >> 16) & 31;
    i.rd = (word >> 11) & 31;
    i.sa = (word >> 6) & 31;
    i.imm = word & 0xFFFF;
    i.simm = (i.imm & 0x8000) ? int32_t(i.imm) - 0x10000 : int32_t(i.imm);
    const std::map<unsigned, std::string>& table = op == 0 ? kSpecial : op == 1 ? kRegimm : kOpcodes;
    const unsigned key = op == 0 ? (word & 63) : op == 1 ? i.rt : op;
    const auto it = table.find(key);
    i.name = it == table.end() ? "bad" : it->second;
    return i;
}

uint32_t reads_mask(const Insn& i) {
    const std::string& n = i.name;
    uint32_t r = 0;
    const uint32_t rs = 1u << i.rs, rt = 1u << i.rt;
    if (one_of(n, {"sll", "srl", "sra"})) r = rt;
    else if (one_of(n, {"sllv", "srlv", "srav", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                        "mult", "multu", "div", "divu", "beq", "bne"}))
        r = rs | rt;
    else if (one_of(n, {"jr", "jalr", "mthi", "mtlo", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "addi",
                        "addiu", "slti", "sltiu", "andi", "ori", "xori"}) ||
             is_load_name(n))
        r = rs;
    else if (is_store_name(n)) r = rs | rt;
    return r & ~1u;
}

std::string hexu(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%xu", v);
    return buf;
}

std::string exit_at(const Insn& i, const InsnCtx& ctx) {
    std::string s = "{ xpc = " + hexu(i.pc) + ";";
    const uint32_t give = ctx.n - ctx.k;
    if (give) s += " left += " + num(give) + "u;";
    if (ctx.has_slot) s += " xbp = " + ctx.slot_cond + "; xbt = " + ctx.slot_target + ";";
    if (ctx.pending_load) {
        const std::string r = "r" + num(ctx.pending_load);
        s += " xplreg = " + num(ctx.pending_load) + "u; xplval = " + r + "; " + r + " = plold;";
    }
    return s + " goto fin; }";
}

std::vector<std::string> gen_insn(const Insn& i, const InsnCtx& ctx) {
    const std::string& n = i.name;
    const std::string a = R(i.rs), b = R(i.rt), ex = exit_at(i, ctx), d = R(i.rd);
    const std::string sa = num(i.sa);
    if (one_of(n, {"sll", "srl", "sra", "sllv", "srlv", "srav", "addu", "subu", "and", "or", "xor", "nor", "slt", "sltu",
                   "mfhi", "mflo"})) {
        const std::map<std::string, std::string> e = {
            {"sll", b + " << " + sa + "u"},
            {"srl", b + " >> " + sa + "u"},
            {"sra", "(uint32_t)((int32_t)" + b + " >> " + sa + ")"},
            {"sllv", b + " << (" + a + " & 31u)"},
            {"srlv", b + " >> (" + a + " & 31u)"},
            {"srav", "(uint32_t)((int32_t)" + b + " >> (" + a + " & 31u))"},
            {"addu", a + " + " + b},
            {"subu", a + " - " + b},
            {"and", a + " & " + b},
            {"or", a + " | " + b},
            {"xor", a + " ^ " + b},
            {"nor", "~(" + a + " | " + b + ")"},
            {"slt", "(uint32_t)((int32_t)" + a + " < (int32_t)" + b + ")"},
            {"sltu", "(uint32_t)(" + a + " < " + b + ")"},
            {"mfhi", "hi_"},
            {"mflo", "lo_"}};
        if (!i.rd) return {};
        return {d + " = " + e.at(n) + ";"};
    }
    if (one_of(n, {"addiu", "slti", "sltiu", "andi", "ori", "xori", "lui"})) {
        const std::map<std::string, std::string> e = {
            {"addiu", a + " + " + hexu(uint32_t(i.simm))},
            {"slti", "(uint32_t)((int32_t)" + a + " < (" + num(i.simm) + "))"},
            {"sltiu", "(uint32_t)(" + a + " < " + hexu(uint32_t(i.simm)) + ")"},
            {"andi", a + " & " + hexu(i.imm)},
            {"ori", a + " | " + hexu(i.imm)},
            {"xori", a + " ^ " + hexu(i.imm)},
            {"lui", hexu(i.imm << 16)}};
        if (!i.rt) return {};
        return {R(i.rt) + " = " + e.at(n) + ";"};
    }
    if (one_of(n, {"add", "sub", "addi"})) {
        const std::string op = n == "sub" ? "__builtin_sub_overflow" : "__builtin_add_overflow";
        const std::string rhs = n != "addi" ? "(int32_t)" + b : "(" + num(i.simm) + ")";
        const uint32_t dest = n != "addi" ? i.rd : i.rt;
        const std::string out = "{ int32_t t_; if (" + op + "((int32_t)" + a + ", " + rhs + ", &t_)) " + ex;
        return {out + (dest ? " " + R(dest) + " = (uint32_t)t_; }" : " }")};
    }
    if (n == "mthi") return {"hi_ = " + a + ";"};
    if (n == "mtlo") return {"lo_ = " + a + ";"};
    if (n == "mult")
        return {"{ int64_t p_ = (int64_t)(int32_t)" + a + " * (int64_t)(int32_t)" + b +
                "; lo_ = (uint32_t)p_; hi_ = (uint32_t)((uint64_t)p_ >> 32); }"};
    if (n == "multu")
        return {"{ uint64_t p_ = (uint64_t)" + a + " * (uint64_t)" + b + "; lo_ = (uint32_t)p_; hi_ = (uint32_t)(p_ >> 32); }"};
    if (n == "div")
        return {"{ int32_t n_ = (int32_t)" + a + ", d_ = (int32_t)" + b + "; if (d_ == 0 || (n_ == INT32_MIN && d_ == -1)) " +
                ex + " lo_ = (uint32_t)(n_ / d_); hi_ = (uint32_t)(n_ % d_); }"};
    if (n == "divu")
        return {"{ uint32_t n_ = " + a + ", d_ = " + b + "; if (d_ == 0) " + ex + " lo_ = n_ / d_; hi_ = n_ % d_; }"};
    if (n == "break") return {ex};
    if (is_load_name(n) || is_store_name(n)) {
        const unsigned size = one_of(n, {"lb", "lbu", "sb"}) ? 1 : one_of(n, {"lh", "lhu", "sh"}) ? 2 : 4;
        std::string guard = "(pa_ >= kRamSize)";
        if (size > 1) guard = "(a_ & " + num(size - 1) + "u) || " + guard;
        std::string head = "{ uint32_t a_ = " + a + " + " + hexu(uint32_t(i.simm)) +
                           "; uint32_t pa_ = a_ & 0x1FFFFFFFu; if (" + guard + ") " + ex;
        if (is_store_name(n)) {
            // The interpreter performs the store and the compiled code is stale from then on: stale() says so.
            head += " if (pa_ - kCodeLo < kCodeSize) { g_stale = true; " + ex.substr(2, ex.size() - 4) + " }";
            const std::string w = n == "sw"   ? "mem.write32(a_, " + b + ");"
                                  : n == "sh" ? "mem.write16(a_, (uint16_t)" + b + ");"
                                              : "mem.write8(a_, (uint8_t)" + b + ");";
            return {head + " " + w + " }"};
        }
        if (!i.rt) return {head + " }"};
        const std::map<std::string, std::string> v = {{"lw", "ld32(ram, pa_)"},
                                                      {"lbu", "(uint32_t)ram[pa_]"},
                                                      {"lb", "(uint32_t)(int32_t)(int8_t)ram[pa_]"},
                                                      {"lhu", "(uint32_t)ld16(ram, pa_)"},
                                                      {"lh", "(uint32_t)(int32_t)(int16_t)ld16(ram, pa_)"}};
        const std::string pre = ctx.save_old ? " plold = r" + num(i.rt) + ";" : "";
        return {head + pre + " r" + num(i.rt) + " = " + v.at(n) + "; }"};
    }
    throw std::runtime_error("cannot translate " + n);
}

namespace {

std::string comment(const Insn& i) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "/* %x: %08x */", i.pc, i.word);
    return buf;
}

std::string jump(uint32_t target, const std::set<uint32_t>& emitted) {
    char buf[24];
    if (emitted.count(target)) {
        std::snprintf(buf, sizeof buf, "goto L_%x;", target);
        return buf;
    }
    return "{ xpc = " + hexu(target) + "; goto fin; }";
}

}  // namespace

void emit_block(const Block& b, const std::set<uint32_t>& emitted, std::vector<std::string>& lines) {
    const uint32_t N = uint32_t(b.insns.size());
    char head[160];
    std::snprintf(head, sizeof head, "L_%x: if (__builtin_expect(left < %uu, 0)) { xpc = %s; goto fin; } left -= %uu;",
                  b.start, N, hexu(b.start).c_str(), N);
    lines.push_back(head);
    const std::vector<Insn>& body = b.insns;
    const bool has_ctrl = body.size() >= 2 && body[body.size() - 2].has_slot();
    const size_t n_straight = has_ctrl ? body.size() - 2 : body.size();
    for (size_t k = 0; k < n_straight; ++k) {
        const Insn& i = body[k];
        const Insn* nxt = k + 1 < n_straight ? &body[k + 1] : has_ctrl ? &body[n_straight] : nullptr;
        InsnCtx ctx;
        ctx.n = N;
        ctx.k = uint32_t(k);
        if (k && body[k - 1].is_load() && body[k - 1].rt) ctx.pending_load = body[k - 1].rt;
        // the old value of a load's register is needed only if the instruction after it can exit
        if (i.is_load() && i.rt && nxt &&
            (one_of(nxt->name, {"break", "div", "divu", "add", "addi", "sub"}) || nxt->is_load() ||
             is_store_name(nxt->name)))
            ctx.save_old = true;
        for (const std::string& s : gen_insn(i, ctx)) lines.push_back("  " + s + " " + comment(i));
    }
    if (!has_ctrl) {
        if (!b.ends_in_break) lines.push_back("  " + jump(*b.fallthrough, emitted));
        return;
    }
    const Insn& slot = body.back();
    const Insn& i = body[n_straight];
    const std::string& n = i.name;
    const std::optional<uint32_t> tgt = i.target();
    lines.push_back("  { " + comment(i));
    bool dyn = false;
    if (n == "jal") {
        lines.push_back("  r31 = " + hexu(i.pc + 8) + ";");
    } else if (n == "jalr") {
        lines.push_back("  r" + num(i.rd ? i.rd : 31) + " = " + hexu(i.pc + 8) + ";");
        lines.push_back("  uint32_t jt_ = " + R(i.rs) + ";");
        dyn = true;
    } else if (n == "jr") {
        lines.push_back("  uint32_t jt_ = " + R(i.rs) + ";");
        dyn = true;
    } else if (n == "bltzal" || n == "bgezal") {
        lines.push_back("  r31 = " + hexu(i.pc + 8) + ";");
    }
    const bool cond = is_cond_branch(n);
    if (cond) {
        const std::string x = R(i.rs), y = R(i.rt);
        const std::map<std::string, std::string> c = {
            {"beq", x + " == " + y},        {"bne", x + " != " + y},        {"blez", "(int32_t)" + x + " <= 0"},
            {"bgtz", "(int32_t)" + x + " > 0"}, {"bltz", "(int32_t)" + x + " < 0"}, {"bgez", "(int32_t)" + x + " >= 0"},
            {"bltzal", "(int32_t)" + x + " < 0"}, {"bgezal", "(int32_t)" + x + " >= 0"}};
        lines.push_back("  bool c_ = " + c.at(n) + ";");
    }
    InsnCtx sctx;
    sctx.n = N;
    sctx.k = uint32_t(n_straight) + 1;
    sctx.has_slot = true;
    sctx.slot_cond = cond ? "c_" : "true";
    sctx.slot_target = dyn ? "jt_" : hexu(*tgt);
    for (const std::string& s : gen_insn(slot, sctx)) lines.push_back("  " + s + " " + comment(slot));
    if (cond) {
        lines.push_back("  if (c_) " + jump(*tgt, emitted));
        lines.push_back("  " + jump(i.pc + 8, emitted));
    } else if (dyn) {
        lines.push_back("  xdyn = jt_; goto dispatch;");
    } else {
        lines.push_back("  " + jump(*tgt, emitted));
    }
    lines.push_back("  }");
}

}  // namespace don_setup
