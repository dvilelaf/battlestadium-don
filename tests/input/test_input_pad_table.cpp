// T-306: the pad table's description in the two layouts and the (port, slot) each player publishes to.
#include "check.h"
#include "input.h"
#include "pad_table.h"

using namespace input;

namespace
{
Ps2HostPadState with_button(PadButton b)
{
    Ps2HostPadState s = neutral_pad_state(true);
    set_pad_button(s, b, true);
    return s;
}

void test_two_port_table()
{
    PadTable t;
    CHECK_EQ(t.source().port_count, 2);
    CHECK_EQ(t.source().slots_per_port[0], 1);
    CHECK_EQ(t.source().slots_per_port[1], 1);
    // A slot outside the description is ignored: nothing is stored for slot 1 of port 0.
    t.publish(0, 1, neutral_pad_state(true));
    Ps2HostPadState s;
    t.read(0, 1, s);
    CHECK_EQ(s.connected, 0);
}

void test_multitap_table()
{
    PadTable t(2, 4);
    CHECK_EQ(t.source().port_count, 2);
    CHECK_EQ(t.source().slots_per_port[0], 4);
    CHECK_EQ(t.source().slots_per_port[1], 1);
    const PadButton buttons[4] = {PadButton::Cross, PadButton::Circle, PadButton::Square, PadButton::Triangle};
    for (int player = 0; player < 4; ++player)
    {
        const PortSlot where = player_to_port_slot(player, PadLayout::MultitapPort0);
        CHECK(where.valid);
        t.publish(where.port, where.slot, with_button(buttons[player]));
    }
    for (int slot = 0; slot < 4; ++slot)
    {
        Ps2HostPadState s;
        t.source().read(t.source().user, 0, slot, &s);
        CHECK_EQ(s.connected, 1);
        for (int b = 0; b < 4; ++b)
            CHECK_EQ(is_pad_button_pressed(s, buttons[b]), b == slot);
    }
    Ps2HostPadState s;
    t.read(1, 0, s); // port 1 is described but nobody is plugged in
    CHECK_EQ(s.connected, 0);
}
} // namespace

int main()
{
    test_two_port_table();
    test_multitap_table();
    return check_result("test_input_pad_table");
}
