// Ahead-of-time translation of the IOP sound module (cri_adxi) of the player's own disc to C++ (task T-694): a C++
// port of tools/iop_recomp/make_native.py and emit_adxi.py, whose output it reproduces byte for byte. The emitted
// file is derived from game code: it lives in the generated tree, never in git.
//
// Contract of the emitted run() (one instruction == one cycle, constant cost per block, exits to the interpreter at
// anything it does not translate) is documented in the Python emitter's header; the text it prints is unchanged.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace don_setup {

constexpr uint32_t kAdxiBase = 0x6E000;      // address the port's IOP emulator loads cri_adxi at
constexpr uint32_t kAdxiCodeHi = 0x78500;    // exclusive end of the code (the IRX import table starts there)
constexpr uint32_t kIopImageSize = 0x20000;  // flat image the IRX is relocated into
constexpr uint32_t kIopRamSize = 0x200000;

struct IopEmitStats {
    size_t functions = 0, refused = 0, blocks = 0, translated_instructions = 0, entries = 0;
};

// Flat image of an IRX (ET_SCE_IOPRELEXEC) with its R_MIPS_32/26/HI16/LO16 relocations applied for `base`.
// Throws std::runtime_error on a damaged module.
std::vector<uint8_t> relocate_irx(const std::vector<uint8_t>& irx, uint32_t base);

// The whole adxi_native.cpp text for a relocated image. Throws std::runtime_error when it cannot be translated.
std::string emit_adxi_native(const std::vector<uint8_t>& image, uint32_t base, uint32_t code_lo, uint32_t code_hi,
                             IopEmitStats* stats = nullptr);

// ---- internals shared by iop_emit.cpp and iop_emit_insn.cpp (public so the tests can reach them) ----------------

struct Insn {
    uint32_t pc = 0, word = 0;
    std::string name;  // "bad" when not translatable
    uint32_t rs = 0, rt = 0, rd = 0, sa = 0, imm = 0;
    int32_t simm = 0;

    bool is_load() const;
    bool has_slot() const;
    std::optional<uint32_t> target() const;  // static target of a branch or j/jal
};

Insn decode(uint32_t pc, uint32_t word);
uint32_t reads_mask(const Insn& i);  // bit n set: reads GPR n (r0 never)

struct Block {
    uint32_t start = 0;
    std::vector<Insn> insns;
    std::optional<uint32_t> fallthrough;  // next block when it ends without a control transfer
    bool ends_in_break = false;
};

struct Function {
    uint32_t start = 0, end = 0;
    std::optional<std::string> refused;
    std::vector<Block> blocks;
};

// What an instruction's text needs to know about its place in the block.
struct InsnCtx {
    uint32_t n = 0, k = 0;  // block length, index of the instruction (instructions executed before a branch exits)
    bool has_slot = false;  // the instruction is a delay slot: an exit leaves this branch pending
    std::string slot_cond, slot_target;
    uint32_t pending_load = 0;  // GPR of the load right before (still pending when this instruction exits)
    bool save_old = false;      // a load whose next instruction can exit: keep the old value of its register
};

std::string hexu(uint32_t v);                                                            // "0x1f4u"
std::string exit_at(const Insn& i, const InsnCtx& ctx);                                  // "{ xpc = ...; goto fin; }"
std::vector<std::string> gen_insn(const Insn& i, const InsnCtx& ctx);                    // C++ of one non-control insn

struct Analysis {
    std::vector<uint8_t> data;
    uint32_t base = 0, lo = 0, hi = 0;
    std::set<uint32_t> stubs, heads, leaders;
    std::vector<Function> functions;

    Analysis(std::vector<uint8_t> image, uint32_t base_, uint32_t code_lo, uint32_t code_hi);
    std::optional<uint32_t> word(uint32_t a) const;
    Insn insn(uint32_t a) const;
    uint32_t end() const { return base + uint32_t(data.size() / 4 * 4); }
    bool in_code(uint32_t a) const { return lo <= a && a < hi && (a & 3) == 0; }

private:
    bool is_stub(uint32_t a) const;
    bool looks_like_head(uint32_t a) const;
    void run();
    void check(Function& f) const;
    std::vector<Block> blocks(const Function& f) const;
};

// The lines of one basic block (iop_emit_insn.cpp).
void emit_block(const Block& b, const std::set<uint32_t>& emitted, std::vector<std::string>& lines);

}  // namespace don_setup
