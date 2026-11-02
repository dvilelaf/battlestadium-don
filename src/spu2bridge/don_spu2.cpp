// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The C API of libdon_spu2 (see don_spu2.h) on top of PCSX2's SPU2 core (v2.8.2, commit
// fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), whose files are compiled unmodified.
//
// This file replaces PCSX2's pcsx2/SPU2/spu2.cpp (audio backend, VMManager, GS capture) and the
// IOP-side hooks the SPU2 calls (IopDma.cpp spu2DMA4Irq/spu2DMA7Irq, IopIrq.cpp spu2Irq). It is
// adapted from the same replacement in the Ridge Racer V recompilation project,
// https://github.com/Fukami-SoftwareHub/RRV-Recomp, commit e8818121566cfaf3d361e38745860b0a9438e27b,
// tools/pcsx2-spu2/rrv_pcsx2_spu2.cpp (GPL-3.0-or-later). What was taken from there: the globals
// the core expects, the interrupt sinks, SPU2read/SPU2write and the DMA entry points (themselves
// adapted from PCSX2's spu2.cpp), the output stage with the DC filter, the power-on sequence and
// the way the core is stepped to the deadlines of the DMA countdowns. What is different: no worker
// thread and no command queue (one thread, time advances only through don_spu2_advance), the API of
// this project, and the output is clamped to -1..1.
//
// PCSX2 pieces taken as they are in spu2.cpp: StereoOut32::Empty, lClocks, SPU2read(),
// SPU2write(), SPU2{read,write}DMA{4,7}Mem(), SPU2interruptDMA{4,7}(), SPU2async(),
// SPU2::InternalReset(false), SPU2::Open(), DCFilter() and spu2Output().

#include "don_spu2.h"

#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/spu2.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopCounters.h"
#include "R3000A.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <deque>
#include <vector>

// ---------------------------------------------------------------------------
// Globals the PCSX2 SPU2 core expects from the rest of the emulator.
// ---------------------------------------------------------------------------

const StereoOut32 StereoOut32::Empty(0, 0); // upstream spu2.cpp
u64 lClocks = 0;                            // upstream spu2.cpp

psxRegisters psxRegs;
s32 psxNextDeltaCounter;
u64 psxNextStartCounter;
psxCounter psxCounters[NUM_COUNTERS];
u8 don_spu2_iopHw[0x10000];
u8 don_spu2_iopRam[DON_SPU2_IOP_RAM_SIZE];

namespace
{
constexpr u32 kTickInterval = 768; // IOP cycles per 48 kHz frame (spu2sys.cpp TickInterval)
constexpr u64 kMaxStep = u64{kTickInterval} * 1024; // far below TimeUpdate's sanity clamp (768*4800)
constexpr u32 kChcrBusy = 0x01000000;
constexpr u32 kChcrToSpu = 0x01000201;   // psxDmaGeneric: IOP -> SPU2
constexpr u32 kChcrFromSpu = 0x01000200; // psxDmaGeneric: SPU2 -> IOP
constexpr u32 kStagingBytesPerCore = DON_SPU2_IOP_RAM_SIZE / 2;
constexpr u32 kRingFrames = 1u << 18; // about 5.4 s of output

static_assert(kTickInterval == DON_SPU2_CYCLES_PER_FRAME, "frame period");

struct Event
{
	int type;
	uint64_t cycle;
};

struct Instance
{
	std::atomic<bool> open{false};
	bool suppress_sinks = false; // power-on sequence: interrupts must not be reported
	bool in_mix = false;         // inside a mixer tick (the interrupt belongs to that tick)
	u32 irq_bits_seen = 0;
	float dc_in[2] = {0, 0};
	float dc_out[2] = {0, 0};
	std::deque<Event> events;

	// Output ring: the core produces, one consumer thread may pull.
	std::vector<float> ring;
	alignas(64) std::atomic<u64> ring_head{0}; // frames written
	alignas(64) std::atomic<u64> ring_tail{0}; // frames read
	u64 dropped = 0;
};

Instance g_inst;

void log_error(const char* fmt, ...)
{
	std::fputs("[don-spu2] ", stderr);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}

void push_event(u64 cycle, int type)
{
	if (g_inst.suppress_sinks)
		return;
	g_inst.events.push_back(Event{type, cycle});
}

void don_mix()
{
	g_inst.in_mix = true;
	isa_native::spu2Mix();
	g_inst.in_mix = false;
}
} // namespace

// ---------------------------------------------------------------------------
// IOP interrupt sinks (upstream: IopIrq.cpp spu2Irq, IopDma.cpp spu2DMA4Irq/7).
// ---------------------------------------------------------------------------

// Upstream spu2.cpp SPU2interruptDMA4/7 (unchanged logic).
void SPU2interruptDMA4()
{
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void spu2Irq()
{
	// SetIrqCall()/CheckDMAProgress() set Spdif.Info bit (4 << core) right before calling us.
	g_inst.irq_bits_seen = (Spdif.Info >> 2) & 3;
	// Inside a mix tick the interrupt belongs to that tick's end cycle (lClocks); otherwise to the
	// current core time.
	const u64 cyc = g_inst.in_mix ? lClocks : psxRegs.cycle;
	push_event(cyc, DON_SPU2_EVENT_IRQ);
}

void spu2DMA4Irq()
{
	SPU2interruptDMA4();
	if (HW_DMA4_CHCR & kChcrBusy)
	{
		HW_DMA4_CHCR &= ~kChcrBusy;
		push_event(psxRegs.cycle, DON_SPU2_EVENT_DMA_END_0);
	}
}

void spu2DMA7Irq()
{
	SPU2interruptDMA7();
	if (HW_DMA7_CHCR & kChcrBusy)
	{
		HW_DMA7_CHCR &= ~kChcrBusy;
		push_event(psxRegs.cycle, DON_SPU2_EVENT_DMA_END_1);
	}
}

// ---------------------------------------------------------------------------
// Upstream spu2.cpp entry points (adapted: no logging, no AudioStream).
// ---------------------------------------------------------------------------

void SPU2readDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAwrite(pMem, size);
}

// Upstream spu2.cpp: true only after SPU2::Reset(true) (PS1 mode), which is never done here.
bool SPU2::IsRunningPSXMode()
{
	return false;
}

void SPU2async()
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;

	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}

	// Upstream compares the 16-bit offset with a full address, so this branch never fires in
	// PCSX2 2.8.2 either; kept verbatim for equivalence.
	if (omem == 0x1f9001AC)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
				SetIrqCall(i);
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);

		if (rmem >> 16 == 0x1f80)
			ret = Cores[0].ReadRegPS1(rmem);
		else if (mem >= 0x800)
			ret = spu2Ru16(mem);
		else
			ret = *(regtable[(mem >> 1)]);
	}

	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	TimeUpdate(psxRegs.cycle);

	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
		SPU2_FastWrite(rmem, value);
}

// Upstream spu2.cpp DCFilter() + spu2Output(), writing to our ring instead of
// AudioStream::WriteChunk. Declared in SPU2/defs.h; called once per frame by the mixer.
void spu2Output(StereoOut32 out)
{
	Instance& st = g_inst;
	const s32 cl = clamp_mix(out.Left);
	const s32 cr = clamp_mix(out.Right);

	float conv[2];
	conv[0] = static_cast<float>(cl) / INT16_MAX;
	conv[1] = static_cast<float>(cr) / INT16_MAX;

	// DC blocking high-pass filter (upstream DCFilter()).
	float o0 = (conv[0] - st.dc_in[0] + ((0.995f * st.dc_out[0])));
	float o1 = (conv[1] - st.dc_in[1] + ((0.995f * st.dc_out[1])));
	st.dc_in[0] = conv[0];
	st.dc_in[1] = conv[1];
	st.dc_out[0] = o0;
	st.dc_out[1] = o1;

	// The filter can overshoot the input range by a little: the API promises -1..1.
	o0 = std::min(1.0f, std::max(-1.0f, o0));
	o1 = std::min(1.0f, std::max(-1.0f, o1));

	const u64 head = st.ring_head.load(std::memory_order_relaxed);
	const u64 tail = st.ring_tail.load(std::memory_order_acquire);
	if (head - tail >= kRingFrames) // full: drop, never block the core
	{
		st.dropped++;
		return;
	}
	const u64 i = (head & (kRingFrames - 1)) * 2;
	st.ring[i] = o0;
	st.ring[i + 1] = o1;
	st.ring_head.store(head + 1, std::memory_order_release);
}

namespace
{
// Adapted from upstream spu2.cpp SPU2::InternalReset(false) + SPU2::Open(), plus a full power-on
// clear so a second instance in the same process starts from exactly the state the first one did.
void core_power_on()
{
	Instance& st = g_inst;
	// spu2sys.cpp keeps `static bool has_to_call_irq_dma[2]`, which no reset clears. It is cleared
	// when a DMA countdown expires, so run one 1-cycle countdown on core 0 with interrupts
	// disabled and the sinks suppressed.
	st.suppress_sinks = true;
	Cores[0].IRQEnable = false;
	Cores[1].IRQEnable = false;
	Cores[0].AutoDMACtrl = 0;
	Cores[0].ReadSize = 0;
	Cores[0].DMAICounter = 1;
	Cores[0].LastClock = 0;
	lClocks = 0;
	psxRegs.cycle = 1;
	TimeUpdate(psxRegs.cycle);
	st.suppress_sinks = false;

	std::memset(static_cast<void*>(Cores), 0, sizeof(Cores));
	std::memset(static_cast<void*>(DebugCores), 0, sizeof(DebugCores));
	std::memset(&Spdif, 0, sizeof(Spdif));
	std::memset(static_cast<void*>(pcm_cache_data), 0, sizeof(PcmCacheEntry) * pcm_BlockCount);
	std::memset(don_spu2_iopHw, 0, sizeof(don_spu2_iopHw));
	std::memset(don_spu2_iopRam, 0, sizeof(don_spu2_iopRam));
	std::memset(psxCounters, 0, sizeof(psxCounters));
	psxNextDeltaCounter = 0;
	psxNextStartCounter = 0;
	OutPos = 0;
	InputPos = 0;
	Cycles = 0;
	PlayMode = 0;
	psxRegs.cycle = 0;
	lClocks = psxRegs.cycle; // SPU2::Open()

	// SPU2::InternalReset(false)
	spu2Mix = don_mix; // wraps isa_native::spu2Mix to tag interrupts raised inside a tick
	ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
	ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);
	std::memset(spu2regs, 0, 0x010000);
	std::memset(_spu2mem, 0, 0x200000);
	std::memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
	std::memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.
	st.dc_in[0] = st.dc_in[1] = 0;
	st.dc_out[0] = st.dc_out[1] = 0;
	Spdif.Info = 0;
	Cores[0].Init(0);
	Cores[1].Init(1);

	st.events.clear();
	st.irq_bits_seen = 0;
	st.dropped = 0;
	// The output ring is NOT reset here (T-303): a reset comes from the game thread while the host thread pulls,
	// and clearing head and tail dropped whatever the host had not pulled yet (up to its 8 ms interval: 27 to 139
	// frames, different at every run), which made two recordings of the same route differ. Frames produced before
	// a reset are still pulled; don_spu2_open() empties the ring.
}

// Run the core to guest cycle t. Steps stop at each pending DMA countdown deadline, which is what
// PCSX2's IOP counter 6 (CounterUpdate) schedules, so DMA completion events land on their exact
// guest cycle regardless of how the caller batches its calls.
void run_to(u64 t)
{
	while (psxRegs.cycle < t)
	{
		u64 next = std::min<u64>(t, psxRegs.cycle + kMaxStep);
		for (int c = 0; c < 2; c++)
		{
			if (Cores[c].DMAICounter > 0)
			{
				const u64 dl = Cores[c].LastClock + static_cast<u64>(Cores[c].DMAICounter);
				if (dl > psxRegs.cycle && dl < next)
					next = dl;
			}
		}
		psxRegs.cycle = next;
		TimeUpdate(psxRegs.cycle);
	}
}

// After any guest-visible change (a register write may have cleared them) refresh which interrupt
// bits are already reported.
void refresh_irq_bits()
{
	g_inst.irq_bits_seen = (Spdif.Info >> 2) & 3;
}

bool in_window(uint32_t addr)
{
	return addr >= DON_SPU2_REG_BASE && addr <= DON_SPU2_REG_END;
}

// Sets the core's transfer area for a DMA of `halfwords` and returns the staging memory.
u16* dma_setup(int core, u32 halfwords, u32 chcr)
{
	const u32 staging = core ? kStagingBytesPerCore : 0;
	(core ? HW_DMA7_MADR : HW_DMA4_MADR) = staging;
	(core ? HW_DMA7_BCR : HW_DMA4_BCR) = ((halfwords + 1) / 2) | (1u << 16);
	(core ? HW_DMA7_CHCR : HW_DMA4_CHCR) = chcr;
	return reinterpret_cast<u16*>(iopPhysMem(staging));
}

// Bytes a DMA of `bytes` is limited to: even, and what the staging area holds.
uint32_t dma_limit(uint32_t bytes)
{
	if (bytes > kStagingBytesPerCore)
	{
		log_error("DMA of %u bytes truncated to %u", bytes, kStagingBytesPerCore);
		bytes = kStagingBytesPerCore;
	}
	return bytes & ~1u;
}
} // namespace

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

extern "C" {

int don_spu2_version(void)
{
	return DON_SPU2_API_VERSION;
}

int don_spu2_open(void)
{
	if (g_inst.open)
	{
		log_error("don_spu2_open: already open");
		return -1;
	}
	g_inst.ring.assign(size_t{kRingFrames} * 2, 0.0f);
	g_inst.ring_head.store(0);
	g_inst.ring_tail.store(0);
	core_power_on();
	g_inst.open = true;
	return 0;
}

void don_spu2_close(void)
{
	if (!g_inst.open)
		return;
	g_inst.open = false;
	g_inst.events.clear();
	g_inst.ring.clear();
	g_inst.ring.shrink_to_fit();
}

void don_spu2_reset(void)
{
	if (!g_inst.open)
		return;
	core_power_on();
}

void don_spu2_write16(uint32_t addr, uint16_t value)
{
	if (!g_inst.open || !in_window(addr))
		return;
	SPU2write(addr, value);
	refresh_irq_bits();
}

uint16_t don_spu2_read16(uint32_t addr)
{
	if (!g_inst.open || !in_window(addr))
		return 0;
	return SPU2read(addr);
}

int32_t don_spu2_dma_write(int core, const uint8_t* data, uint32_t bytes)
{
	if (core != 0 && core != 1)
		return -1;
	if (!g_inst.open || !data)
		return 0;
	bytes = dma_limit(bytes);
	const u32 halfwords = bytes / 2;
	u16* mem = dma_setup(core, halfwords, kChcrToSpu);
	std::memcpy(mem, data, bytes);
	if (core)
		SPU2writeDMA7Mem(mem, halfwords);
	else
		SPU2writeDMA4Mem(mem, halfwords);
	refresh_irq_bits();
	return static_cast<int32_t>(bytes);
}

int32_t don_spu2_dma_read(int core, uint8_t* dst, uint32_t bytes)
{
	if (core != 0 && core != 1)
		return -1;
	if (!g_inst.open || !dst)
		return 0;
	bytes = dma_limit(bytes);
	const u32 halfwords = bytes / 2;
	// The data the hardware would copy after the transfer delay is taken now, from the core's
	// transfer address: the two differ only if the mixer writes that range in between (the
	// 0x0000-0x27FF area the voices and the reverb use as working memory).
	const u32 start = Cores[core].TSA & 0xFFFFF;
	for (u32 i = 0; i < halfwords; i++)
	{
		const u16 v = static_cast<u16>(_spu2mem[(start + i) & 0xFFFFF]);
		dst[2 * i] = static_cast<uint8_t>(v & 0xFF);
		dst[2 * i + 1] = static_cast<uint8_t>(v >> 8);
	}
	u16* mem = dma_setup(core, halfwords, kChcrFromSpu);
	if (core)
		SPU2readDMA7Mem(mem, halfwords);
	else
		SPU2readDMA4Mem(mem, halfwords);
	refresh_irq_bits();
	return static_cast<int32_t>(bytes);
}

void don_spu2_advance(uint64_t iop_cycles)
{
	if (!g_inst.open)
		return;
	run_to(psxRegs.cycle + iop_cycles);
	refresh_irq_bits();
}

uint64_t don_spu2_cycle(void)
{
	return g_inst.open ? psxRegs.cycle : 0;
}

int don_spu2_poll_event(int* type, uint64_t* iop_cycle)
{
	if (!g_inst.open || g_inst.events.empty())
		return 0;
	const Event e = g_inst.events.front();
	g_inst.events.pop_front();
	if (type)
		*type = e.type;
	if (iop_cycle)
		*iop_cycle = e.cycle;
	return 1;
}

uint32_t don_spu2_pull_output(float* stereo, uint32_t frames)
{
	if (!g_inst.open || !stereo)
		return 0;
	Instance& st = g_inst;
	const u64 tail = st.ring_tail.load(std::memory_order_relaxed);
	const u64 head = st.ring_head.load(std::memory_order_acquire);
	const uint32_t avail = static_cast<uint32_t>(std::min<u64>(head - tail, frames));
	for (uint32_t i = 0; i < avail; i++)
	{
		const u64 j = ((tail + i) & (kRingFrames - 1)) * 2;
		stereo[2 * i] = st.ring[j];
		stereo[2 * i + 1] = st.ring[j + 1];
	}
	st.ring_tail.store(tail + avail, std::memory_order_release);
	return avail;
}

} // extern "C"
