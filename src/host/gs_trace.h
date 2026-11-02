// SPDX-License-Identifier: GPL-3.0-or-later
// T-202 step 4a: record one frame's GIF transfers, to compare the port's packets with the console's
// GS dump (tools/verify/gif_stats.py reads both).
//
//     DON_GS_PACKETS=<file>          record one frame's transfers into this file
//     DON_GS_PACKETS_FRAME=<n>       which vertical blank (1-based, like the host's frame counter)
//
// Off by default: with DON_GS_PACKETS unset the two calls below return immediately (one getenv at
// startup), and nothing is allocated.
//
// Format (the same tool reads the console's PCSX2 dump and this):
//     "DONGS1\n" u32 frame, then one record per transfer: path u8, size u32 (little endian), bytes
// `path` is the runtime's own path: 1 = the VU1's XGKICK, 2 = VIF1 direct, 3 = the GIF channel
// (src/host/gs_glue.cpp, Ps2HostGs::gif_transfer). A PCSX2 dump cannot tell those apart
// (docs/knowledge/15-gs-dumps.md), so the port's capture is the only side that shows which path a
// packet came through.
//
// Threading: both calls happen on the game thread (the GS glue's thread_begin/thread_end thread), so
// no locking is needed. The file is written at the end of the recorded frame.
#pragma once

#include <cstdint>

namespace gs_trace
{

// Game thread, from the GS glue's gif_transfer, for every transfer it hands to the GS library.
void transfer(int path, const uint8_t *data, uint32_t bytes);

// Game thread, from the GS glue's vsync, with the number of the vertical blank that just ended. The
// transfers of the frame are those seen since the previous call.
void vsync(uint64_t frame);

// True when DON_GS_PACKETS is set (one getenv, cached).
bool enabled();

} // namespace gs_trace
