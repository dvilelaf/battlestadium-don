// SPDX-License-Identifier: GPL-3.0-or-later
//
// Types of the shim objects that PCSX2's VU code is redirected to (see don_vu_prefix.h), plus the
// glue between don_vu.cpp and shim/don_vu_shims.cpp.
//
// Adapted from KillzoneRecomp, https://github.com/drakolordx7/KillzoneRecomp, commit
// 5f0700079f9ac4e14799d66d01747dbcdcec55f6, ext/kzvu/shim/include/kzvu_shim.h (GPL-3.0-or-later).
// Kept: the reduced EmuConfig, the GIF unit that collects PATH1 packets, the MTVU stub. Changed:
// names (don_vu), no per-thread VU0 alias, no VU0 and no XGKICK in-place delivery, and the host
// callback takes a contiguous copy of the packet.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// ---- EmuConfig: only the fields the VU code reads ------------------------------------------------
struct DonVuEmuConfig
{
	struct
	{
		struct
		{
			bool EnableEE = false;
			bool EnableIOP = false;
			bool EnableVU0 = false;
			bool EnableVU1 = true; // REC_VU1: microVU (true) or the interpreter (false)
			bool vu0Overflow = true;
			bool vu0ExtraOverflow = false;
			bool vu0SignOverflow = false;
			bool vu0Underflow = false;
			bool vu1Overflow = true;
			bool vu1ExtraOverflow = false;
			bool vu1SignOverflow = false;
			bool vu1Underflow = false;
			bool fpuOverflow = true;
			bool fpuExtraOverflow = false;
			bool fpuFullMode = false;
			bool EnableEECache = false;
			bool EnableFastmem = false;
		} Recompiler;

		FPControlRegister FPUFPCR;
		FPControlRegister FPUDivFPCR;
		FPControlRegister VU0FPCR;
		FPControlRegister VU1FPCR;
	} Cpu;

	struct
	{
		bool FpuMulHack = false;
		bool VuAddSubHack = false;
		bool IbitHack = false;
		bool VUSyncHack = false;
		bool VUOverflowHack = false;
		bool XgKickHack = false;
		bool FullVU0SyncHack = false;
	} Gamefixes;

	struct
	{
		bool vuFlagHack = true;
		bool vuThread = false;
		bool vu1Instant = true;
		s8 EECycleRate = 0;
		u8 EECycleSkip = 0;
	} Speedhacks;
};
extern DonVuEmuConfig don_vu_EmuConfig;

// ---- gifUnit: XGKICK packets are collected here and handed to the host ---------------------------
// microVU and the interpreter call the same three Gif_Unit members PCSX2 uses for PATH1. Data arrives
// in pieces (a 16 KiB wrap, or tag by tag from the interpreter's timed transfer); complete packets
// (up to and including the tag with EOP) are passed to the host callback in one call.
struct DonVuGifPath
{
	void CopyGSPacketData(u8* pMem, u32 size, bool aligned = false);
};

struct DonVuGifUnit
{
	DonVuGifPath gifPath[3];

	u32 GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset = 0, u32 size = ~0u, bool flush = false);
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned = false);
};
extern DonVuGifUnit don_vu_gifUnit;

// ---- vu1Thread: MTVU is compiled out (THREAD_VU1 == false); this only has to satisfy the compiler
struct DonVuVuThreadStub
{
	alignas(16) VIFregisters vifRegs;
	u32 vuFBRST = 0;
	std::atomic<u32> mtvuInterrupts{0};

	bool IsOpen() const { return false; }
	void Open() {}
	void Close() {}
	void WaitVU() {}
	void Get_MTVUChanges() {}
	void KickStart() {}
	void ExecuteVU(u32, u32, u32, u32) {}
};
extern DonVuVuThreadStub don_vu_vu1Thread;

// ---- glue between the API (don_vu.cpp) and the shims ---------------------------------------------
namespace donvu
{
	using XgkickFn = void (*)(void* user, const u8* packet, u32 bytes);

	void SetXgkickCallback(XgkickFn fn, void* user);
	void SetHostFPCR(FPControlRegister fpcr); // MXCSR the XGKICK callback runs with (the caller's)
	void GifReset();                          // drop any partially collected packet
	u64 GifPacketCount();                     // packets delivered since open

	bool AllocCodeCache(u32 bytes, std::string* err); // executable block, near the library's image
	void FreeCodeCache();
	u8* CodeCacheBase();

	u32 TakeInterrupts(u32 mask); // INTC_VU1 bits raised (D/T stop) since the last call
} // namespace donvu
