// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/IopCounters.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/IopCounters.h (2.8.2). spu2sys.cpp CounterUpdate()
// schedules counter 6 as "call SPU2async() at the DMA deadline". The DON glue
// computes that deadline itself from Cores[].LastClock/DMAICounter, so these
// are write-only sinks.
#pragma once
#include "common/Pcsx2Types.h"

struct psxCounter
{
	u64 count, target;
	u32 rate, interrupt;
	u64 startCycle;
	s32 deltaCycles;
};

#define NUM_COUNTERS 8
extern psxCounter psxCounters[NUM_COUNTERS];
