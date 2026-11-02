// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-test of libdon_vu through its C API only (no PCSX2 header). Needs no game data: the
// microprograms are hand assembled below. Every case runs on both engines (microVU and PCSX2's
// interpreter), each result is checked against the expected values, and the two engines' final states
// (data memory, VF and VI registers, GIF packets) are compared with each other.
//
// Every check prints one OK or FAIL line; exit code 0 only if all pass.
//
// Encoding of a VU instruction pair, as PCSX2's tables (pcsx2/VUops.cpp) define it: 8 bytes, the LOWER
// instruction in the first word and the UPPER one in the second.
//   upper: bit 31 I (the lower word is a float constant), 30 E (end), 29 M, 28 D, 27 T,
//          bits 24..21 dest (x y z w), 20..16 ft, 15..11 fs, 10..6 fd, 5..0 opcode
//   lower: bits 31..25 opcode (bit 31 set: register-register group), 24..21 dest, 20..16 ft/it,
//          15..11 fs/is, 10..0 immediate (IADDIU and ISUBIU: 15 bits, bits 14..11 at 24..21)

#include <cstdarg>
#include "don_vu.h"

#include <xmmintrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void check(bool ok, const char* fmt, ...)
{
	char text[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	++g_checks;
	if (!ok)
		++g_failures;
	std::printf("%s %s\n", ok ? "OK  " : "FAIL", text);
}

// ---- encoding helpers ----------------------------------------------------------------------------

struct Pair
{
	uint32_t lower;
	uint32_t upper;
};

constexpr uint32_t L_NOP = 0x8000033C;    // nop (MOVE vf0, vf0)
constexpr uint32_t U_NOP = 0x000002FF;    // nop
constexpr uint32_t U_NOP_E = 0x400002FF;  // nop with the E bit: the program ends after the next pair
constexpr uint32_t DEST_XYZW = 0xFu << 21;

constexpr uint32_t u_fmac(uint32_t op, uint32_t fd, uint32_t fs, uint32_t ft)
{
	return DEST_XYZW | (ft << 16) | (fs << 11) | (fd << 6) | op;
}
constexpr uint32_t U_ADD(uint32_t fd, uint32_t fs, uint32_t ft) { return u_fmac(0x28, fd, fs, ft); }
constexpr uint32_t U_SUB(uint32_t fd, uint32_t fs, uint32_t ft) { return u_fmac(0x2C, fd, fs, ft); }
constexpr uint32_t U_MUL(uint32_t fd, uint32_t fs, uint32_t ft) { return u_fmac(0x2A, fd, fs, ft); }

constexpr uint32_t L_LQ(uint32_t ft, uint32_t is, uint32_t imm) { return DEST_XYZW | (ft << 16) | (is << 11) | imm; }
constexpr uint32_t L_SQ(uint32_t fs, uint32_t it, uint32_t imm) { return (0x01u << 25) | DEST_XYZW | (it << 16) | (fs << 11) | imm; }
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

// ---- running a program on the library -------------------------------------------------------------

struct Packet
{
	std::vector<uint8_t> bytes;
};

struct Capture
{
	std::vector<Packet> packets;
	uint32_t mxcsr_in_callback = 0;
};

void on_xgkick(void* user, const uint8_t* packet, uint32_t bytes)
{
	Capture* c = static_cast<Capture*>(user);
	c->packets.push_back({std::vector<uint8_t>(packet, packet + bytes)});
	c->mxcsr_in_callback = _mm_getcsr();
}

// State at the end of a case, for the checks and for the comparison between engines.
struct Result
{
	std::vector<uint8_t> data;     // 16 KiB of data memory
	uint32_t vf[32 * 4];
	uint32_t vi[32];
	std::vector<Packet> packets;
	uint32_t cycles = 0;
	bool valid = false;
};

void load_program(const std::vector<Pair>& program, uint32_t addr_bytes = 0)
{
	std::vector<uint8_t> bytes(program.size() * 8);
	std::memcpy(bytes.data(), program.data(), bytes.size());
	don_vu1_write_micro(addr_bytes, bytes.data(), static_cast<uint32_t>(bytes.size()));
}

void put_qword(uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
	const uint32_t v[4] = {x, y, z, w};
	std::memcpy(don_vu1_data() + index * 16, v, 16);
}

void get_qword(uint32_t index, uint32_t out[4])
{
	std::memcpy(out, don_vu1_data() + index * 16, 16);
}

constexpr uint32_t bits(float f)
{
	return __builtin_bit_cast(uint32_t, f);
}

Result snapshot(Capture& cap, uint32_t cycles)
{
	Result r;
	r.data.assign(don_vu1_data(), don_vu1_data() + 0x4000);
	don_vu1_get_regs(r.vf, r.vi);
	r.packets = cap.packets;
	r.cycles = cycles;
	r.valid = true;
	return r;
}

bool qword_is(uint32_t index, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
	uint32_t q[4];
	get_qword(index, q);
	return q[0] == x && q[1] == y && q[2] == z && q[3] == w;
}

std::string qword_text(uint32_t index)
{
	uint32_t q[4];
	get_qword(index, q);
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%08x %08x %08x %08x", q[0], q[1], q[2], q[3]);
	return buf;
}

const char* g_engine_name = "";

// One case = one function that runs on the open engine and returns the end state.
// Program 1 (the one of the task card): load a vector, add it to itself, store it, end.
//   pair 0: LQ.xyzw vf1, 0(vi0)
//   pair 1: ADD.xyzw vf2, vf1, vf1
//   pair 2: SQ.xyzw vf2, 1(vi0)
//   pair 3: end (E bit), pair 4 is its delay slot
Result case_add(Capture& cap)
{
	const std::vector<Pair> program = {
		{L_LQ(1, 0, 0), U_NOP},       // lq.xyzw vf1, 0(vi0)
		{L_NOP, U_ADD(2, 1, 1)},      // add.xyzw vf2, vf1, vf1
		{L_SQ(2, 0, 1), U_NOP},       // sq.xyzw vf2, 1(vi0)
		{L_NOP, U_NOP_E},             // nop ; nop [E]
		{L_NOP, U_NOP},               // delay slot of the end
	};
	load_program(program);
	put_qword(0, bits(1.5f), bits(-2.0f), bits(0.25f), bits(100.0f));
	put_qword(1, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef);
	const uint32_t cycles = don_vu1_execute(0, 1000);
	check(!don_vu1_running(), "[%s] add: the program ended", g_engine_name);
	check(cycles > 0 && cycles < 100, "[%s] add: cycles used = %u", g_engine_name, cycles);
	check(qword_is(1, bits(3.0f), bits(-4.0f), bits(0.5f), bits(200.0f)),
		"[%s] add: result in data memory = %s (expected 3, -4, 0.5, 200)", g_engine_name, qword_text(1).c_str());
	return snapshot(cap, cycles);
}

// Program 2: a loop with an integer counter and a branch with its delay slot.
//   pair 0: IADDIU vi1, vi0, count            pair 1: LQ vf1 ; SUB vf2, vf2, vf2 (vf2 = 0)
//   pair 2 (loop): ISUBIU vi1, vi1, 1 ; ADD vf2, vf2, vf1
//   pairs 3, 4: nop
//   pair 5: IBNE vi1, vi0, loop               pair 6 (delay slot): IADDIU vi2, vi2, 1
//   pair 7: SQ vf2, 1(vi0)                    pair 8: E                pair 9: delay slot
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

Result case_loop(Capture& cap)
{
	load_program(loop_program(4));
	put_qword(0, bits(1.0f), bits(2.0f), bits(3.0f), bits(4.0f));
	put_qword(1, 0, 0, 0, 0);
	uint32_t vi[32] = {};
	don_vu1_set_regs(nullptr, vi); // vi1, vi2 = 0
	const uint32_t cycles = don_vu1_execute(0, 10000);
	uint32_t out_vi[32], out_vf[128];
	don_vu1_get_regs(out_vf, out_vi);
	check(!don_vu1_running(), "[%s] loop: the program ended", g_engine_name);
	check(qword_is(1, bits(4.0f), bits(8.0f), bits(12.0f), bits(16.0f)),
		"[%s] loop: vf2 stored = %s (expected 4, 8, 12, 16: four passes)", g_engine_name, qword_text(1).c_str());
	check(out_vi[1] == 0 && out_vi[2] == 4, "[%s] loop: vi1 = %u (expected 0), vi2 = %u (expected 4: delay slot ran each pass)",
		g_engine_name, out_vi[1], out_vi[2]);
	return snapshot(cap, cycles);
}

// Program 3: XGKICK of a packet whose address comes in vi1 (set from outside).
std::vector<Pair> xgkick_program()
{
	return {
		{L_XGKICK(1), U_NOP}, // xgkick vi1
		{L_NOP, U_NOP_E},     // nop ; nop [E]
		{L_NOP, U_NOP},       // delay slot of the end
	};
}

// GIF tag: NLOOP = 1, EOP = 1, FLG = PACKED, NREG = 1, REGS = 0xE (A+D); followed by one qword of data.
constexpr uint32_t TAG_LO0 = 0x00008001, TAG_LO1 = 0x10000000, TAG_HI0 = 0x0000000E, TAG_HI1 = 0;

Result case_xgkick(Capture& cap)
{
	load_program(xgkick_program());
	std::memset(don_vu1_data(), 0, 0x4000);
	// Packet A: tag at qword 8, data at qword 9.
	put_qword(8, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
	put_qword(9, 0x11223344, 0x55667788, 0x0000004C, 0);
	// Packet B wraps around the end of data memory: tag at qword 1023, data at qword 0.
	put_qword(1023, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
	put_qword(0, 0xA1A2A3A4, 0xB1B2B3B4, 0x0000004C, 0);
	uint32_t vi[32] = {};
	vi[1] = 8;
	don_vu1_set_regs(nullptr, vi);
	uint32_t cycles = don_vu1_execute(0, 1000);
	vi[1] = 1023;
	don_vu1_set_regs(nullptr, vi);
	cycles += don_vu1_execute(0, 1000);

	check(cap.packets.size() == 2, "[%s] xgkick: %zu packets delivered (expected 2)", g_engine_name, cap.packets.size());
	if (cap.packets.size() == 2)
	{
		uint8_t expect_a[32], expect_b[32];
		std::memcpy(expect_a, don_vu1_data() + 8 * 16, 32);
		std::memcpy(expect_b, don_vu1_data() + 1023 * 16, 16);
		std::memcpy(expect_b + 16, don_vu1_data(), 16); // joined across the wrap
		check(cap.packets[0].bytes.size() == 32 && std::memcmp(cap.packets[0].bytes.data(), expect_a, 32) == 0,
			"[%s] xgkick: packet at qword 8 = tag + 1 qword, bytes as in data memory", g_engine_name);
		check(cap.packets[1].bytes.size() == 32 && std::memcmp(cap.packets[1].bytes.data(), expect_b, 32) == 0,
			"[%s] xgkick: packet that wraps (tag at qword 1023, data at qword 0) arrives joined, 32 bytes", g_engine_name);
		uint32_t tag0;
		std::memcpy(&tag0, cap.packets[0].bytes.data(), 4);
		check(tag0 == TAG_LO0, "[%s] xgkick: first word of packet A = %08x", g_engine_name, tag0);
	}
	check(don_vu1_xgkick_count() >= 2, "[%s] xgkick: don_vu1_xgkick_count = %llu", g_engine_name,
		static_cast<unsigned long long>(don_vu1_xgkick_count()));
	return snapshot(cap, cycles);
}

// Program 4: multiplications that overflow. On the PS2 the result is the largest finite value with the
// sign of the true result, not infinity (an IEEE multiplication gives infinity).
Result case_saturate(Capture& cap)
{
	const std::vector<Pair> program = {
		{L_LQ(1, 0, 0), U_NOP},       // lq.xyzw vf1, 0(vi0)
		{L_LQ(2, 0, 1), U_NOP},       // lq.xyzw vf2, 1(vi0)
		{L_NOP, U_MUL(3, 1, 2)},      // mul.xyzw vf3, vf1, vf2
		{L_NOP, U_NOP},               // nop ; nop (the multiply takes 4 cycles)
		{L_NOP, U_NOP},               // nop ; nop
		{L_SQ(3, 0, 2), U_NOP},       // sq.xyzw vf3, 2(vi0)
		{L_NOP, U_NOP_E},             // nop ; nop [E]
		{L_NOP, U_NOP},               // delay slot of the end
	};
	load_program(program);
	put_qword(0, bits(1e30f), bits(-1e30f), bits(1e30f), bits(1e30f));
	put_qword(1, bits(1e30f), bits(1e30f), bits(-1e30f), bits(1.0f));
	put_qword(2, 0, 0, 0, 0);
	const uint32_t cycles = don_vu1_execute(0, 1000);
	check(qword_is(2, 0x7F7FFFFF, 0xFF7FFFFF, 0xFF7FFFFF, bits(1e30f)),
		"[%s] saturate: 1e30*1e30, -1e30*1e30, 1e30*-1e30, 1e30*1 = %s (expected 7f7fffff ff7fffff ff7fffff %08x)",
		g_engine_name, qword_text(2).c_str(), bits(1e30f));
	return snapshot(cap, cycles);
}

// Program 5: new microcode over a program that was already run (and compiled).
Result case_rewrite(Capture& cap)
{
	auto program_with = [](uint32_t upper_op) {
		return std::vector<Pair>{
			{L_LQ(1, 0, 0), U_NOP},
			{L_NOP, upper_op},
			{L_NOP, U_NOP},
			{L_NOP, U_NOP},
			{L_SQ(2, 0, 1), U_NOP},
			{L_NOP, U_NOP_E},
			{L_NOP, U_NOP},
		};
	};
	put_qword(0, bits(3.0f), bits(-5.0f), bits(0.5f), bits(8.0f));
	uint32_t cycles = 0;
	auto run = [&](const char* what, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
		put_qword(1, 0, 0, 0, 0);
		cycles += don_vu1_execute(0, 1000);
		check(qword_is(1, x, y, z, w), "[%s] rewrite: %s -> %s (expected %08x %08x %08x %08x)", g_engine_name, what,
			qword_text(1).c_str(), x, y, z, w);
	};
	load_program(program_with(U_ADD(2, 1, 1)));
	run("add (first run)", bits(6.0f), bits(-10.0f), bits(1.0f), bits(16.0f));
	run("add (second run, cached)", bits(6.0f), bits(-10.0f), bits(1.0f), bits(16.0f));
	load_program(program_with(U_MUL(2, 1, 1)));
	run("mul written over it", bits(9.0f), bits(25.0f), bits(0.25f), bits(64.0f));
	load_program(program_with(U_SUB(2, 1, 1)));
	run("sub written over it", 0, 0, 0, 0);
	load_program(program_with(U_ADD(2, 1, 1)));
	run("add written back", bits(6.0f), bits(-10.0f), bits(1.0f), bits(16.0f));
	// Overwrite one instruction only (the upper word of pair 1, byte offset 12).
	const uint32_t mul_word = U_MUL(2, 1, 1);
	don_vu1_write_micro(12, reinterpret_cast<const uint8_t*>(&mul_word), 4);
	run("one word (4 bytes) replaced with mul", bits(9.0f), bits(25.0f), bits(0.25f), bits(64.0f));
	return snapshot(cap, cycles);
}

// Program 2 run in slices of the cycle budget, against one run: same final state.
Result case_slices(Capture& cap, const Result& whole_run)
{
	load_program(loop_program(200));
	put_qword(0, bits(1.0f), bits(2.0f), bits(3.0f), bits(4.0f));
	put_qword(1, 0, 0, 0, 0);
	don_vu1_reset();
	uint32_t whole = don_vu1_execute(0, 100000);
	uint32_t whole_vf[128], whole_vi[32];
	don_vu1_get_regs(whole_vf, whole_vi);
	uint32_t whole_q[4];
	get_qword(1, whole_q);

	put_qword(1, 0, 0, 0, 0);
	don_vu1_reset();
	uint32_t total = don_vu1_execute(0, 40);
	int slices = 1;
	check(don_vu1_running() == 1, "[%s] slices: the program is still running after a budget of 40 cycles", g_engine_name);
	while (don_vu1_running() && slices < 100000)
	{
		total += don_vu1_continue(40);
		++slices;
	}
	uint32_t sliced_vf[128], sliced_vi[32];
	don_vu1_get_regs(sliced_vf, sliced_vi);
	uint32_t sliced_q[4];
	get_qword(1, sliced_q);
	check(slices > 5, "[%s] slices: the program took %d slices", g_engine_name, slices);
	check(std::memcmp(whole_q, sliced_q, 16) == 0 && whole_q[0] == bits(200.0f),
		"[%s] slices: stored result %s after slices == one run (x = 200.0)", g_engine_name, qword_text(1).c_str());
	bool same_vf = std::memcmp(whole_vf, sliced_vf, sizeof(whole_vf)) == 0;
	bool same_vi = true;
	for (int i = 0; i < 16; ++i)
		same_vi = same_vi && whole_vi[i] == sliced_vi[i];
	check(same_vf && same_vi, "[%s] slices: VF and VI00..VI15 after slices == after one run (vi1 = %u, vi2 = %u)",
		g_engine_name, sliced_vi[1], sliced_vi[2]);
	check(total == whole, "[%s] slices: total cycles in slices %u == in one run %u", g_engine_name, total, whole);
	(void)whole_run;
	return snapshot(cap, total);
}

// The floating point control register of the caller is restored, and the callback runs with it.
void case_mxcsr(Capture& cap)
{
	load_program(xgkick_program());
	put_qword(8, TAG_LO0, TAG_LO1, TAG_HI0, TAG_HI1);
	put_qword(9, 1, 2, 3, 4);
	uint32_t vi[32] = {};
	vi[1] = 8;
	don_vu1_set_regs(nullptr, vi);
	const uint32_t caller = (0x1F80u & ~0x6000u) | 0x2000u; // exceptions masked, round down, no FTZ/DAZ
	const uint32_t saved = _mm_getcsr();
	_mm_setcsr(caller);
	cap.packets.clear();
	don_vu1_execute(0, 1000);
	const uint32_t after = _mm_getcsr();
	_mm_setcsr(saved);
	check(after == caller, "[%s] mxcsr: caller's value %04x restored after execute (found %04x)", g_engine_name, caller, after);
	check(cap.packets.size() == 1 && cap.mxcsr_in_callback == caller,
		"[%s] mxcsr: the XGKICK callback ran with the caller's value (%04x)", g_engine_name, cap.mxcsr_in_callback);
}

// ---- driver ----------------------------------------------------------------------------------------

struct EngineRun
{
	Result add, loop, xgkick, saturate, rewrite, slices;
};

EngineRun run_engine(int engine)
{
	g_engine_name = engine == DON_VU_ENGINE_MICROVU ? "microvu" : "interp ";
	Capture cap;
	EngineRun run;
	const int rc = don_vu1_open_ex(engine, on_xgkick, &cap);
	check(rc == 0, "[%s] open_ex returned %d", g_engine_name, rc);
	if (rc != 0)
		return run;
	check(don_vu1_engine() == engine, "[%s] don_vu1_engine() = %d", g_engine_name, don_vu1_engine());
	check(don_vu1_open(nullptr, nullptr) != 0, "[%s] a second open is refused", g_engine_name);

	run.add = case_add(cap);
	cap.packets.clear();
	don_vu1_reset();
	run.loop = case_loop(cap);
	cap.packets.clear();
	don_vu1_reset();
	run.xgkick = case_xgkick(cap);
	cap.packets.clear();
	don_vu1_reset();
	run.saturate = case_saturate(cap);
	cap.packets.clear();
	don_vu1_reset();
	run.rewrite = case_rewrite(cap);
	cap.packets.clear();
	don_vu1_reset();
	run.slices = case_slices(cap, run.loop);
	cap.packets.clear();
	don_vu1_reset();
	case_mxcsr(cap);

	std::printf("      [%s] cycles: add %u, loop %u, xgkick %u, saturate %u, rewrite %u, slices %u\n", g_engine_name,
		run.add.cycles, run.loop.cycles, run.xgkick.cycles, run.saturate.cycles, run.rewrite.cycles, run.slices.cycles);
	don_vu1_close();
	return run;
}

void compare(const char* name, const Result& a, const Result& b)
{
	if (!a.valid || !b.valid)
	{
		check(false, "compare %s: a result is missing", name);
		return;
	}
	bool same_data = a.data == b.data;
	bool same_vf = std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0;
	bool same_vi = true;
	for (int i = 0; i < 16; ++i)
		same_vi = same_vi && a.vi[i] == b.vi[i];
	for (int i : {20, 21, 22, 23})
		same_vi = same_vi && a.vi[i] == b.vi[i];
	bool same_packets = a.packets.size() == b.packets.size();
	for (size_t i = 0; same_packets && i < a.packets.size(); ++i)
		same_packets = a.packets[i].bytes == b.packets[i].bytes;
	check(same_data && same_vf && same_vi && same_packets,
		"compare %s: microVU == interpreter (data memory %s, VF %s, VI/R/I/Q/P %s, GIF packets %s)", name,
		same_data ? "same" : "DIFFERENT", same_vf ? "same" : "DIFFERENT", same_vi ? "same" : "DIFFERENT",
		same_packets ? "same" : "DIFFERENT");
}

} // namespace

int main()
{
	check(don_vu_version() == DON_VU_VERSION, "don_vu_version() = %d", don_vu_version());
	don_vu1_close(); // must be harmless when nothing is open
	check(don_vu1_engine() == -1 && don_vu1_running() == 0, "nothing is open before the first open");

	const EngineRun jit = run_engine(DON_VU_ENGINE_MICROVU);
	const EngineRun interp = run_engine(DON_VU_ENGINE_INTERPRETER);

	compare("add", jit.add, interp.add);
	compare("loop", jit.loop, interp.loop);
	compare("xgkick", jit.xgkick, interp.xgkick);
	compare("saturate", jit.saturate, interp.saturate);
	compare("rewrite", jit.rewrite, interp.rewrite);
	compare("slices", jit.slices, interp.slices);

	std::printf("%s: %d checks, %d failed\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
