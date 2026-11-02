// T-111: the translation from device states to the PS2 pad state, with synthetic inputs.
#include "check.h"
#include "input.h"

using namespace input;

namespace
{
// The word the guest reads with exactly these buttons pressed (active low).
uint16_t word_with(std::initializer_list<int> bits)
{
    uint16_t w = 0xFFFF;
    for (int b : bits)
        w = static_cast<uint16_t>(w & ~(1u << b));
    return w;
}

HostDeviceState connected()
{
    HostDeviceState d;
    d.connected = true;
    return d;
}

void test_layout_constants()
{
    CHECK_EQ(static_cast<int>(PadButton::Select), 0);
    CHECK_EQ(static_cast<int>(PadButton::Start), 3);
    CHECK_EQ(static_cast<int>(PadButton::Up), 4);
    CHECK_EQ(static_cast<int>(PadButton::Left), 7);
    CHECK_EQ(static_cast<int>(PadButton::L2), 8);
    CHECK_EQ(static_cast<int>(PadButton::R1), 11);
    CHECK_EQ(static_cast<int>(PadButton::Triangle), 12);
    CHECK_EQ(static_cast<int>(PadButton::Circle), 13);
    CHECK_EQ(static_cast<int>(PadButton::Cross), 14);
    CHECK_EQ(static_cast<int>(PadButton::Square), 15);
    // Pressure byte order: Right, Left, Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2.
    CHECK_EQ(pressure_index(PadButton::Right), 0);
    CHECK_EQ(pressure_index(PadButton::Left), 1);
    CHECK_EQ(pressure_index(PadButton::Up), 2);
    CHECK_EQ(pressure_index(PadButton::Down), 3);
    CHECK_EQ(pressure_index(PadButton::Triangle), 4);
    CHECK_EQ(pressure_index(PadButton::Circle), 5);
    CHECK_EQ(pressure_index(PadButton::Cross), 6);
    CHECK_EQ(pressure_index(PadButton::Square), 7);
    CHECK_EQ(pressure_index(PadButton::L1), 8);
    CHECK_EQ(pressure_index(PadButton::L2), 10);
    CHECK_EQ(pressure_index(PadButton::R1), 9);
    CHECK_EQ(pressure_index(PadButton::R2), 11);
    CHECK_EQ(pressure_index(PadButton::Start), -1);
    CHECK_EQ(pressure_index(PadButton::L3), -1);
}

// The contract with the runtime, stated once: the pressure index of a button is its byte in the pad data
// buffer minus 8, for the buffer of a DualShock 2 (what the runtime writes since patch 0022).
void test_pressure_index_is_buffer_byte_minus_8()
{
    struct Byte
    {
        PadButton button;
        int byte;
    };
    const Byte buffer[] = {{PadButton::Right, 8},   {PadButton::Left, 9},    {PadButton::Up, 10},
                           {PadButton::Down, 11},   {PadButton::Triangle, 12}, {PadButton::Circle, 13},
                           {PadButton::Cross, 14},  {PadButton::Square, 15}, {PadButton::L1, 16},
                           {PadButton::R1, 17},     {PadButton::L2, 18},     {PadButton::R2, 19}};
    for (const Byte &b : buffer)
        CHECK_EQ(pressure_index(b.button), b.byte - 8);

    // And in a state: each button pressed alone puts 0xFF exactly in its byte of the buffer (8 + index).
    for (const Byte &b : buffer)
    {
        Ps2HostPadState s = neutral_pad_state(true);
        set_pad_button(s, b.button, true);
        for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
            CHECK_EQ(s.pressure[i], 8 + i == b.byte ? 0xFF : 0);
    }
}

void test_button_names()
{
    PadButton b;
    CHECK(parse_button_name("CROSS", b) && b == PadButton::Cross);
    CHECK(parse_button_name("circle", b) && b == PadButton::Circle);
    CHECK(parse_button_name("L1", b) && b == PadButton::L1);
    CHECK(parse_button_name("Select", b) && b == PadButton::Select);
    CHECK(!parse_button_name("", b));
    CHECK(!parse_button_name("X", b));
    CHECK(!parse_button_name("CROSSS", b));
    for (int i = 0; i < kButtonCount; ++i)
    {
        PadButton r;
        CHECK(parse_button_name(button_name(static_cast<PadButton>(i)), r) && static_cast<int>(r) == i);
    }
}

void test_disconnected()
{
    HostDeviceState d; // connected = false, even with buttons "pressed"
    d.south = true;
    d.left_x = 1.f;
    const Ps2HostPadState s = device_to_pad_state(d);
    CHECK_EQ(s.connected, 0);
    CHECK_EQ(s.buttons, 0xFFFF);
    CHECK_EQ(s.lx, 0x80);
    CHECK_EQ(s.has_pressure, 0);
}

void test_rest()
{
    const Ps2HostPadState s = device_to_pad_state(connected());
    CHECK_EQ(s.connected, 1);
    CHECK_EQ(s.buttons, 0xFFFF);
    CHECK_EQ(s.lx, 0x80);
    CHECK_EQ(s.ly, 0x80);
    CHECK_EQ(s.rx, 0x80);
    CHECK_EQ(s.ry, 0x80);
    CHECK_EQ(s.has_pressure, 1);
    for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        CHECK_EQ(s.pressure[i], 0);
}

// Each button alone: its bit is cleared, the others are set; its pressure byte (if any) is 0xFF, the others 0.
void test_each_button_alone()
{
    struct Case
    {
        const char *name;
        void (*press)(HostDeviceState &);
        PadButton button;
    };
    const Case cases[] = {
        {"south", [](HostDeviceState &d) { d.south = true; }, PadButton::Cross},
        {"east", [](HostDeviceState &d) { d.east = true; }, PadButton::Circle},
        {"west", [](HostDeviceState &d) { d.west = true; }, PadButton::Square},
        {"north", [](HostDeviceState &d) { d.north = true; }, PadButton::Triangle},
        {"back", [](HostDeviceState &d) { d.back = true; }, PadButton::Select},
        {"start", [](HostDeviceState &d) { d.start = true; }, PadButton::Start},
        {"left_stick", [](HostDeviceState &d) { d.left_stick = true; }, PadButton::L3},
        {"right_stick", [](HostDeviceState &d) { d.right_stick = true; }, PadButton::R3},
        {"left_shoulder", [](HostDeviceState &d) { d.left_shoulder = true; }, PadButton::L1},
        {"right_shoulder", [](HostDeviceState &d) { d.right_shoulder = true; }, PadButton::R1},
        {"dpad_up", [](HostDeviceState &d) { d.dpad_up = true; }, PadButton::Up},
        {"dpad_down", [](HostDeviceState &d) { d.dpad_down = true; }, PadButton::Down},
        {"dpad_left", [](HostDeviceState &d) { d.dpad_left = true; }, PadButton::Left},
        {"dpad_right", [](HostDeviceState &d) { d.dpad_right = true; }, PadButton::Right},
        {"left_trigger", [](HostDeviceState &d) { d.left_trigger = 1.f; }, PadButton::L2},
        {"right_trigger", [](HostDeviceState &d) { d.right_trigger = 1.f; }, PadButton::R2},
    };
    for (const Case &c : cases)
    {
        HostDeviceState d = connected();
        c.press(d);
        const Ps2HostPadState s = device_to_pad_state(d);
        const uint16_t expected = static_cast<uint16_t>(~(1u << static_cast<unsigned>(c.button)));
        if (s.buttons != expected)
            std::fprintf(stderr, "case %s: buttons %04x, expected %04x\n", c.name, s.buttons, expected);
        CHECK_EQ(s.buttons, expected);
        for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        {
            const bool mine = i == pressure_index(c.button);
            if (s.pressure[i] != (mine ? 0xFF : 0))
                std::fprintf(stderr, "case %s: pressure[%d] = %d\n", c.name, i, s.pressure[i]);
            CHECK_EQ(s.pressure[i], mine ? 0xFF : 0);
        }
    }
}

void test_face_buttons_by_position()
{
    HostDeviceState d = connected();
    d.south = true; // bottom button
    CHECK_EQ(device_to_pad_state(d).buttons, word_with({14})); // Cross
    d = connected();
    d.east = true; // right button
    CHECK_EQ(device_to_pad_state(d).buttons, word_with({13})); // Circle
}

void test_combinations()
{
    HostDeviceState d = connected();
    d.dpad_down = true;
    d.south = true;
    d.start = true;
    d.left_shoulder = true;
    Ps2HostPadState s = device_to_pad_state(d);
    CHECK_EQ(s.buttons, word_with({6, 14, 3, 10}));
    CHECK_EQ(s.pressure[3], 0xFF);  // Down
    CHECK_EQ(s.pressure[6], 0xFF);  // Cross
    CHECK_EQ(s.pressure[8], 0xFF);  // L1
    CHECK_EQ(s.pressure[0], 0);     // Right

    d = connected();
    d.north = d.east = d.west = d.south = true;
    CHECK_EQ(device_to_pad_state(d).buttons, word_with({12, 13, 14, 15}));

    d = connected();
    d.dpad_up = d.dpad_right = true; // a diagonal
    s = device_to_pad_state(d);
    CHECK_EQ(s.buttons, word_with({4, 5}));
    CHECK_EQ(s.pressure[2], 0xFF);
    CHECK_EQ(s.pressure[0], 0xFF);
}

void test_sticks()
{
    HostDeviceState d = connected();
    Ps2HostPadState s;

    // Extremes: x positive = right, y positive = down.
    d.left_x = 1.f;
    d.left_y = 0.f;
    s = device_to_pad_state(d);
    CHECK_EQ(s.lx, 0xFF);
    CHECK_EQ(s.ly, 0x80);
    d.left_x = -1.f;
    s = device_to_pad_state(d);
    CHECK_EQ(s.lx, 0x00);
    d = connected();
    d.left_y = 1.f;
    CHECK_EQ(device_to_pad_state(d).ly, 0xFF);
    d.left_y = -1.f;
    CHECK_EQ(device_to_pad_state(d).ly, 0x00);

    d = connected();
    d.right_x = 1.f;
    d.right_y = -1.f;
    s = device_to_pad_state(d);
    CHECK(s.rx > 0xE0);
    CHECK(s.ry < 0x20);
    CHECK_EQ(s.lx, 0x80); // the left stick is independent

    // Corner of the square range reaches the full range as well.
    d = connected();
    d.left_x = 1.f;
    d.left_y = 1.f;
    s = device_to_pad_state(d);
    CHECK_EQ(s.lx, 0xFF);
    CHECK_EQ(s.ly, 0xFF);

    // Inside the dead zone: rest. Just outside: moves, monotonically.
    d = connected();
    d.left_x = 0.2f;
    d.left_y = -0.2f; // magnitude 0.283 > 0.25: out
    s = device_to_pad_state(d);
    CHECK(s.lx > 0x80);
    CHECK(s.ly < 0x80);
    d.left_x = 0.15f;
    d.left_y = 0.15f; // magnitude 0.212 < 0.25: in
    s = device_to_pad_state(d);
    CHECK_EQ(s.lx, 0x80);
    CHECK_EQ(s.ly, 0x80);
    d.left_x = 0.24f;
    d.left_y = 0.f;
    CHECK_EQ(device_to_pad_state(d).lx, 0x80);
    d.right_x = -0.1f;
    d.right_y = 0.1f;
    s = device_to_pad_state(d);
    CHECK_EQ(s.rx, 0x80);
    CHECK_EQ(s.ry, 0x80);

    // Half deflection: dead zone rescaled, (0.5 - 0.25) / 0.75 = 1/3 -> 0x80 + 42.3.
    d = connected();
    d.left_x = 0.5f;
    CHECK_EQ(device_to_pad_state(d).lx, 170);
    d.left_x = -0.5f;
    CHECK_EQ(device_to_pad_state(d).lx, 85);

    // Out-of-range input is clamped.
    CHECK_EQ(stick_axis_to_byte(5.f), 0xFF);
    CHECK_EQ(stick_axis_to_byte(-5.f), 0x00);
    CHECK_EQ(stick_axis_to_byte(0.f), 0x80);
}

void test_triggers()
{
    HostDeviceState d = connected();
    d.left_trigger = 0.2f; // below the threshold: no digital bit, the analog value as pressure
    Ps2HostPadState s = device_to_pad_state(d);
    CHECK_EQ(s.buttons, 0xFFFF);
    CHECK_EQ(s.pressure[pressure_index(PadButton::L2)], 51);
    CHECK_EQ(s.pressure[pressure_index(PadButton::R2)], 0);

    d.left_trigger = 0.49f;
    CHECK_EQ(device_to_pad_state(d).buttons, 0xFFFF);
    d.left_trigger = kTriggerThreshold; // at the threshold: pressed
    CHECK_EQ(device_to_pad_state(d).buttons, word_with({8}));
    d.left_trigger = 0.8f;
    d.right_trigger = 1.f;
    s = device_to_pad_state(d);
    CHECK_EQ(s.buttons, word_with({8, 9}));
    CHECK_EQ(s.pressure[10], 204);
    CHECK_EQ(s.pressure[11], 255);

    d.left_trigger = 1.5f; // clamped
    CHECK_EQ(device_to_pad_state(d).pressure[10], 255);
}

void test_keyboard()
{
    KeySet k;
    CHECK_EQ(keyboard_to_pad_state(k).connected, 1);
    CHECK_EQ(keyboard_to_pad_state(k).buttons, 0xFFFF);

    struct Case
    {
        Key key;
        PadButton button;
    };
    const Case cases[] = {
        {Key::ArrowUp, PadButton::Up},     {Key::ArrowDown, PadButton::Down},
        {Key::ArrowLeft, PadButton::Left}, {Key::ArrowRight, PadButton::Right},
        {Key::I, PadButton::Triangle},     {Key::J, PadButton::Square},
        {Key::K, PadButton::Cross},        {Key::L, PadButton::Circle},
        {Key::Q, PadButton::L1},           {Key::E, PadButton::R1},
        {Key::Num1, PadButton::L2},        {Key::Num3, PadButton::R2},
        {Key::Enter, PadButton::Start},    {Key::Backspace, PadButton::Select},
        {Key::C, PadButton::L3},           {Key::V, PadButton::R3},
    };
    for (const Case &c : cases)
    {
        KeySet one;
        one.set(c.key);
        const Ps2HostPadState s = keyboard_to_pad_state(one);
        CHECK_EQ(s.buttons, static_cast<uint16_t>(~(1u << static_cast<unsigned>(c.button))));
        CHECK_EQ(s.lx, 0x80);
        one.set(c.key, false);
        CHECK_EQ(one.bits, 0);
    }

    // Left stick on WASD.
    KeySet w;
    w.set(Key::D);
    Ps2HostPadState s = keyboard_to_pad_state(w);
    CHECK_EQ(s.lx, 0xFF);
    CHECK_EQ(s.ly, 0x80);
    CHECK_EQ(s.buttons, 0xFFFF);
    w = KeySet{};
    w.set(Key::W);
    CHECK_EQ(keyboard_to_pad_state(w).ly, 0x00);
    w = KeySet{};
    w.set(Key::W);
    w.set(Key::A);
    s = keyboard_to_pad_state(w);
    CHECK(s.lx < 0x80 && s.lx > 0x10); // diagonal normalised: not the full corner
    CHECK(s.ly < 0x80 && s.ly > 0x10);
    w.set(Key::S); // W + S cancel, A remains
    s = keyboard_to_pad_state(w);
    CHECK_EQ(s.ly, 0x80);
    CHECK_EQ(s.lx, 0x00);

    // Opposite directions cancel.
    KeySet lr;
    lr.set(Key::ArrowLeft);
    lr.set(Key::ArrowRight);
    CHECK_EQ(keyboard_to_pad_state(lr).buttons, 0xFFFF);

    // Combination: Right + Circle + Start.
    KeySet combo;
    combo.set(Key::ArrowRight);
    combo.set(Key::L);
    combo.set(Key::Enter);
    CHECK_EQ(keyboard_to_pad_state(combo).buttons, word_with({5, 13, 3}));
}

void test_merge()
{
    HostDeviceState pad = connected();
    pad.south = true;
    pad.left_x = 1.f;
    const Ps2HostPadState gp = device_to_pad_state(pad);
    KeySet keys;
    keys.set(Key::L);
    keys.set(Key::ArrowUp);
    const Ps2HostPadState kb = keyboard_to_pad_state(keys);

    Ps2HostPadState m = merge_pad_states(kb, gp);
    CHECK_EQ(m.connected, 1);
    CHECK_EQ(m.buttons, word_with({14, 13, 4})); // pressed on either
    CHECK_EQ(m.lx, 0xFF);                        // the stick that is deflected wins
    CHECK_EQ(m.pressure[pressure_index(PadButton::Cross)], 0xFF);
    CHECK_EQ(m.pressure[pressure_index(PadButton::Circle)], 0xFF);
    CHECK_EQ(m.pressure[pressure_index(PadButton::Up)], 0xFF);
    CHECK_EQ(m.pressure[pressure_index(PadButton::Square)], 0);

    // Symmetric.
    const Ps2HostPadState m2 = merge_pad_states(gp, kb);
    CHECK_EQ(m2.buttons, m.buttons);
    CHECK_EQ(m2.lx, m.lx);

    // Nothing pressed on either: nothing pressed.
    m = merge_pad_states(keyboard_to_pad_state(KeySet{}), device_to_pad_state(connected()));
    CHECK_EQ(m.buttons, 0xFFFF);
    CHECK_EQ(m.lx, 0x80);

    // The larger trigger value wins.
    HostDeviceState t1 = connected(), t2 = connected();
    t1.left_trigger = 0.3f;
    t2.left_trigger = 0.8f;
    m = merge_pad_states(device_to_pad_state(t1), device_to_pad_state(t2));
    CHECK_EQ(m.pressure[10], 204);
    CHECK_EQ(m.buttons, word_with({8}));

    // A disconnected source contributes nothing and the result is the other one.
    const Ps2HostPadState gone = device_to_pad_state(HostDeviceState{});
    m = merge_pad_states(gone, kb);
    CHECK_EQ(m.connected, 1);
    CHECK_EQ(m.buttons, kb.buttons);
    m = merge_pad_states(gp, gone);
    CHECK_EQ(m.buttons, gp.buttons);
    m = merge_pad_states(gone, gone);
    CHECK_EQ(m.connected, 0);
    CHECK_EQ(m.buttons, 0xFFFF);

    // A source without pressure counts as 0xFF on its pressed buttons.
    Ps2HostPadState legacy; // connected = 0 by default
    legacy.connected = 1;
    legacy.buttons = static_cast<uint16_t>(~button_bit(PadButton::Square));
    m = merge_pad_states(legacy, gp);
    CHECK_EQ(m.has_pressure, 1);
    CHECK_EQ(m.pressure[pressure_index(PadButton::Square)], 0xFF);
    CHECK_EQ(m.pressure[pressure_index(PadButton::Cross)], 0xFF);
}

void test_pad_state_helpers()
{
    Ps2HostPadState s = neutral_pad_state(true);
    set_pad_button(s, PadButton::Start, true);
    CHECK(is_pad_button_pressed(s, PadButton::Start));
    CHECK_EQ(s.buttons, word_with({3}));
    set_pad_button(s, PadButton::Start, false);
    CHECK_EQ(s.buttons, 0xFFFF);
    CHECK_EQ(neutral_pad_state(false).connected, 0);
}

void test_player_ports()
{
    PortSlot p = player_to_port_slot(0, PadLayout::TwoPortsNoMultitap);
    CHECK(p.valid && p.port == 0 && p.slot == 0);
    p = player_to_port_slot(1, PadLayout::TwoPortsNoMultitap);
    CHECK(p.valid && p.port == 1 && p.slot == 0);
    CHECK(!player_to_port_slot(2, PadLayout::TwoPortsNoMultitap).valid);
    CHECK(!player_to_port_slot(-1, PadLayout::TwoPortsNoMultitap).valid);
    for (int i = 0; i < 4; ++i)
    {
        p = player_to_port_slot(i, PadLayout::MultitapPort0);
        CHECK(p.valid && p.port == 0 && p.slot == i);
    }
    CHECK(!player_to_port_slot(4, PadLayout::MultitapPort0).valid);
    // T-306: every player has its own (port, slot) in each layout; none is shared.
    for (const PadLayout layout : {PadLayout::TwoPortsNoMultitap, PadLayout::MultitapPort0})
        for (int a = 0; a < 4; ++a)
            for (int b = a + 1; b < 4; ++b)
            {
                const PortSlot pa = player_to_port_slot(a, layout), pb = player_to_port_slot(b, layout);
                CHECK(!(pa.valid && pb.valid && pa.port == pb.port && pa.slot == pb.slot));
            }
}
} // namespace

int main()
{
    test_layout_constants();
    test_pressure_index_is_buffer_byte_minus_8();
    test_button_names();
    test_disconnected();
    test_rest();
    test_each_button_alone();
    test_face_buttons_by_position();
    test_combinations();
    test_sticks();
    test_triggers();
    test_keyboard();
    test_merge();
    test_pad_state_helpers();
    test_player_ports();
    return check_result("test_input_mapping");
}
