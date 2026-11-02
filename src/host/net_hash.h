// SPDX-License-Identifier: GPL-3.0-or-later
// The game's RAM hash, exposed to the host (T-509): the game override (src/game/don_overrides.cpp)
// installs its FNV-1a hash of the 32 MiB of guest RAM here; the host (host.cpp) reads it to send the
// netplay hashes (docs/knowledge/22-netplay.md sections 7 and 9). The function pointer is the same
// pattern as video_skip.h's player_probe: the game side writes it once, the host reads it, nothing else.
#pragma once

#include <cstdint>

namespace don_net
{

// The 64-bit hash of the guest RAM the game override computes (FNV-1a over the 32 MiB). Null when the
// program is not the game (or before applyDon ran). Called on the game thread.
inline uint64_t (*&ram_hash())(const uint8_t *ram)
{
    static uint64_t (*hash)(const uint8_t *) = nullptr;
    return hash;
}

} // namespace don_net
