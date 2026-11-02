// SPDX-License-Identifier: GPL-3.0-or-later
#include "input.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace input
{
namespace
{
constexpr const char *kNames[kButtonCount] = {"SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                              "L2",     "R2", "L1", "R1",    "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};

// The button of each pressure byte, in the order of the pad buffer bytes 8..19 (the controller's: L1, R1, L2, R2).
constexpr PadButton kPressureButtons[PS2_HOST_PAD_PRESSURE_COUNT] = {
    PadButton::Right, PadButton::Left, PadButton::Up, PadButton::Down, PadButton::Triangle, PadButton::Circle,
    PadButton::Cross, PadButton::Square, PadButton::L1, PadButton::R1, PadButton::L2, PadButton::R2};

uint8_t effective_pressure(const Ps2HostPadState &s, int index)
{
    if (s.has_pressure)
        return s.pressure[index];
    return is_pad_button_pressed(s, kPressureButtons[index]) ? 0xFFu : 0u;
}

int stick_distance2(uint8_t x, uint8_t y)
{
    const int dx = static_cast<int>(x) - 0x80;
    const int dy = static_cast<int>(y) - 0x80;
    return dx * dx + dy * dy;
}

void dead_zone(float &x, float &y)
{
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= kStickDeadZone)
    {
        x = y = 0.f;
        return;
    }
    const float scale = (mag - kStickDeadZone) / (1.f - kStickDeadZone) / mag;
    x = std::clamp(x * scale, -1.f, 1.f);
    y = std::clamp(y * scale, -1.f, 1.f);
}
} // namespace

int pressure_index(PadButton b)
{
    for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        if (kPressureButtons[i] == b)
            return i;
    return -1;
}

const char *button_name(PadButton b) { return kNames[static_cast<unsigned>(b)]; }

bool parse_button_name(std::string_view name, PadButton &out)
{
    for (int i = 0; i < kButtonCount; ++i)
    {
        const std::string_view n = kNames[i];
        if (n.size() != name.size())
            continue;
        bool same = true;
        for (size_t c = 0; c < n.size(); ++c)
            if (n[c] != std::toupper(static_cast<unsigned char>(name[c])))
                same = false;
        if (same)
        {
            out = static_cast<PadButton>(i);
            return true;
        }
    }
    return false;
}

Ps2HostPadState neutral_pad_state(bool connected)
{
    Ps2HostPadState s;
    s.connected = connected ? 1 : 0;
    s.has_pressure = 1;
    return s;
}

void set_pad_button(Ps2HostPadState &state, PadButton b, bool pressed)
{
    const uint16_t bit = button_bit(b);
    state.buttons = pressed ? static_cast<uint16_t>(state.buttons & ~bit) : static_cast<uint16_t>(state.buttons | bit);
    const int index = pressure_index(b);
    if (index >= 0)
        state.pressure[index] = pressed ? 0xFFu : 0u;
}

bool is_pad_button_pressed(const Ps2HostPadState &state, PadButton b) { return (state.buttons & button_bit(b)) == 0; }

uint8_t stick_axis_to_byte(float v)
{
    v = std::clamp(v, -1.f, 1.f);
    const float f = v < 0.f ? 128.f + v * 128.f : 128.f + v * 127.f;
    return static_cast<uint8_t>(std::floor(f + 0.5f));
}

Ps2HostPadState device_to_pad_state(const HostDeviceState &d)
{
    Ps2HostPadState s;
    if (!d.connected)
        return s;
    s = neutral_pad_state(true);

    set_pad_button(s, PadButton::Cross, d.south);
    set_pad_button(s, PadButton::Circle, d.east);
    set_pad_button(s, PadButton::Square, d.west);
    set_pad_button(s, PadButton::Triangle, d.north);
    set_pad_button(s, PadButton::Select, d.back);
    set_pad_button(s, PadButton::Start, d.start);
    set_pad_button(s, PadButton::L3, d.left_stick);
    set_pad_button(s, PadButton::R3, d.right_stick);
    set_pad_button(s, PadButton::L1, d.left_shoulder);
    set_pad_button(s, PadButton::R1, d.right_shoulder);
    set_pad_button(s, PadButton::Up, d.dpad_up);
    set_pad_button(s, PadButton::Down, d.dpad_down);
    set_pad_button(s, PadButton::Left, d.dpad_left);
    set_pad_button(s, PadButton::Right, d.dpad_right);

    float lx = d.left_x, ly = d.left_y, rx = d.right_x, ry = d.right_y;
    dead_zone(lx, ly);
    dead_zone(rx, ry);
    s.lx = stick_axis_to_byte(lx);
    s.ly = stick_axis_to_byte(ly);
    s.rx = stick_axis_to_byte(rx);
    s.ry = stick_axis_to_byte(ry);

    const float lt = std::clamp(d.left_trigger, 0.f, 1.f);
    const float rt = std::clamp(d.right_trigger, 0.f, 1.f);
    set_pad_button(s, PadButton::L2, lt >= kTriggerThreshold);
    set_pad_button(s, PadButton::R2, rt >= kTriggerThreshold);
    s.pressure[pressure_index(PadButton::L2)] = static_cast<uint8_t>(std::floor(lt * 255.f + 0.5f));
    s.pressure[pressure_index(PadButton::R2)] = static_cast<uint8_t>(std::floor(rt * 255.f + 0.5f));
    return s;
}

HostDeviceState keyboard_to_device(const KeySet &k)
{
    HostDeviceState d;
    d.connected = true;
    d.north = k.test(Key::I);
    d.west = k.test(Key::J);
    d.south = k.test(Key::K);
    d.east = k.test(Key::L);
    d.left_shoulder = k.test(Key::Q);
    d.right_shoulder = k.test(Key::E);
    d.start = k.test(Key::Enter);
    d.back = k.test(Key::Backspace);
    d.left_stick = k.test(Key::C);
    d.right_stick = k.test(Key::V);
    d.left_trigger = k.test(Key::Num1) ? 1.f : 0.f;
    d.right_trigger = k.test(Key::Num3) ? 1.f : 0.f;

    const bool up = k.test(Key::ArrowUp), down = k.test(Key::ArrowDown);
    const bool left = k.test(Key::ArrowLeft), right = k.test(Key::ArrowRight);
    d.dpad_up = up && !down;
    d.dpad_down = down && !up;
    d.dpad_left = left && !right;
    d.dpad_right = right && !left;

    float x = (k.test(Key::D) ? 1.f : 0.f) - (k.test(Key::A) ? 1.f : 0.f);
    float y = (k.test(Key::S) ? 1.f : 0.f) - (k.test(Key::W) ? 1.f : 0.f);
    if (x != 0.f && y != 0.f)
    {
        x *= 0.70710678f;
        y *= 0.70710678f;
    }
    d.left_x = x;
    d.left_y = y;
    return d;
}

Ps2HostPadState keyboard_to_pad_state(const KeySet &keys) { return device_to_pad_state(keyboard_to_device(keys)); }

Ps2HostPadState merge_pad_states(const Ps2HostPadState &a, const Ps2HostPadState &b)
{
    if (!a.connected && !b.connected)
        return Ps2HostPadState{};
    if (!a.connected)
        return b;
    if (!b.connected)
        return a;

    Ps2HostPadState m;
    m.connected = 1;
    m.buttons = static_cast<uint16_t>(a.buttons & b.buttons);
    m.has_pressure = (a.has_pressure || b.has_pressure) ? 1 : 0;
    for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        m.pressure[i] = std::max(effective_pressure(a, i), effective_pressure(b, i));

    const bool left_a = stick_distance2(a.lx, a.ly) >= stick_distance2(b.lx, b.ly);
    m.lx = left_a ? a.lx : b.lx;
    m.ly = left_a ? a.ly : b.ly;
    const bool right_a = stick_distance2(a.rx, a.ry) >= stick_distance2(b.rx, b.ry);
    m.rx = right_a ? a.rx : b.rx;
    m.ry = right_a ? a.ry : b.ry;
    return m;
}

PortSlot player_to_port_slot(int player, PadLayout layout)
{
    PortSlot r;
    if (player < 0)
        return r;
    switch (layout)
    {
    case PadLayout::TwoPortsNoMultitap:
        if (player < 2)
            r = {true, player, 0};
        break;
    case PadLayout::MultitapPort0:
        if (player < 4)
            r = {true, 0, player};
        break;
    }
    return r;
}

RumbleCommand rumble_command(uint8_t small_motor, uint8_t large_motor)
{
    RumbleCommand c;
    c.low_frequency = static_cast<uint16_t>(large_motor * 257u);
    c.high_frequency = small_motor ? 0xFFFFu : 0u;
    c.duration_ms = (c.low_frequency || c.high_frequency) ? RumbleSchedule::kDurationMs : 0u;
    return c;
}

bool RumbleSchedule::update(uint64_t now_ms, uint8_t small_motor, uint8_t large_motor, RumbleCommand &out)
{
    const uint8_t small_on = small_motor ? 1u : 0u;
    const bool changed = small_on != m_small || large_motor != m_large;
    const bool running = small_on != 0 || large_motor != 0;
    if (!changed && !(running && now_ms - m_sent_at >= kRenewMs))
        return false;
    m_small = small_on;
    m_large = large_motor;
    m_sent_at = now_ms;
    out = rumble_command(small_on, large_motor);
    return true;
}

} // namespace input
