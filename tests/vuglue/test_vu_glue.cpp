// SPDX-License-Identifier: GPL-3.0-or-later
// T-205 part 2: the VU1 library through src/host/vu_glue.{h,cpp}, with the REAL library
// (build/vu/libdon_vu.so), NO game and NO runtime. This program plays the runtime's part: it calls the
// callbacks of the Ps2HostVu1 table (runtime/ps2_host_vu1.h) the way the runtime's VIF1 does, and
// checks what comes back.
//
// The runtime's sequence, as the patch's header and ps2_memory.cpp describe it:
//   thread_begin(sink, sink_user) once, then data() and, if it is not null, reset(); then, per VIF1
//   command: write_micro for an MPG upload, set_top(TOP, ITOP) and execute(start_pc, 65536) for
//   MSCAL/MSCALF, execute(0xFFFFFFFF, 65536) for MSCNT, and continue_(65536) while running() says the
//   program is not finished; XGKICK packets arrive through the sink during execute / continue_;
//   thread_end() closes the library.
//
// The microprograms are the ones of src/vubridge/vu_selftest.cpp (hand-assembled; every instruction
// keeps its mnemonic in a comment). Every case runs on both engines and the two engines are compared.
//
//   test_vu_glue [--vu-dir <folder with libdon_vu>]
#include "check.h"
#include "vu_glue.h"

#include <xmmintrin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

// ---- encoding helpers (from src/vubridge/vu_selftest.cpp) -----------------------------------------
//
// Encoding of a VU instruction pair, as PCSX2's tables (pcsx2/VUops.cpp) define it: 8 bytes, the LOWER
// instruction in the first word and the UPPER one in the second.
//   upper: bit 31 I (the lower word is a float constant), 30 E (end), 29 M, 28 D, 27 T,
//          bits 24..21 dest (x y z w), 20..16 ft, 15..11 fs, 10..6 fd, 5..0 opcode
//   lower: bits 31..25 opcode (bit 31 set: register-register group), 24..21 dest, 20..16 ft/it,
//          15..11 fs/is, 10..0 immediate (IADDIU and ISUBIU: 15 bits, bits 14..11 at 24..21)
struct Pair
{
    uint32_t lower;
    uint32_t upper;
};

constexpr uint32_t L_NOP = 0x8000033C;   // nop (MOVE vf0, vf0)
constexpr uint32_t U_NOP = 0x000002FF;   // nop
constexpr uint32_t U_NOP_E = 0x400002FF; // nop with the E bit: the program ends after the next pair
constexpr uint32_t DEST_XYZW = 0xFu << 21;

constexpr uint32_t u_fmac(uint32_t op, uint32_t fd, uint32_t fs, uint32_t ft)
{
    return DEST_XYZW | (ft << 16) | (fs << 11) | (fd << 6) | op;
}
constexpr uint32_t U_ADD(uint32_t fd, uint32_t fs, uint32_t ft) { return u_fmac(0x28, fd, fs, ft); }
constexpr uint32_t U_SUB(uint32_t fd, uint32_t fs, uint32_t ft) { return u_fmac(0x2C, fd, fs, ft); }

constexpr uint32_t L_LQ(uint32_t ft, uint32_t is, uint32_t imm) { return DEST_XYZW | (ft << 16) | (is << 11) | imm; }
constexpr uint32_t L_SQ(uint32_t fs, uint32_t it, uint32_t imm)
{
    return (0x01u << 25) | DEST_XYZW | (it << 16) | (fs << 11) | imm;
}
constexpr uint32_t L_IADDIU(uint32_t it, uint32_t is, uint32_t imm15)
{
    return (0x08u << 25) | ((imm15 & 0x7800) << 10) | (it << 16) | (is << 11) | (imm15 & 0x7FF);
}
constexpr uint32_t L_ISUBIU(uint32_t it, uint32_t is, uint32_t imm15)
{
    return (0x09u << 25) | ((imm15 & 0x7800) << 10) | (it << 16) | (is << 11) | (imm15 & 0x7FF);
}
constexpr uint32_t L_IBNE(uint32_t it, uint32_t is, int32_t offset_pairs)
{
    return (0x29u << 25) | (it << 16) | (is << 11) | (static_cast<uint32_t>(offset_pairs) & 0x7FF);
}
constexpr uint32_t L_XGKICK(uint32_t is) { return 0x800006FC | (is << 11); }
// LowerOP group: bits 31..25 select LowerOP (0x40), bits 5..0 select the sub-table (0x3C = T3_00,
// 0x3D = T3_01) and bits 10..6 the entry in it. XTOP is T3_00 entry 26, XITOP T3_01 entry 26, and both
// write the `it` field of the lower word (bits 20..16): the I register of the PS2 documentation is
// reached through that field, so here they land in a plain integer register, which is observable.
constexpr uint32_t L_XTOP(uint32_t it) { return 0x800006BCu | (it << 16); }  // xtop vi<it>
constexpr uint32_t L_XITOP(uint32_t it) { return 0x800006BDu | (it << 16); } // xitop vi<it>

constexpr uint32_t bits(float f) { return __builtin_bit_cast(uint32_t, f); }
constexpr uint32_t kDataBytes = 0x4000; // 16 KiB of VU1 data memory

// The two XGKICK callback types must be the same, or the runtime's sink cannot go to the library as it
// is (vu_glue.cpp chooses a counting trampoline only for DON_VU_LOG=1).
static_assert(std::is_same_v<DonVuXgkick, Ps2HostVu1XgkickFn>,
              "the library's XGKICK callback and the runtime's sink must have the same type");

const char *g_engine_name = "";

// ---- the runtime's part ----------------------------------------------------------------------------

struct Capture
{
    std::vector<std::vector<uint8_t>> packets; // cleared between cases
    uint64_t packets_total = 0;                // never cleared: what the sink received in all
    uint64_t bytes_total = 0;                  // never cleared: the same, in bytes
    uint32_t mxcsr_in_callback = 0;
};

void sink(void *user, const uint8_t *packet, uint32_t bytes)
{
    auto *cap = static_cast<Capture *>(user);
    cap->packets.emplace_back(packet, packet + bytes);
    ++cap->packets_total;
    cap->bytes_total += bytes;
    cap->mxcsr_in_callback = _mm_getcsr(); // must be the caller's value, not the VU's
}

void write_program(const Ps2HostVu1 &h, const std::vector<Pair> &program, uint32_t addr_bytes = 0)
{
    std::vector<uint8_t> bytes(program.size() * 8);
    std::memcpy(bytes.data(), program.data(), bytes.size());
    h.write_micro(h.user, addr_bytes, bytes.data(), static_cast<uint32_t>(bytes.size()));
}

uint8_t *data_of(const Ps2HostVu1 &h) { return h.data(h.user); }

void clear_data(const Ps2HostVu1 &h) { std::memset(data_of(h), 0, kDataBytes); }

void put_qword(const Ps2HostVu1 &h, uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    const uint32_t v[4] = {x, y, z, w};
    std::memcpy(data_of(h) + index * 16, v, 16);
}

void get_qword(const Ps2HostVu1 &h, uint32_t index, uint32_t out[4])
{
    std::memcpy(out, data_of(h) + index * 16, 16);
}

void check_qword(const Ps2HostVu1 &h, uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                 const char *what)
{
    uint32_t q[4];
    get_qword(h, index, q);
    const bool ok = q[0] == x && q[1] == y && q[2] == z && q[3] == w;
    if (!ok)
        std::fprintf(stderr, "[%s] %s: qword %u = %08x %08x %08x %08x (expected %08x %08x %08x %08x)\n",
                     g_engine_name, what, index, q[0], q[1], q[2], q[3], x, y, z, w);
    CHECK(ok);
}

// The end state of one case: what a program left in data memory and what it sent through XGKICK.
struct Snap
{
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> packets;
    uint32_t cycles = 0;
    bool valid = false;
};

Snap snap(const Ps2HostVu1 &h, Capture &cap, uint32_t cycles)
{
    Snap s;
    s.data.assign(data_of(h), data_of(h) + kDataBytes);
    s.packets = cap.packets;
    s.cycles = cycles;
    s.valid = true;
    return s;
}

// ---- the cases ------------------------------------------------------------------------------------

// Program 1: load a vector, add it to itself, store it, end (the program of the T-204 card).
Snap case_add(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    const std::vector<Pair> program = {
        {L_LQ(1, 0, 0), U_NOP},  // lq.xyzw vf1, 0(vi0)
        {L_NOP, U_ADD(2, 1, 1)}, // add.xyzw vf2, vf1, vf1
        {L_SQ(2, 0, 1), U_NOP},  // sq.xyzw vf2, 1(vi0)
        {L_NOP, U_NOP_E},        // nop ; nop [E]
        {L_NOP, U_NOP},          // delay slot of the end
    };
    write_program(h, program);
    put_qword(h, 0, bits(1.5f), bits(-2.0f), bits(0.25f), bits(100.0f));
    put_qword(h, 1, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef);
    h.set_top(h.user, 1, 2); // the runtime does this before every start
    const uint32_t cycles = h.execute(h.user, 0, 1000);
    CHECK(cycles > 0 && cycles < 100);
    CHECK_EQ(h.running(h.user), 0);
    check_qword(h, 1, bits(3.0f), bits(-4.0f), bits(0.5f), bits(200.0f), "add: result");
    // get_regs through the table: vf1 was loaded by lq, vf2 holds the sum, vi00 stays 0.
    uint32_t vf[32 * 4] = {};
    uint32_t vi[32] = {};
    h.get_regs(h.user, vf, vi);
    CHECK(vf[1 * 4 + 0] == bits(1.5f) && vf[1 * 4 + 1] == bits(-2.0f) && vf[1 * 4 + 2] == bits(0.25f) &&
          vf[1 * 4 + 3] == bits(100.0f));
    CHECK(vf[2 * 4 + 0] == bits(3.0f) && vf[2 * 4 + 1] == bits(-4.0f) && vf[2 * 4 + 2] == bits(0.5f) &&
          vf[2 * 4 + 3] == bits(200.0f));
    CHECK_EQ(vi[0], 0);
    return snap(h, cap, cycles);
}

// Program 2: a loop with an integer counter and a branch with its delay slot.
std::vector<Pair> loop_program(uint32_t count)
{
    return {
        {L_IADDIU(1, 0, count), U_NOP},      // iaddiu vi1, vi0, count
        {L_LQ(1, 0, 0), U_SUB(2, 2, 2)},     // lq.xyzw vf1, 0(vi0) ; sub.xyzw vf2, vf2, vf2
        {L_ISUBIU(1, 1, 1), U_ADD(2, 2, 1)}, // loop: isubiu vi1, vi1, 1 ; add.xyzw vf2, vf2, vf1
        {L_NOP, U_NOP},                      // nop ; nop
        {L_NOP, U_NOP},                      // nop ; nop
        {L_IBNE(0, 1, -4), U_NOP},           // ibne vi1, vi0, loop   (target = pair 5 + 1 - 4 = pair 2)
        {L_IADDIU(2, 2, 1), U_NOP},          // delay slot: iaddiu vi2, vi2, 1
        {L_SQ(2, 0, 1), U_NOP},              // sq.xyzw vf2, 1(vi0)
        {L_NOP, U_NOP_E},                    // nop ; nop [E]
        {L_NOP, U_NOP},                      // delay slot of the end
    };
}

Snap case_loop(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    write_program(h, loop_program(4));
    put_qword(h, 0, bits(1.0f), bits(2.0f), bits(3.0f), bits(4.0f));
    put_qword(h, 1, 0, 0, 0, 0);
    uint32_t vi[32] = {};
    h.set_regs(h.user, nullptr, vi); // vi1, vi2 = 0
    h.set_top(h.user, 0, 0);
    const uint32_t cycles = h.execute(h.user, 0, 10000);
    CHECK_EQ(h.running(h.user), 0);
    check_qword(h, 1, bits(4.0f), bits(8.0f), bits(12.0f), bits(16.0f), "loop: four passes");
    uint32_t vf[32 * 4] = {};
    uint32_t out_vi[32] = {};
    h.get_regs(h.user, vf, out_vi);
    CHECK_EQ(out_vi[1], 0); // the counter came back to 0
    CHECK_EQ(out_vi[2], 4); // the delay slot ran once per pass
    return snap(h, cap, cycles);
}

// Program 3: XGKICK of a packet whose address comes in vi1 (set from outside). One packet inside data
// memory and one that wraps at the end of it (the library joins the two parts).
constexpr uint32_t TAG_LO0 = 0x00008001, TAG_LO1 = 0x10000000, TAG_HI0 = 0x0000000E, TAG_HI1 = 0;

Snap case_xgkick(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    const std::vector<Pair> program = {
        {L_XGKICK(1), U_NOP}, // xgkick vi1
        {L_NOP, U_NOP_E},     // nop ; nop [E]
        {L_NOP, U_NOP},       // delay slot of the end
    };
    write_program(h, program);
    // GIF tag: NLOOP = 1, EOP = 1, FLG = PACKED, NREG = 1, REGS = 0xE (A+D); then one qword of data.
    put_qword(h, 8, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
    put_qword(h, 9, 0x11223344, 0x55667788, 0x0000004C, 0);
    put_qword(h, 1023, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
    put_qword(h, 0, 0xA1A2A3A4, 0xB1B2B3B4, 0x0000004C, 0);
    uint32_t vi[32] = {};
    vi[1] = 8;
    h.set_regs(h.user, nullptr, vi);
    h.set_top(h.user, 3, 4);
    uint32_t cycles = h.execute(h.user, 0, 1000);
    vi[1] = 1023;
    h.set_regs(h.user, nullptr, vi);
    cycles += h.execute(h.user, 0, 1000);

    CHECK_EQ(cap.packets.size(), 2);
    if (cap.packets.size() == 2)
    {
        uint8_t expect[32];
        std::memcpy(expect, data_of(h) + 8 * 16, 32); // tag at qword 8, data at qword 9
        CHECK(cap.packets[0].size() == 32 && std::memcmp(cap.packets[0].data(), expect, 32) == 0);
        std::memcpy(expect, data_of(h) + 1023 * 16, 16);
        std::memcpy(expect + 16, data_of(h), 16); // joined across the wrap
        CHECK(cap.packets[1].size() == 32 && std::memcmp(cap.packets[1].data(), expect, 32) == 0);
        uint32_t tag0 = 0;
        std::memcpy(&tag0, cap.packets[0].data(), 4);
        CHECK_EQ(tag0, TAG_LO0);
    }
    return snap(h, cap, cycles);
}

// Program 4: XITOP and XTOP, to see what set_top passed. Each writes the register named by the `it`
// field of its lower word: with it = 3 and 4 those are plain integer registers, readable through
// get_regs (vi3 = ITOP, vi4 = TOP).
Snap case_set_top(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    const std::vector<Pair> program = {
        {L_XITOP(3), U_NOP}, // xitop vi3
        {L_XTOP(4), U_NOP},  // xtop vi4
        {L_NOP, U_NOP_E},    // nop ; nop [E]
        {L_NOP, U_NOP},      // delay slot of the end
    };
    write_program(h, program);
    uint32_t vi[32] = {};
    h.set_regs(h.user, nullptr, vi);
    h.set_top(h.user, 5, 7);
    const uint32_t cycles = h.execute(h.user, 0, 1000);
    CHECK_EQ(h.running(h.user), 0);
    uint32_t vf[32 * 4] = {};
    uint32_t out_vi[32] = {};
    h.get_regs(h.user, vf, out_vi);
    CHECK_EQ(out_vi[3], 7); // the ITOP the runtime passed
    CHECK_EQ(out_vi[4], 5); // the TOP
    return snap(h, cap, cycles);
}

// The budget and the continuation, the two ways the runtime uses the table:
//  * a start with a budget and then continue_ while running() says the program is not finished (the
//    loop of PS2Memory::runHostVu1); the final state and the cycle count must be those of one whole run;
//  * MSCNT: execute(PS2_HOST_VU1_CONTINUE_PC = 0xFFFFFFFF), which starts at the current TPC and must not
//    put it back to 0. (don_vu.h: execute() first runs to its end a program the budget had stopped, and
//    then starts at TPC: the runtime's own continuation loop is the continue_ one above.)
Snap case_continue(const Ps2HostVu1 &h, Capture &cap)
{
    // The whole run first: the reference.
    clear_data(h);
    write_program(h, loop_program(200));
    put_qword(h, 0, bits(1.0f), bits(2.0f), bits(3.0f), bits(4.0f));
    put_qword(h, 1, 0, 0, 0, 0);
    uint32_t vi[32] = {};
    h.set_regs(h.user, nullptr, vi);
    h.reset(h.user);
    const uint32_t whole_cycles = h.execute(h.user, 0, 100000);
    CHECK_EQ(h.running(h.user), 0);
    uint32_t whole_q[4];
    get_qword(h, 1, whole_q);
    uint32_t vf[32 * 4] = {};
    uint32_t whole_vi[32] = {};
    h.get_regs(h.user, vf, whole_vi);

    // The same in slices: a small budget, so the library stops before the end.
    put_qword(h, 1, 0, 0, 0, 0);
    h.set_regs(h.user, nullptr, vi);
    h.reset(h.user);
    uint32_t total = h.execute(h.user, 0, 40);
    CHECK_EQ(h.running(h.user), 1);
    int slices = 1;
    while (h.running(h.user) && slices < 100000)
    {
        total += h.continue_(h.user, 40);
        ++slices;
    }
    CHECK(slices > 5);
    CHECK_EQ(h.running(h.user), 0);
    check_qword(h, 1, bits(200.0f), bits(400.0f), bits(600.0f), bits(800.0f), "continue: 200 passes");
    uint32_t sliced_q[4];
    get_qword(h, 1, sliced_q);
    CHECK(std::memcmp(whole_q, sliced_q, 16) == 0);
    uint32_t sliced_vi[32] = {};
    h.get_regs(h.user, vf, sliced_vi);
    CHECK_EQ(sliced_vi[1], whole_vi[1]);
    CHECK_EQ(sliced_vi[2], whole_vi[2]);
    CHECK_EQ(total, whole_cycles); // PCSX2's cycle count does not depend on the slicing

    // MSCNT. Two programs in one microcode write: one at pair 0 (which would set vi6 = 9) and one at
    // pair 2 (vi6 = 3). TPC is set to pair 2 through set_regs, so only the second one must run.
    clear_data(h);
    h.reset(h.user);
    write_program(h, {
                         {L_IADDIU(6, 0, 9), U_NOP}, // iaddiu vi6, vi0, 9   (pair 0, must not run)
                         {L_NOP, U_NOP},             // nop ; nop
                         {L_IADDIU(6, 0, 3), U_NOP}, // iaddiu vi6, vi0, 3   (pair 2 = TPC)
                         {L_NOP, U_NOP_E},           // nop ; nop [E]
                         {L_NOP, U_NOP},             // delay slot of the end
                     });
    uint32_t vi_tpc[32] = {};
    vi_tpc[26] = 2; // TPC: the program counter is in 8-byte instruction pairs
    h.set_regs(h.user, nullptr, vi_tpc);
    h.set_top(h.user, 0, 0);
    const uint32_t mscnt_cycles = h.execute(h.user, PS2_HOST_VU1_CONTINUE_PC, 1000);
    CHECK_EQ(h.running(h.user), 0);
    uint32_t out_vi[32] = {};
    h.get_regs(h.user, vf, out_vi);
    CHECK_EQ(out_vi[6], 3); // MSCNT started at TPC = pair 2, not at pair 0
    CHECK(mscnt_cycles > 0);
    return snap(h, cap, total + mscnt_cycles);
}

// reset: registers, flags and the compiled programs go back to zero, both memories are kept (don_vu.h).
Snap case_reset(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    const std::vector<Pair> program = {
        {L_LQ(1, 0, 0), U_NOP},  // lq.xyzw vf1, 0(vi0)
        {L_NOP, U_ADD(2, 1, 1)}, // add.xyzw vf2, vf1, vf1
        {L_SQ(2, 0, 1), U_NOP},  // sq.xyzw vf2, 1(vi0)
        {L_NOP, U_NOP_E},        // nop ; nop [E]
        {L_NOP, U_NOP},          // delay slot of the end
    };
    write_program(h, program);
    put_qword(h, 0, bits(1.5f), bits(-2.0f), bits(0.25f), bits(100.0f));
    put_qword(h, 1, 0, 0, 0, 0);
    uint32_t vi[32] = {};
    vi[4] = 0x77;
    h.set_regs(h.user, nullptr, vi);
    h.set_top(h.user, 9, 9);
    const uint32_t cycles = h.execute(h.user, 0, 1000);
    CHECK_EQ(h.running(h.user), 0);
    check_qword(h, 1, bits(3.0f), bits(-4.0f), bits(0.5f), bits(200.0f), "reset: the run before it");

    h.reset(h.user);
    CHECK_EQ(h.running(h.user), 0);
    uint32_t vf[32 * 4] = {};
    uint32_t out_vi[32] = {};
    h.get_regs(h.user, vf, out_vi);
    CHECK_EQ(out_vi[4], 0);   // the integer registers were zeroed
    CHECK_EQ(vf[2 * 4 + 0], 0); // and so were the VF registers
    check_qword(h, 1, bits(3.0f), bits(-4.0f), bits(0.5f), bits(200.0f), "reset: data memory is kept");
    // The program can be run again after the reset (the compiled code was discarded, not the memory).
    put_qword(h, 1, 0, 0, 0, 0);
    h.execute(h.user, 0, 1000);
    check_qword(h, 1, bits(3.0f), bits(-4.0f), bits(0.5f), bits(200.0f), "reset: the program still runs");
    return snap(h, cap, cycles);
}

// The caller's MXCSR: the library switches it for the duration of execute / continue_ and restores it,
// and the XGKICK sink runs with the caller's value (don_vu.h). The game thread runs game code with its
// own settings, so this has to hold through the table too.
void case_mxcsr(const Ps2HostVu1 &h, Capture &cap)
{
    clear_data(h);
    write_program(h, {{L_XGKICK(1), U_NOP}, {L_NOP, U_NOP_E}, {L_NOP, U_NOP}});
    put_qword(h, 8, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
    put_qword(h, 9, 1, 2, 3, 4);
    uint32_t vi[32] = {};
    vi[1] = 8;
    h.set_regs(h.user, nullptr, vi);

    const uint32_t caller = (0x1F80u & ~0x6000u) | 0x2000u; // exceptions masked, round down, no FTZ/DAZ
    const uint32_t saved = _mm_getcsr();
    _mm_setcsr(caller);
    cap.packets.clear();
    h.execute(h.user, 0, 1000);
    const uint32_t after = _mm_getcsr();
    _mm_setcsr(saved);
    CHECK_EQ(after, caller);
    CHECK_EQ(cap.packets.size(), 1);
    CHECK_EQ(cap.mxcsr_in_callback, caller);
}

// ---- one engine ------------------------------------------------------------------------------------

struct Run
{
    Snap add, loop, xgkick, set_top, reset, cont;
};

Run run_engine(const DonVuApi &api, int engine)
{
    g_engine_name = engine == DON_VU_ENGINE_MICROVU ? "microvu" : "pcsx2interp";
    Capture cap;
    Run run;

    HostVu vu(api, engine);
    const Ps2HostVu1 &h = vu.iface();
    CHECK_EQ(vu.engine(), engine);
    CHECK_EQ(vu.state(), HostVu::Pending);
    CHECK(h.user == static_cast<void *>(&vu));
    CHECK(h.data && h.write_micro && h.execute && h.continue_ && h.running && h.reset && h.set_top &&
          h.set_regs && h.get_regs);
    CHECK(h.thread_begin != nullptr);
    CHECK(h.thread_end != nullptr);

    // Before thread_begin every callback does nothing and none of them crashes.
    CHECK(h.data(h.user) == nullptr);
    CHECK_EQ(h.execute(h.user, 0, 1000), 0);
    CHECK_EQ(h.continue_(h.user, 1000), 0);
    CHECK_EQ(h.running(h.user), 0);
    h.write_micro(h.user, 0, nullptr, 0);
    h.set_top(h.user, 1, 2);
    h.reset(h.user);
    h.set_regs(h.user, nullptr, nullptr);
    uint32_t untouched[32 * 4];
    std::memset(untouched, 0xAB, sizeof(untouched));
    h.get_regs(h.user, untouched, nullptr);
    CHECK(untouched[0] == 0xABABABABu); // get_regs did not touch the caller's buffer

    // thread_begin gives the library the runtime's XGKICK sink and its user pointer.
    h.thread_begin(h.user, &sink, &cap);
    CHECK_EQ(vu.state(), HostVu::Open);
    CHECK(vu.is_open());
    CHECK(h.data(h.user) != nullptr);
    CHECK_EQ(static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(h.data(h.user)) & 63u), 0u); // 64-byte aligned

    // A second thread_begin is refused cleanly and the open VU1 keeps working (checked by the cases
    // below, starting with the add).
    h.thread_begin(h.user, &sink, &cap);
    CHECK_EQ(vu.state(), HostVu::Open);
    CHECK(h.data(h.user) != nullptr);

    // The runtime calls reset() once, right after data() succeeded.
    h.reset(h.user);
    run.add = case_add(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    run.loop = case_loop(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    run.xgkick = case_xgkick(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    run.set_top = case_set_top(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    run.reset = case_reset(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    run.cont = case_continue(h, cap);
    cap.packets.clear();
    h.reset(h.user);
    case_mxcsr(h, cap);
    cap.packets.clear();
    h.reset(h.user);

    // Opening twice (a second instance while this one is open) must be refused cleanly: a message, the
    // Failed state, no data memory and no callback that does anything, and it must not close the VU1
    // the first instance is using.
    {
        HostVu other(api, engine);
        CHECK_EQ(other.state(), HostVu::Pending);
        other.iface().thread_begin(other.iface().user, &sink, &cap);
        CHECK_EQ(other.state(), HostVu::Failed);
        CHECK(other.error().rfind("[don-vu]", 0) == 0);
        CHECK(other.iface().data(other.iface().user) == nullptr);
        CHECK_EQ(other.iface().execute(other.iface().user, 0, 1000), 0);
        CHECK_EQ(other.iface().running(other.iface().user), 0);
        other.iface().thread_end(other.iface().user);
        CHECK_EQ(other.state(), HostVu::Failed); // a failed open stays failed after thread_end
    }
    CHECK(vu.is_open());
    cap.packets.clear();
    const Snap after_refusal = case_add(h, cap); // the first instance still runs programs
    CHECK(after_refusal.data == run.add.data);

    // The counters of the session, for DON_VU_LOG.
    h.thread_end(h.user);
    CHECK_EQ(vu.state(), HostVu::Closed);
    CHECK(!vu.is_open());
    CHECK(h.data(h.user) == nullptr); // after thread_end the runtime must not use the memory
    CHECK_EQ(h.execute(h.user, 0, 1000), 0);
    h.thread_end(h.user); // a second thread_end is harmless

    const DonVuSummary summary = vu.summary();
    CHECK(summary.starts > 0);
    CHECK(summary.cycles > 0);
    CHECK(summary.continues > 0); // the continuation case
    const bool log = std::getenv("DON_VU_LOG") != nullptr && std::strcmp(std::getenv("DON_VU_LOG"), "1") == 0;
    if (log)
    {
        // The sink trampoline counts both (the library has no byte counter).
        CHECK_EQ(summary.packets, cap.packets_total);
        CHECK_EQ(summary.bytes, cap.bytes_total);
    }
    else
    {
        // Without the log the runtime's sink goes to the library directly and nothing is counted.
        CHECK_EQ(summary.packets, 0);
        CHECK_EQ(summary.bytes, 0);
    }
    vu.print_summary();
    return run;
}

} // namespace

int main(int argc, char **argv)
{
    std::string vu_dir = std::string(DON_REPO_ROOT) + "/build/vu";
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--vu-dir") == 0 && i + 1 < argc)
            vu_dir = argv[++i];
        else if (argv[i][0] != '-')
            vu_dir = argv[i];
        else
        {
            std::fprintf(stderr, "usage: %s [--vu-dir <folder>]\n", argv[0]);
            return 2;
        }
    }

    // A folder with no library: a clear message and a failure, not a crash.
    {
        DonVuApi missing;
        std::string missing_error;
        CHECK(!don_vu_load("no-such-vu-folder", missing, missing_error));
        CHECK(missing_error.rfind("[don-vu]", 0) == 0);
        CHECK(missing_error.find("libdon_vu") != std::string::npos);
    }

    DonVuApi api;
    std::string error;
    if (!don_vu_load(vu_dir, api, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("-- library: %s\n", api.path.string().c_str());

    const Run jit = run_engine(api, DON_VU_ENGINE_MICROVU);
    const Run interp = run_engine(api, DON_VU_ENGINE_INTERPRETER);

    // The same sequence on the other engine: same data memory, same packets.
    const struct
    {
        const char *name;
        const Snap *a;
        const Snap *b;
    } pairs[] = {
        {"add", &jit.add, &interp.add},           {"loop", &jit.loop, &interp.loop},
        {"xgkick", &jit.xgkick, &interp.xgkick},  {"set_top", &jit.set_top, &interp.set_top},
        {"reset", &jit.reset, &interp.reset},     {"continue", &jit.cont, &interp.cont},
    };
    for (const auto &pair : pairs)
    {
        CHECK(pair.a->valid && pair.b->valid);
        if (!pair.a->valid || !pair.b->valid)
            continue;
        const bool same_data = pair.a->data == pair.b->data;
        const bool same_packets = pair.a->packets == pair.b->packets;
        if (!same_data || !same_packets)
            std::fprintf(stderr, "compare %s: microvu %s the interpreter (data memory %s, packets %s)\n",
                         pair.name, same_data && same_packets ? "matches" : "differs from",
                         same_data ? "same" : "DIFFERENT", same_packets ? "same" : "DIFFERENT");
        CHECK(same_data && same_packets);
    }

    return check_result("test_vu_glue");
}
