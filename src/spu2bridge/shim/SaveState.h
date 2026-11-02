// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/SaveState.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for the parts of PCSX2 pcsx2/SaveState.h (2.8.2) the SPU2 core uses.
#pragma once
#include "common/Pcsx2Types.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

struct freezeData
{
	int size;
	u8* data;
};
