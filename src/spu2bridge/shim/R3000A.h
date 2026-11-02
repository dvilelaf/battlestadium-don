// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/R3000A.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/R3000A.h (2.8.2). There is no IOP CPU: the only
// field the SPU2 core reads is psxRegs.cycle, which the DON glue sets to the
// guest IOP cycle stamped on each command (never host time).
#pragma once
#include "common/Pcsx2Types.h"

struct psxRegisters
{
	u64 cycle;
};

extern psxRegisters psxRegs;
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;
