// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/Host/AudioStream.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/Host/AudioStream.h (2.8.2). The SPU2 core only
// includes it; the output backend is replaced by the DON SPSC ring
// (see don_spu2.cpp, spu2Output()).
#pragma once
