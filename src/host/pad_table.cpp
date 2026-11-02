// SPDX-License-Identifier: GPL-3.0-or-later
#include "pad_table.h"

#include <cstring>

namespace
{
    // Ps2HostPadState is 1 + 2 + 4 + 1 + 12 = 20 bytes of fields; packed by hand into three words so that the
    // record does not depend on the struct's padding.
    struct Packed
    {
        uint64_t w[3];
    };

    Packed pack(const Ps2HostPadState &s)
    {
        uint8_t bytes[24] = {};
        bytes[0] = s.connected ? 1 : 0;
        bytes[1] = static_cast<uint8_t>(s.buttons & 0xFF);
        bytes[2] = static_cast<uint8_t>(s.buttons >> 8);
        bytes[3] = s.rx;
        bytes[4] = s.ry;
        bytes[5] = s.lx;
        bytes[6] = s.ly;
        bytes[7] = s.has_pressure ? 1 : 0;
        std::memcpy(&bytes[8], s.pressure, PS2_HOST_PAD_PRESSURE_COUNT);
        Packed p;
        std::memcpy(p.w, bytes, sizeof(bytes));
        return p;
    }

    void unpack(const Packed &p, Ps2HostPadState &s)
    {
        uint8_t bytes[24];
        std::memcpy(bytes, p.w, sizeof(bytes));
        s.connected = bytes[0];
        s.buttons = static_cast<uint16_t>(bytes[1] | (bytes[2] << 8));
        s.rx = bytes[3];
        s.ry = bytes[4];
        s.lx = bytes[5];
        s.ly = bytes[6];
        s.has_pressure = bytes[7];
        std::memcpy(s.pressure, &bytes[8], PS2_HOST_PAD_PRESSURE_COUNT);
    }
} // namespace

PadTable::PadTable(uint32_t ports, uint32_t slots_port0)
{
    m_ports = ports > PS2_HOST_PAD_MAX_PORTS ? PS2_HOST_PAD_MAX_PORTS : ports;
    for (uint32_t p = 0; p < PS2_HOST_PAD_MAX_PORTS; ++p)
        m_slot_count[p] = kSlotsPerPort;
    m_slot_count[0] = slots_port0 > PS2_HOST_PAD_MAX_SLOTS ? PS2_HOST_PAD_MAX_SLOTS : slots_port0;

    // Every slot starts "not connected": the zero words unpack to connected = 0, but the buttons must read
    // as "nothing pressed" (0xFFFF, active low) even then.
    const Packed idle = pack(Ps2HostPadState{});
    for (auto &port : m_slots)
        for (auto &slot : port)
            for (int i = 0; i < 3; ++i)
                slot.words[i].store(idle.w[i], std::memory_order_relaxed);

    m_source.user = this;
    m_source.port_count = m_ports;
    for (uint32_t p = 0; p < m_ports; ++p)
        m_source.slots_per_port[p] = m_slot_count[p];
    m_source.read = [](void *user, int port, int slot, Ps2HostPadState *out) {
        static_cast<const PadTable *>(user)->read(static_cast<uint32_t>(port), static_cast<uint32_t>(slot), *out);
    };
    m_source.actuator = [](void *user, int port, int slot, uint8_t small_motor, uint8_t large_motor) {
        static_cast<PadTable *>(user)->set_actuators(static_cast<uint32_t>(port), static_cast<uint32_t>(slot),
                                                     small_motor, large_motor);
    };
}

void PadTable::set_actuators(uint32_t port, uint32_t slot, uint8_t small_motor, uint8_t large_motor)
{
    if (port >= m_ports || slot >= m_slot_count[port])
        return;
    m_actuators[port][slot].store(static_cast<uint16_t>(small_motor | (large_motor << 8)), std::memory_order_relaxed);
}

void PadTable::actuators(uint32_t port, uint32_t slot, uint8_t &small_motor, uint8_t &large_motor) const
{
    small_motor = 0;
    large_motor = 0;
    if (port >= m_ports || slot >= m_slot_count[port])
        return;
    const uint16_t v = m_actuators[port][slot].load(std::memory_order_relaxed);
    small_motor = static_cast<uint8_t>(v & 0xFF);
    large_motor = static_cast<uint8_t>(v >> 8);
}

void PadTable::publish(uint32_t port, uint32_t slot, const Ps2HostPadState &state)
{
    if (port >= m_ports || slot >= m_slot_count[port])
        return;
    Slot &s = m_slots[port][slot];
    const Packed p = pack(state);
    const uint32_t seq = s.seq.load(std::memory_order_relaxed);
    s.seq.store(seq + 1, std::memory_order_relaxed); // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < 3; ++i)
        s.words[i].store(p.w[i], std::memory_order_relaxed);
    s.seq.store(seq + 2, std::memory_order_release);
}

void PadTable::read(uint32_t port, uint32_t slot, Ps2HostPadState &out) const
{
    if (port >= m_ports || slot >= m_slot_count[port])
        return;
    const Slot &s = m_slots[port][slot];
    for (;;)
    {
        const uint32_t before = s.seq.load(std::memory_order_acquire);
        if ((before & 1u) == 0)
        {
            Packed p;
            for (int i = 0; i < 3; ++i)
                p.w[i] = s.words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) == before)
            {
                unpack(p, out);
                return;
            }
        }
        // The writer is in the middle of a publish (a few instructions): try again.
    }
}
