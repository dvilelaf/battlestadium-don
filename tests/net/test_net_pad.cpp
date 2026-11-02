// net_pad.h: the 20-byte wire layout and the conversions to and from the runtime's pad state (D14).
#include "check.h"
#include "net_pad.h"

using namespace don_net;

int main()
{
    // Defaults: released, centred, not connected; neutral_pad is the same but connected.
    NetPad d;
    CHECK_EQ(d.buttons, 0xFFFF);
    CHECK_EQ(d.lx, 0x80);
    CHECK_EQ(d.connected, 0);
    CHECK_EQ(neutral_pad().connected, 1);
    CHECK(neutral_pad().buttons == 0xFFFF);

    // Round trip through the wire bytes, every field distinct.
    NetPad p;
    p.buttons = 0x1234;
    p.lx = 1;
    p.ly = 2;
    p.rx = 3;
    p.ry = 4;
    p.connected = 1;
    p.has_pressure = 1;
    for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        p.pressure[i] = static_cast<uint8_t>(100 + i);
    uint8_t wire[kNetPadBytes] = {};
    write_pad(p, wire);
    CHECK_EQ(wire[0], 0x34); // little-endian buttons
    CHECK_EQ(wire[1], 0x12);
    CHECK_EQ(wire[6], 1);
    CHECK_EQ(wire[19], 111);
    CHECK(read_pad(wire) == p);

    // Round trip through Ps2HostPadState.
    const Ps2HostPadState s = from_net(p);
    CHECK_EQ(s.buttons, 0x1234);
    CHECK_EQ(s.ry, 4);
    CHECK_EQ(s.pressure[11], 111);
    CHECK(to_net(s) == p);

    return check_result("test_net_pad");
}
