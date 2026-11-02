// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C API of libdon_vu (see don_vu.h) on top of PCSX2's microVU1 and VU1 interpreter (v2.8.2, commit
// fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), whose files are compiled unmodified.
//
// Adapted from KillzoneRecomp, https://github.com/drakolordx7/KillzoneRecomp, commit
// 5f0700079f9ac4e14799d66d01747dbcdcec55f6, ext/kzvu/src/kzvu.cpp (GPL-3.0-or-later). Kept: the way
// the engines are driven (vu1ExecMicro's sequence of VPU_STAT and TPC, the cycle accounting, the budget
// and continue semantics), the VU floating point control register and its scoped switch, the
// seeding of the pipelined flags, and the static memories that sit within reach of the generated
// code. Changed: Linux, the API of this project, VU1 only, runtime choice of engine, and the
// executable memory (shim/don_vu_shims.cpp).
//
// Compiled with the don_vu_prefix.h forced include, so EmuConfig, gifUnit, cpuRegs, VU1... below are
// PCSX2's names redirected to the shim objects.

#include "don_vu.h"

#include "common/FPControl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// Micro and data memory of VU0 and VU1. Static, so they sit inside this library's image, within reach of
// the generated code: microVU encodes the addresses of constant VU memory accesses as 32-bit
// displacements (see the note in shim/don_vu_shims.cpp). VU0's are only there because PCSX2's code
// dereferences VU0.Mem / VU0.Micro in places; VU0 never runs.
alignas(64) static u8 s_vu0_micro[VU0_PROGSIZE];
alignas(64) static u8 s_vu0_mem[VU0_MEMSIZE];
alignas(64) static u8 s_vu1_micro[VU1_PROGSIZE];
alignas(64) static u8 s_vu1_mem[VU1_MEMSIZE];

static bool s_open = false;
static int s_engine = -1;
static u64 s_total_cycles = 0;
static int s_clamp_mode = 1;
static bool s_flag_hack = true;
static bool s_ibit_hack = false;
static bool s_xgkick_hack = false;

// The VU's floating point control register, as PCSX2 (Pcsx2Config.cpp, VU0FPCR / VU1FPCR): exceptions
// masked, denormals are zero, flush to zero, round toward zero.
static constexpr FPControlRegister DON_VU_FPCR = FPControlRegister::GetDefault()
													 .DisableExceptions()
													 .SetDenormalsAreZero(true)
													 .SetFlushToZero(true)
													 .SetRoundMode(FPRoundMode::ChopZero);

static void ApplyOptions()
{
	auto& rec = EmuConfig.Cpu.Recompiler;
	rec.EnableVU1 = s_engine == DON_VU_ENGINE_MICROVU;
	rec.EnableVU0 = false;
	// PCSX2's GameDatabase clamping modes (vuClampMode): 1 sets Overflow, 2 also ExtraOverflow, 3 also SignOverflow.
	rec.vu0Overflow = rec.vu1Overflow = s_clamp_mode >= 1;
	rec.vu0ExtraOverflow = rec.vu1ExtraOverflow = s_clamp_mode >= 2;
	rec.vu0SignOverflow = rec.vu1SignOverflow = s_clamp_mode >= 3;
	rec.vu0Underflow = rec.vu1Underflow = false;
	EmuConfig.Gamefixes.IbitHack = s_ibit_hack;
	EmuConfig.Gamefixes.XgKickHack = s_xgkick_hack;
	EmuConfig.Speedhacks.vuFlagHack = s_flag_hack;
	EmuConfig.Speedhacks.vuThread = false; // MTVU is compiled out
	EmuConfig.Speedhacks.vu1Instant = true;
	EmuConfig.Cpu.FPUFPCR = DON_VU_FPCR;
	EmuConfig.Cpu.FPUDivFPCR = DON_VU_FPCR;
	EmuConfig.Cpu.VU0FPCR = DON_VU_FPCR;
	EmuConfig.Cpu.VU1FPCR = DON_VU_FPCR;

	CpuVU1 = s_engine == DON_VU_ENGINE_MICROVU ? static_cast<BaseVUmicroCPU*>(&CpuMicroVU1) : static_cast<BaseVUmicroCPU*>(&CpuIntVU1);
	CpuVU0 = &CpuIntVU0;
}

static void ResetRegs()
{
	for (int i = 0; i < 2; i++)
	{
		std::memset(static_cast<void*>(&vuRegs[i]), 0, sizeof(VURegs));
		vuRegs[i].idx = i;
		vuRegs[i].VF[0].f.w = 1.0f;
	}
	VU0.Micro = s_vu0_micro;
	VU0.Mem = s_vu0_mem;
	VU1.Micro = s_vu1_micro;
	VU1.Mem = s_vu1_mem;
	std::memset(static_cast<void*>(&_cpuRegistersPack), 0, sizeof(_cpuRegistersPack));
	vif1Regs.top = 0;
	vif1Regs.itop = 0;
	vif1Regs.stat._u32 = 0;
	donvu::GifReset();
}

// Switches MXCSR to the VU's control register for the duration of a run and back to the caller's
// afterwards. PCSX2 runs its EE thread with the VU's value, so microVU's entry code does not switch it
// itself (it does so only when the "EE" value differs, see mvuNeedsFPCRUpdate in microVU_Execute.inl);
// the XGKICK callback gets the caller's value back (shim/don_vu_shims.cpp, GifDeliver).
class ScopedVuFPCR
{
public:
	ScopedVuFPCR()
		: m_host(FPControlRegister::GetCurrent())
	{
		donvu::SetHostFPCR(m_host);
		FPControlRegister::SetCurrent(EmuConfig.Cpu.VU1FPCR);
	}
	~ScopedVuFPCR() { FPControlRegister::SetCurrent(m_host); }

private:
	FPControlRegister m_host;
};

static u32 RunCycles(u32 budget)
{
	cpuRegs.cycle = VU1.cycle;
	const u64 start = VU1.cycle;
	CpuVU1->Execute(budget);
	const u64 used = VU1.cycle - start;
	s_total_cycles += used;
	return static_cast<u32>(used);
}

// microVU keeps four pipelined instances of each flag; the status flag instances are in microVU's own
// bit layout (mVUallocSFLAGd), the same conversion PCSX2's vu0ExecMicro uses.
static u32 DenormalizeMicroStatus(u32 status)
{
	return ((status >> 3) & 0x18u) | ((status >> 11) & 0x1800u) | ((status >> 14) & 0x3cf0000u);
}

static void SeedFlags(u32 status, u32 mac, u32 clip)
{
	VU1.VI[REG_STATUS_FLAG].UL = status;
	VU1.VI[REG_MAC_FLAG].UL = mac;
	VU1.VI[REG_CLIP_FLAG].UL = clip;
	VU1.statusflag = status;
	VU1.macflag = mac;
	VU1.clipflag = clip;
	for (u32& f : VU1.micro_statusflags)
		f = DenormalizeMicroStatus(status);
	for (u32& f : VU1.micro_macflags)
		f = mac;
	for (u32& f : VU1.micro_clipflags)
		f = clip;
}

// ---- lifetime ------------------------------------------------------------------------------------

extern "C" {

int don_vu_version(void)
{
	return DON_VU_VERSION;
}

int don_vu1_open(DonVuXgkick cb, void* user)
{
	return don_vu1_open_ex(DON_VU_ENGINE_MICROVU, cb, user);
}

int don_vu1_open_ex(int engine, DonVuXgkick cb, void* user)
{
	if (s_open)
	{
		std::fprintf(stderr, "[don-vu] a VU1 is already open\n");
		return 1;
	}
	if (engine != DON_VU_ENGINE_MICROVU && engine != DON_VU_ENGINE_INTERPRETER)
	{
		std::fprintf(stderr, "[don-vu] unknown engine %d\n", engine);
		return 1;
	}
	s_engine = engine;
	x86Emitter::use_avx = g_cpu.vectorISA >= ProcessorFeatures::VectorISA::AVX;

	if (engine == DON_VU_ENGINE_MICROVU)
	{
		std::string err;
		if (!donvu::AllocCodeCache(HostMemoryMap::mVU1recSize, &err))
		{
			std::fprintf(stderr, "[don-vu] %s\n", err.c_str());
			s_engine = -1;
			return 1;
		}
	}

	std::memset(s_vu0_micro, 0, sizeof(s_vu0_micro));
	std::memset(s_vu0_mem, 0, sizeof(s_vu0_mem));
	std::memset(s_vu1_micro, 0, sizeof(s_vu1_micro));
	std::memset(s_vu1_mem, 0, sizeof(s_vu1_mem));
	ResetRegs();
	ApplyOptions();
	donvu::SetXgkickCallback(cb, user);
	if (engine == DON_VU_ENGINE_MICROVU)
	{
		CpuMicroVU1.Reserve();
		CpuMicroVU1.Reset();
	}
	CpuIntVU1.Reset();
	s_total_cycles = 0;
	s_open = true;
	return 0;
}

void don_vu1_close(void)
{
	if (!s_open)
		return;
	if (s_engine == DON_VU_ENGINE_MICROVU)
		CpuMicroVU1.Shutdown();
	donvu::FreeCodeCache();
	donvu::SetXgkickCallback(nullptr, nullptr);
	s_open = false;
	s_engine = -1;
}

void don_vu1_reset(void)
{
	if (!s_open)
		return;
	ResetRegs();
	if (s_engine == DON_VU_ENGINE_MICROVU)
		CpuMicroVU1.Reset();
	CpuIntVU1.Reset();
}

int don_vu1_set_option(int option, int value)
{
	int* clamp = nullptr;
	switch (option)
	{
		case DON_VU_OPT_CLAMP_MODE:
			if (value < 0 || value > 3)
				return 1;
			clamp = &s_clamp_mode;
			break;
		case DON_VU_OPT_FLAG_HACK:
		case DON_VU_OPT_IBIT_HACK:
		case DON_VU_OPT_XGKICK_HACK:
			if (value < 0 || value > 1)
				return 1;
			break;
		default:
			return 1;
	}
	const bool changed = (option == DON_VU_OPT_CLAMP_MODE && s_clamp_mode != value) ||
						 (option == DON_VU_OPT_FLAG_HACK && s_flag_hack != (value != 0)) ||
						 (option == DON_VU_OPT_IBIT_HACK && s_ibit_hack != (value != 0)) ||
						 (option == DON_VU_OPT_XGKICK_HACK && s_xgkick_hack != (value != 0));
	if (clamp)
		*clamp = value;
	else if (option == DON_VU_OPT_FLAG_HACK)
		s_flag_hack = value != 0;
	else if (option == DON_VU_OPT_IBIT_HACK)
		s_ibit_hack = value != 0;
	else
		s_xgkick_hack = value != 0;
	if (s_open)
	{
		ApplyOptions();
		if (changed && s_engine == DON_VU_ENGINE_MICROVU)
			CpuMicroVU1.Reset();
	}
	return 0;
}

int don_vu1_engine(void)
{
	return s_open ? s_engine : -1;
}

// ---- memory --------------------------------------------------------------------------------------

uint8_t* don_vu1_data(void)
{
	return s_vu1_mem;
}

const uint8_t* don_vu1_micro(void)
{
	return s_vu1_micro;
}

static void WriteMicroPart(u32 offset, const u8* src, u32 size)
{
	if (size == 0)
		return;
	std::memcpy(s_vu1_micro + offset, src, size);
	if (s_open)
		CpuVU1->Clear(offset, size); // microVU: "cleared": the next start checks the programs it has against the new contents
}

void don_vu1_write_micro(uint32_t addr_bytes, const uint8_t* data, uint32_t bytes)
{
	if (!data)
		return;
	u32 offset = addr_bytes & VU1_PROGMASK;
	if (bytes > VU1_PROGSIZE)
		bytes = VU1_PROGSIZE;
	const u32 first = std::min<u32>(bytes, VU1_PROGSIZE - offset);
	WriteMicroPart(offset, data, first);
	WriteMicroPart(0, data + first, bytes - first);
}

// ---- registers -----------------------------------------------------------------------------------

void don_vu1_set_regs(const void* vf, const void* vi)
{
	if (vf)
	{
		const u32* in = static_cast<const u32*>(vf);
		std::memcpy(VU1.VF[1].UL, in + 4, 31 * 16); // VF00 is fixed
	}
	if (vi)
	{
		const u32* in = static_cast<const u32*>(vi);
		for (int i = 1; i < 16; i++)
			VU1.VI[i].UL = in[i] & 0xffff;
		SeedFlags(in[REG_STATUS_FLAG], in[REG_MAC_FLAG], in[REG_CLIP_FLAG]);
		VU1.VI[REG_R].UL = in[REG_R];
		VU1.VI[REG_I].UL = in[REG_I];
		VU1.VI[REG_Q].UL = in[REG_Q];
		VU1.VI[REG_P].UL = in[REG_P];
		VU1.pending_q = in[REG_Q];
		VU1.pending_p = in[REG_P];
		VU1.VI[REG_TPC].UL = in[REG_TPC] & 0x7FF;
	}
}

void don_vu1_get_regs(void* vf, void* vi)
{
	if (vf)
		std::memcpy(vf, VU1.VF, 32 * 16);
	if (vi)
	{
		u32* out = static_cast<u32*>(vi);
		std::memset(out, 0, 32 * sizeof(u32));
		for (int i = 0; i < 16; i++)
			out[i] = VU1.VI[i].UL & 0xffff;
		out[REG_STATUS_FLAG] = VU1.VI[REG_STATUS_FLAG].UL;
		out[REG_MAC_FLAG] = VU1.VI[REG_MAC_FLAG].UL;
		out[REG_CLIP_FLAG] = VU1.VI[REG_CLIP_FLAG].UL;
		out[REG_R] = VU1.VI[REG_R].UL;
		out[REG_I] = VU1.VI[REG_I].UL;
		out[REG_Q] = VU1.VI[REG_Q].UL;
		out[REG_P] = VU1.VI[REG_P].UL;
		out[REG_TPC] = VU1.VI[REG_TPC].UL;
		out[REG_FBRST] = VU1.VI[REG_FBRST].UL;
		out[REG_VPU_STAT] = VU0.VI[REG_VPU_STAT].UL;
	}
}

void don_vu1_get_acc(uint32_t acc[4])
{
	std::memcpy(acc, VU1.ACC.UL, 16);
}

void don_vu1_set_acc(const uint32_t acc[4])
{
	std::memcpy(VU1.ACC.UL, acc, 16);
}

void don_vu1_set_top(uint32_t top, uint32_t itop)
{
	vif1Regs.top = top & 0x3ff;
	vif1Regs.itop = itop & 0x3ff;
}

// ---- execution -----------------------------------------------------------------------------------

uint32_t don_vu1_execute(uint32_t start_pc_bytes, uint32_t max_cycles)
{
	if (!s_open)
		return 0;
	const ScopedVuFPCR fpcr;

	// vu1ExecMicro(): a program that is still running is finished first.
	u32 used = 0;
	if (VU0.VI[REG_VPU_STAT].UL & 0x100)
	{
		used += RunCycles(vu1RunCycles);
		if (VU0.VI[REG_VPU_STAT].UL & 0x100)
		{
			std::fprintf(stderr, "[don-vu] force-stopping the previous VU1 program, it ran for too long\n");
			VU0.VI[REG_VPU_STAT].UL &= ~0x100;
		}
	}

	VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
	VU0.VI[REG_VPU_STAT].UL |= 0x0100;
	if (start_pc_bytes != 0xFFFFFFFFu)
		VU1.VI[REG_TPC].UL = (start_pc_bytes >> 3) & 0x7FF;
	CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
	used += RunCycles(max_cycles);
	return used;
}

uint32_t don_vu1_continue(uint32_t max_cycles)
{
	if (!s_open || !(VU0.VI[REG_VPU_STAT].UL & 0x100))
		return 0;
	const ScopedVuFPCR fpcr;
	return RunCycles(max_cycles);
}

int don_vu1_running(void)
{
	return s_open && (VU0.VI[REG_VPU_STAT].UL & 0x100) ? 1 : 0;
}

uint64_t don_vu1_total_cycles(void)
{
	return s_total_cycles;
}

uint64_t don_vu1_xgkick_count(void)
{
	return donvu::GifPacketCount();
}

} // extern "C"
