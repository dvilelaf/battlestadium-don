#include "lockstep.h"

#include <algorithm>

namespace don_net
{

Lockstep::Lockstep(int players, int local_player, int delay)
    : m_players(std::clamp(players, 2, kMaxPlayers)),
      m_local(std::clamp(local_player, 0, kMaxPlayers - 1)),
      m_delay(std::clamp(delay, 1, 10)),
      m_last_confirmed(static_cast<uint32_t>(m_delay)),
      m_acked(static_cast<uint32_t>(m_delay))
{
}

uint32_t Lockstep::submit_local(uint32_t f, const NetPad &pad)
{
    const uint32_t target = f + static_cast<uint32_t>(m_delay);
    if (m_local_pads.find(target) == m_local_pads.end() && target > m_acked)
        m_local_pads[target] = pad;
    record(m_local, target, pad);
    return target;
}

void Lockstep::record(int player, uint32_t frame, const NetPad &pad)
{
    if (player < 0 || player >= m_players || !live(frame))
        return;
    Frame &fr = m_frames[frame];
    if (fr.confirmed || fr.recorded[player])
        return;
    fr.pads[player] = pad;
    fr.recorded[player] = true;
}

bool Lockstep::complete(uint32_t frame) const
{
    if (frame <= static_cast<uint32_t>(m_delay))
        return true;
    auto it = m_frames.find(frame);
    if (it == m_frames.end())
        return false;
    if (it->second.confirmed)
        return true;
    for (int p = 0; p < m_players; ++p)
        if (!it->second.recorded[p])
            return false;
    return true;
}

void Lockstep::confirm(uint32_t frame, const NetPad *pads)
{
    if (!live(frame))
        return;
    if (pads == nullptr && !complete(frame))
        return;
    Frame &fr = m_frames[frame];
    if (fr.confirmed)
        return;
    if (pads != nullptr)
        for (int p = 0; p < m_players; ++p)
        {
            fr.pads[p] = pads[p];
            fr.recorded[p] = true;
        }
    fr.confirmed = true;
    advance();
}

void Lockstep::advance()
{
    for (;;)
    {
        auto it = m_frames.find(m_last_confirmed + 1);
        if (it == m_frames.end() || !it->second.confirmed)
            return;
        ++m_last_confirmed;
    }
}

bool Lockstep::confirmed(uint32_t frame) const
{
    if (frame <= static_cast<uint32_t>(m_delay))
        return true;
    auto it = m_frames.find(frame);
    return it != m_frames.end() && it->second.confirmed;
}

NetPad Lockstep::confirmed_pad(uint32_t frame, int player) const
{
    if (player < 0 || player >= m_players || frame <= static_cast<uint32_t>(m_delay))
        return neutral_pad();
    auto it = m_frames.find(frame);
    if (it == m_frames.end() || !it->second.confirmed)
        return neutral_pad();
    return it->second.pads[player];
}

uint8_t Lockstep::missing(uint32_t frame) const
{
    if (frame <= static_cast<uint32_t>(m_delay))
        return 0;
    auto it = m_frames.find(frame);
    uint8_t bits = 0;
    for (int p = 0; p < m_players; ++p)
    {
        if (it != m_frames.end() && (it->second.confirmed || it->second.recorded[p]))
            continue;
        bits |= static_cast<uint8_t>(1u << p);
    }
    return bits;
}

std::vector<std::pair<uint32_t, NetPad>> Lockstep::unacked_local(std::size_t max) const
{
    std::vector<std::pair<uint32_t, NetPad>> out;
    for (auto it = m_local_pads.upper_bound(m_acked); it != m_local_pads.end() && out.size() < max; ++it)
        out.emplace_back(it->first, it->second);
    return out;
}

void Lockstep::ack_local(uint32_t through_frame)
{
    if (through_frame <= m_acked)
        return;
    m_acked = through_frame;
    m_local_pads.erase(m_local_pads.begin(), m_local_pads.upper_bound(through_frame));
}

std::vector<uint32_t> Lockstep::confirmed_since(uint32_t from, std::size_t max) const
{
    std::vector<uint32_t> out;
    if (max == 0 || from >= m_last_confirmed)
        return out;
    uint32_t first = from + 1;
    if (static_cast<uint64_t>(m_last_confirmed) - from > max)
        first = m_last_confirmed - static_cast<uint32_t>(max) + 1;
    for (uint32_t f = first; f <= m_last_confirmed; ++f)
        out.push_back(f);
    return out;
}

uint32_t Lockstep::last_confirmed() const
{
    return m_last_confirmed;
}

void Lockstep::forget_before(uint32_t frame)
{
    if (frame <= m_floor)
        return;
    m_floor = frame;
    m_frames.erase(m_frames.begin(), m_frames.lower_bound(frame));
}

} // namespace don_net
