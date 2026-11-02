// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/Config.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/Config.h (2.8.2). The SPU2 core only reads
// EmuConfig.SPU2 in PCSX2_DEVBUILD builds, which we never compile.
#pragma once
#include "common/Pcsx2Types.h"
struct Pcsx2Config;
