// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/rrv_spu2_prelude.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim: force-included before every PCSX2 SPU2 translation unit.
// Plays the role of PCSX2's precompiled header (PrecompiledHeader.h), which the
// SPU2 sources rely on for the basic integer types and helper macros.
#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"
#include "common/Assertions.h"
#include "IopMem.h" // upstream reaches iopPhysMem() through the PCH

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
