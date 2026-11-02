// SPDX-License-Identifier: GPL-3.0-or-later
// The pad state shared between the thread that polls the devices (the main thread) and the game
// thread (T-110; T-111 fills it from SDL3).
//
//   main thread:  table.publish(port, slot, state)    any time, any number of times
//   game thread:  the runtime calls the Ps2HostPad::read callback (PadTable::source()), which only
//                 copies the last published state: no lock, no allocation, no call into the runtime.
//
// Each slot is a sequence-locked record of atomic words: the reader retries while the writer is in the
// middle of a publish, so it neither blocks nor sees a torn state. One writer thread, any readers.
#pragma once

#include "runtime/ps2_host_pad.h"

#include <atomic>
#include <cstdint>

class PadTable
{
public:
    // What the guest is told about the controller layout (scePadGetPortMax / scePadGetSlotMax).
    // T-111 changes these two constants if the game needs a multitap; everything else follows.
    static constexpr uint32_t kPorts = 2;
    static constexpr uint32_t kSlotsPerPort = 1;

    // T-306: the default is the two-port layout (2 ports, 1 slot each). With the multitap layout (four players)
    // the description is port_count = 1 and slots_per_port[0] = 4 (input::PadLayout::MultitapPort0); the other
    // port, if any, keeps one slot.
    explicit PadTable(uint32_t ports = kPorts, uint32_t slots_port0 = kSlotsPerPort);

    // Main thread. (port, slot) outside the layout is ignored. The state is "not connected" until the
    // first publish.
    void publish(uint32_t port, uint32_t slot, const Ps2HostPadState &state);

    // Game thread (also usable from tests).
    void read(uint32_t port, uint32_t slot, Ps2HostPadState &out) const;

    // T-308, vibration. Game thread: the runtime's actuator callback (Ps2HostPad::actuator) stores the motors the
    // guest set for (port, slot). Main thread (or any): actuators() reads them. Plain atomics, no lock.
    void set_actuators(uint32_t port, uint32_t slot, uint8_t small_motor, uint8_t large_motor);
    void actuators(uint32_t port, uint32_t slot, uint8_t &small_motor, uint8_t &large_motor) const;

    // The source to hand to PS2Runtime::setHostPad(). The table must outlive the runtime.
    const Ps2HostPad &source() const { return m_source; }

private:
    struct Slot
    {
        std::atomic<uint32_t> seq{0}; // odd while a publish is in progress
        std::atomic<uint64_t> words[3]{};
    };

    Slot m_slots[PS2_HOST_PAD_MAX_PORTS][PS2_HOST_PAD_MAX_SLOTS];
    std::atomic<uint16_t> m_actuators[PS2_HOST_PAD_MAX_PORTS][PS2_HOST_PAD_MAX_SLOTS]{}; // small | large << 8
    Ps2HostPad m_source;
    uint32_t m_ports;
    uint32_t m_slot_count[PS2_HOST_PAD_MAX_PORTS];
};
