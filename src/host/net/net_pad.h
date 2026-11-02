// One pad's state as it crosses the network (D14, docs/knowledge/22-netplay.md): the fields of the runtime's
// Ps2HostPadState in a fixed 20-byte wire layout. This header is the contract every netplay piece shares.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "runtime/ps2_host_pad.h"

namespace don_net
{

// Wire layout, little-endian, 20 bytes:
//   0-1 buttons (active low, as Ps2HostPadState), 2 lx, 3 ly, 4 rx, 5 ry, 6 connected, 7 has_pressure,
//   8-19 pressure[12].
constexpr std::size_t kNetPadBytes = 20;
constexpr int kMaxPlayers = 4;

struct NetPad
{
    uint16_t buttons = 0xFFFFu;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    uint8_t connected = 0;
    uint8_t has_pressure = 0;
    uint8_t pressure[PS2_HOST_PAD_PRESSURE_COUNT] = {};

    bool operator==(const NetPad &o) const
    {
        return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry &&
               connected == o.connected && has_pressure == o.has_pressure &&
               std::memcmp(pressure, o.pressure, sizeof(pressure)) == 0;
    }
    bool operator!=(const NetPad &o) const { return !(*this == o); }
};

// The pad of a player who is in the session but has not pressed anything: connected, all released.
inline NetPad neutral_pad()
{
    NetPad p;
    p.connected = 1;
    return p;
}

inline NetPad to_net(const Ps2HostPadState &s)
{
    NetPad p;
    p.buttons = s.buttons;
    p.lx = s.lx;
    p.ly = s.ly;
    p.rx = s.rx;
    p.ry = s.ry;
    p.connected = s.connected;
    p.has_pressure = s.has_pressure;
    std::memcpy(p.pressure, s.pressure, sizeof(p.pressure));
    return p;
}

inline Ps2HostPadState from_net(const NetPad &p)
{
    Ps2HostPadState s;
    s.buttons = p.buttons;
    s.lx = p.lx;
    s.ly = p.ly;
    s.rx = p.rx;
    s.ry = p.ry;
    s.connected = p.connected;
    s.has_pressure = p.has_pressure;
    std::memcpy(s.pressure, p.pressure, sizeof(s.pressure));
    return s;
}

// Writes the 20 wire bytes at out; reads them back. No allocation, no checks beyond the fixed size.
inline void write_pad(const NetPad &p, uint8_t *out)
{
    out[0] = static_cast<uint8_t>(p.buttons & 0xFF);
    out[1] = static_cast<uint8_t>(p.buttons >> 8);
    out[2] = p.lx;
    out[3] = p.ly;
    out[4] = p.rx;
    out[5] = p.ry;
    out[6] = p.connected;
    out[7] = p.has_pressure;
    std::memcpy(out + 8, p.pressure, sizeof(p.pressure));
}

inline NetPad read_pad(const uint8_t *in)
{
    NetPad p;
    p.buttons = static_cast<uint16_t>(in[0] | (in[1] << 8));
    p.lx = in[2];
    p.ly = in[3];
    p.rx = in[4];
    p.ry = in[5];
    p.connected = in[6];
    p.has_pressure = in[7];
    std::memcpy(p.pressure, in + 8, sizeof(p.pressure));
    return p;
}

} // namespace don_net
