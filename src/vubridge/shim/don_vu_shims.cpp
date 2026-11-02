// SPDX-License-Identifier: GPL-3.0-or-later
//
// The emulator state and subsystems that PCSX2's VU code references, reduced to what VU1 needs.
//
// Adapted from KillzoneRecomp, https://github.com/drakolordx7/KillzoneRecomp, commit
// 5f0700079f9ac4e14799d66d01747dbcdcec55f6, ext/kzvu/shim/KzvuShims.cpp (GPL-3.0-or-later).
// Kept: the globals, the CPU feature detection, the GIF PATH1 collector and GetGSPacketSize.
// Changed: Linux (cpuid.h) instead of MSVC/Windows; the code cache memory comes from don_vu_codemem.h;
// the code cache is only microVU1's and is
// mapped near this library's image; the GIF collector always copies and hands the callback one
// contiguous packet (no in-place delivery, no per-thread state).

#include "don_vu_prefix.h"

#include "common/CrashHandler.h"

#include "don_vu_codemem.h"

#include <cpuid.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- emulator globals ----------------------------------------------------------------------------
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware]; // only vif1Regs (TOP/ITOP/STAT) is used
alignas(16) cpuRegistersPack _cpuRegistersPack;         // cpuRegs.cycle
alignas(16) VURegs vuRegs[2];

DonVuEmuConfig don_vu_EmuConfig;
DonVuGifUnit don_vu_gifUnit;
DonVuVuThreadStub don_vu_vu1Thread;

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

// ---- CPU features (PCSX2 fills this from cpuinfo in GS/MultiISA.cpp) ------------------------------
static ProcessorFeatures DetectProcessorFeatures()
{
	ProcessorFeatures f = {};
	unsigned eax, ebx, ecx, edx;
	if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx))
		return f;
	const unsigned max_leaf = eax;
	__get_cpuid(1, &eax, &ebx, &ecx, &edx);
	const bool osxsave = (ecx >> 27) & 1;
	const bool avx = (ecx >> 28) & 1;
	f.hasFMA = (ecx >> 12) & 1;
	bool ymm_os = false;
	if (osxsave)
	{
		unsigned xcr0_lo, xcr0_hi;
		__asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
		ymm_os = (xcr0_lo & 6) == 6;
	}
	bool avx2 = false;
	if (max_leaf >= 7)
	{
		__cpuid_count(7, 0, eax, ebx, ecx, edx);
		avx2 = (ebx >> 5) & 1;
		f.hasBMI2 = (ebx >> 8) & 1;
	}
	if (avx && ymm_os)
		f.vectorISA = avx2 ? ProcessorFeatures::VectorISA::AVX2 : ProcessorFeatures::VectorISA::AVX;
	else
		f.vectorISA = ProcessorFeatures::VectorISA::SSE4;
	f.hasSlowGather = false;
	return f;
}
const ProcessorFeatures g_cpu = DetectProcessorFeatures();

// ---- interrupts / EE events ----------------------------------------------------------------------
static u32 s_pending_irq = 0;

void hwIntcIrq(int n)
{
	// VU1 raises INTC_VU1 (7) when it stops on a D or T bit; VPU_STAT says which bit.
	s_pending_irq |= 1u << n;
}

u32 donvu::TakeInterrupts(u32 mask)
{
	const u32 r = s_pending_irq & mask;
	s_pending_irq &= ~mask;
	return r;
}

void CPU_INT(EE_EventType n, s32 ecycle)
{
	// The interpreter schedules a VIF1 wake-up when an XGKICK finishes while VIF1 waits on the GIF
	// (VGW). The host's VIF1 is not stalled by this library, so there is nothing to wake.
}

// ---- JIT code memory -----------------------------------------------------------------------------
// microVU encodes the VU registers, VU memory and its own state as 32-bit displacements, either
// relative to the instruction pointer or to its "text pointer" register (a pointer into the
// library's image, x86emitter.cpp EmitSibMagic); an address farther than that fails an assert. All of
// that state is static data in this library's image, so the block of generated code is mapped within
// reach of it: asked for at addresses just after the image, the first one the kernel grants within
// 1 GiB of the image is kept. If none is granted the block is mapped anywhere (microVU then reaches the
// data through the text pointer only, which is what PCSX2 itself does).
static u8* s_code = nullptr;
static size_t s_code_size = 0;
static bool s_code_near = false;

static bool CodeIsNear(const u8* block, size_t size)
{
	const intptr_t image = reinterpret_cast<intptr_t>(&vuRegs);
	const intptr_t lo = reinterpret_cast<intptr_t>(block);
	const intptr_t hi = lo + static_cast<intptr_t>(size);
	constexpr intptr_t kReach = intptr_t{1} << 30;
	return (lo - image) > -kReach && (hi - image) < kReach && (image - lo) > -kReach;
}

bool donvu::AllocCodeCache(u32 bytes, std::string* err)
{
	if (s_code)
		return true;
	const uintptr_t image = reinterpret_cast<uintptr_t>(&vuRegs) & ~uintptr_t{0xFFFFF};
	void* block = nullptr;
	for (uintptr_t step = 0x1000000; step < (uintptr_t{1} << 29) && block == nullptr; step += 0x1000000)
	{
		// Above the image first, then below it.
		for (const intptr_t sign : {1, -1})
		{
			void* const hint = reinterpret_cast<void*>(sign > 0 ? image + step : image - step - bytes);
			void* const p = donvu::codemem::AllocExecutable(hint, bytes, nullptr);
			if (!p)
				continue;
			if (CodeIsNear(static_cast<u8*>(p), bytes))
			{
				block = p;
				break;
			}
			donvu::codemem::FreeExecutable(p, bytes);
		}
	}
	if (!block)
		block = donvu::codemem::AllocExecutable(nullptr, bytes, err);
	if (!block)
		return false;
	s_code = static_cast<u8*>(block);
	s_code_size = bytes;
	s_code_near = CodeIsNear(s_code, bytes);
	return true;
}

void donvu::FreeCodeCache()
{
	if (s_code)
		donvu::codemem::FreeExecutable(s_code, s_code_size);
	s_code = nullptr;
	s_code_size = 0;
}

u8* donvu::CodeCacheBase()
{
	return s_code;
}

u8* SysMemory::GetCodePtr(size_t offset)
{
	// Only microVU1 asks for code memory: [mVU1recOffset, mVU1recOffset + mVU1recSize].
	pxAssertRel(offset >= HostMemoryMap::mVU1recOffset && offset <= HostMemoryMap::mVU1recOffset + HostMemoryMap::mVU1recSize,
		"only microVU1 code memory is provided");
	return s_code + (offset - HostMemoryMap::mVU1recOffset);
}

// ---- GIF PATH1 (XGKICK) --------------------------------------------------------------------------
static donvu::XgkickFn s_xgkick_fn = nullptr;
static void* s_xgkick_user = nullptr;
static std::vector<u8> s_gif_buf; // bytes of the packet being collected
static u64 s_gif_packets = 0;
static FPControlRegister s_host_fpcr = FPControlRegister::GetCurrent();

void donvu::SetHostFPCR(FPControlRegister fpcr)
{
	s_host_fpcr = fpcr;
}

void donvu::SetXgkickCallback(XgkickFn fn, void* user)
{
	s_xgkick_fn = fn;
	s_xgkick_user = user;
}

void donvu::GifReset()
{
	s_gif_buf.clear();
	s_gif_packets = 0;
}

u64 donvu::GifPacketCount()
{
	return s_gif_packets;
}

// Hands one complete packet to the host callback.
static void GifDeliver(const u8* packet, u32 bytes)
{
	if (s_xgkick_fn)
	{
		// VU code runs with the VU rounding mode (chop, DAZ/FTZ); the host's GIF/GS code gets its own
		// MXCSR back for the duration of the call.
		const FPControlRegister vu_fpcr = FPControlRegister::GetCurrent();
		FPControlRegister::SetCurrent(s_host_fpcr);
		s_xgkick_fn(s_xgkick_user, packet, bytes);
		FPControlRegister::SetCurrent(vu_fpcr);
	}
	s_gif_packets++;
}

// Emits every complete packet (tags up to and including the one with EOP) at the front of the buffer.
static void GifEmitComplete()
{
	size_t pos = 0;
	size_t packet_start = 0;
	while (pos + 16 <= s_gif_buf.size())
	{
		Gif_Tag tag(&s_gif_buf[pos]);
		const size_t end = pos + 16 + tag.len;
		if (end > s_gif_buf.size())
			break;
		pos = end;
		if (tag.tag.EOP)
		{
			GifDeliver(&s_gif_buf[packet_start], static_cast<u32>(pos - packet_start));
			packet_start = pos;
		}
	}
	if (packet_start)
		s_gif_buf.erase(s_gif_buf.begin(), s_gif_buf.begin() + packet_start);
}

void DonVuGifPath::CopyGSPacketData(u8* pMem, u32 size, bool aligned)
{
	s_gif_buf.insert(s_gif_buf.end(), pMem, pMem + size);
}

u32 DonVuGifUnit::TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned)
{
	s_gif_buf.insert(s_gif_buf.end(), pMem, pMem + size);
	GifEmitComplete();
	return size;
}

// Same as Gif_Unit::GetGSPacketSize (Gif_Unit.h), with REC_VU1 meaning "microVU is the active VU1 core".
u32 DonVuGifUnit::GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset, u32 size, bool flush)
{
	u32 memMask = pathIdx ? ~0u : 0x3fffu;
	u32 curSize = 0;
	for (;;)
	{
		Gif_Tag gifTag(&pMem[offset & memMask]);
		incTag(offset, curSize, 16 + gifTag.len); // Tag + Data length
		if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
		{
			DevCon.Warning("Gif Unit - GS packet size exceeded VU memory size!");
			return 0;
		}
		if (curSize >= size)
			return size;
		if (((flush && gifTag.tag.EOP) || !flush) && (CHECK_XGKICKHACK || !REC_VU1))
			return curSize | ((u32)gifTag.tag.EOP << 31);
		if (gifTag.tag.EOP)
			return curSize;
	}
}

// ---- host hooks used by the pcsx2/common slice ---------------------------------------------------
void CrashHandler::WriteDumpForCaller()
{
}

// common/HostSys.cpp, which also holds the virtual memory API this library does not use.
[[noreturn]] void AbortWithMessage(const char* msg)
{
	std::fprintf(stderr, "[don-vu] fatal: %s\n", msg);
	std::abort();
}
