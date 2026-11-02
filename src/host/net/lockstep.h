// Lockstep bookkeeping for netplay (D14, docs/knowledge/22-netplay.md section 4): which pads exist for which frame.
// Pure data: no time, no threads, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "net_pad.h"

namespace don_net
{

class Lockstep
{
public:
    Lockstep(int players, int local_player, int delay);              // players 2..4, delay 1..10 (clamped)
    // The local pad sampled at frame f; stored for frame f + delay. Returns that target frame.
    uint32_t submit_local(uint32_t f, const NetPad &pad);
    // Host: a player's pad for a frame (from INPUT or from submit_local). A second value for the same frame is ignored.
    void record(int player, uint32_t frame, const NetPad &pad);
    // Host: the frame's set is complete (every player recorded, or frame <= delay).
    bool complete(uint32_t frame) const;
    // Host: confirm a complete frame (pads == nullptr copies the recorded pads; ignored if the frame is not complete).
    // Client: from FRAMES (pads[players]). A frame already confirmed is not changed.
    void confirm(uint32_t frame, const NetPad *pads);
    bool confirmed(uint32_t frame) const;                            // frames <= delay are always confirmed (neutral)
    NetPad confirmed_pad(uint32_t frame, int player) const;          // neutral_pad() if the frame is not confirmed
    // Players whose pad for frame is still missing (bit p), on the host. 0 for a confirmed frame.
    uint8_t missing(uint32_t frame) const;
    // Client: the local pads to (re)send: frames (acked, last_submitted], at most max of them, oldest first.
    std::vector<std::pair<uint32_t, NetPad>> unacked_local(std::size_t max) const;
    void ack_local(uint32_t through_frame);                          // the host confirmed up to this frame
    // Host: the confirmed frames among (from, last_confirmed], oldest first; when more than max, the newest max.
    std::vector<uint32_t> confirmed_since(uint32_t from, std::size_t max) const;
    uint32_t last_confirmed() const;                                 // highest f with every frame 1..f confirmed
    void forget_before(uint32_t frame);                              // drop old frames (memory bound)

    int players() const { return m_players; }
    int local_player() const { return m_local; }
    int delay() const { return m_delay; }

private:
    struct Frame
    {
        NetPad pads[kMaxPlayers];
        bool recorded[kMaxPlayers] = {};
        bool confirmed = false;
    };
    bool live(uint32_t frame) const { return frame > static_cast<uint32_t>(m_delay) && frame >= m_floor; }
    void advance();

    int m_players, m_local, m_delay;
    uint32_t m_floor = 0;                // frames below this were forgotten
    uint32_t m_last_confirmed;
    uint32_t m_acked;
    std::map<uint32_t, Frame> m_frames;
    std::map<uint32_t, NetPad> m_local_pads; // target frame -> local pad, until acked
};

} // namespace don_net
