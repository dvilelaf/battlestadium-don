// Adapted from https://github.com/Fukami-SoftwareHub/RRV-Recomp (commit e8818121566cfaf3d361e38745860b0a9438e27b),
// tools/pcsx2-spu2/shim/IopDma.h, GPL-3.0-or-later. Renamed rrv_ -> don_ and RRV -> DON; comments adjusted accordingly.
// SPDX-License-Identifier: GPL-3.0+
// DON shim for PCSX2 pcsx2/IopDma.h (2.8.2): the three SPU2 -> IOP interrupt
// sinks. Implemented by the DON glue, which turns them into timestamped events.
#pragma once

extern void spu2DMA4Irq();
extern void spu2DMA7Irq();
extern void spu2Irq();
