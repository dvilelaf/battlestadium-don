// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/IopMem.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/IopMem.h (2.8.2). iopPhysMem() maps into a private
// 2 MiB buffer that the DON glue uses as DMA staging memory (payloads are
// copied there at submit time, so the SPU2 core's lazy DMAPtr stays valid).
#pragma once
#include "common/Pcsx2Types.h"

static constexpr u32 DON_SPU2_IOP_RAM_SIZE = 0x200000;
extern u8 don_spu2_iopRam[DON_SPU2_IOP_RAM_SIZE];

static inline u8* iopPhysMem(u32 addr)
{
	return &don_spu2_iopRam[addr & (DON_SPU2_IOP_RAM_SIZE - 1)];
}
