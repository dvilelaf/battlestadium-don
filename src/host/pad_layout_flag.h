// SPDX-License-Identifier: GPL-3.0-or-later
// "The port presents a multitap in port 0" (T-306): the one bit the host (host.cpp, from --players 4) hands
// to the game side (src/game/don_overrides.cpp). The game's multitap connection query (0x255EA0, replaced)
// answers non-zero for port 0 when this is set; the game's per-frame pad poll (0x138FB0) then opens slots 0 to
// 3 of port 0 and reads the four pads from there (docs/knowledge/14-iop-modules.md section E). The host sets
// it before the runtime starts and never changes it; the game thread only reads it.
#pragma once

#include <atomic>

namespace don_pads
{
inline std::atomic<bool> &multitap_in_port0()
{
    static std::atomic<bool> present{false};
    return present;
}
} // namespace don_pads
