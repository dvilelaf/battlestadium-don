// SPDX-License-Identifier: GPL-3.0-or-later
// Input layer (T-111): what a device reports, in plain structures that do not depend on SDL, and the pure
// functions that turn it into the pad state the runtime asks the host for (Ps2HostPadState, see
// runtime/ps2_host_pad.h). Everything here is deterministic and free of operating system calls, so it is
// unit-tested without a gamepad (tests/input/). SDL3 lives only in input_sdl.*; the scripted input
// (input_script.*) builds its states with the same functions.
//
// Players are numbered from 0 in code (player 0 is "P1" in scripts and in the documentation).
//
// ---- The button word ------------------------------------------------------------------------------------
// Ps2HostPadState::buttons is active low (a pressed button clears its bit; 0xFFFF = nothing pressed), with the
// layout of bytes 2 and 3 of the PS2 pad buffer: the bit of each button is the value of PadButton below
// (Select 0, L3 1, R3 2, Start 3, Up 4, Right 5, Down 6, Left 7, L2 8, R2 9, L1 10, R1 11, Triangle 12,
// Circle 13, Cross 14, Square 15).
//
// ---- Pressure --------------------------------------------------------------------------------------------
// The states built here always set has_pressure = 1. The twelve pressure bytes are in the order of the pad
// buffer bytes 8..19, which is the real controller's order: Right, Left, Up, Down, Triangle, Circle, Cross,
// Square, L1, R1, L2, R2 (pressure[i] is buffer byte 8+i, so bytes 16..19 are L1, R1, L2, R2). References:
// PCSX2's DualShock 2 (pcsx2/SIO/Pad/PadDualshock2.cpp: after Square come PAD_L1, PAD_R1, PAD_L2, PAD_R2)
// and the KillzoneRecomp project's patch 0026-pad-pressure-order, which fixed the runtime's L1, L2, R1, R2.
// The runtime patch 0022 does the same here. A pressed digital button has pressure 0xFF and a released one
// 0; L2 and R2 from an analog trigger carry the trigger value. Start, Select, L3 and R3 have no pressure
// byte.
//
// ---- Face buttons ----------------------------------------------------------------------------------------
// A standard (Xbox-style, SDL "standard gamepad") layout is mapped BY POSITION, which is what a player
// expects when the gamepad is in their hands:
//
//     bottom button (SDL south, Xbox A)  -> Cross
//     right button  (SDL east,  Xbox B)  -> Circle
//     left button   (SDL west,  Xbox X)  -> Square
//     top button    (SDL north, Xbox Y)  -> Triangle
//
// This game confirms with Circle and cancels with Cross (measured in the reference emulator, T-017), so on
// a standard gamepad: right button = confirm, bottom button = cancel. Back -> Select, Start -> Start,
// stick clicks -> L3/R3, shoulders -> L1/R1, triggers -> L2/R2, D-pad -> directions.
//
// ---- Keyboard (player 0 only, fixed, positional keys) ----------------------------------------------------
//
//     Arrow keys   D-pad                 I   Triangle        Q   L1        Enter      Start
//     W A S D      left stick (full)     L   Circle (OK)     E   R1        Backspace  Select
//                                        K   Cross (cancel)  1   L2        C          L3
//                                        J   Square          3   R2        V          R3
//
// Two opposite directions pressed together (Left+Right) cancel each other, as the PS2 pad cannot report them.
// There is no right stick on the keyboard. Remapping is a later task (T-113 area); it will replace
// keyboard_to_device() with a table-driven version and keep everything else.
#pragma once

#include "runtime/ps2_host_pad.h"

#include <cstdint>
#include <string_view>

namespace input
{

// The 16 digital buttons. The value of each enumerator is its bit in Ps2HostPadState::buttons.
enum class PadButton : uint8_t
{
    Select = 0,
    L3 = 1,
    R3 = 2,
    Start = 3,
    Up = 4,
    Right = 5,
    Down = 6,
    Left = 7,
    L2 = 8,
    R2 = 9,
    L1 = 10,
    R1 = 11,
    Triangle = 12,
    Circle = 13,
    Cross = 14,
    Square = 15,
};
constexpr int kButtonCount = 16;

// The mask of the button's bit (set bit = the bit that is cleared when pressed).
constexpr uint16_t button_bit(PadButton b) { return static_cast<uint16_t>(1u << static_cast<unsigned>(b)); }

// Index of the button's pressure byte (0..11, the order documented above), -1 for the buttons without one.
int pressure_index(PadButton b);

// "CROSS", "START"... The names scripts use.
const char *button_name(PadButton b);
// Case-insensitive. false if the name is not one of the sixteen.
bool parse_button_name(std::string_view name, PadButton &out);

// ---- Pad state helpers (also used by the script player) --------------------------------------------------

// A connected pad with nothing pressed, sticks at rest, has_pressure = 1 and all pressure bytes 0.
Ps2HostPadState neutral_pad_state(bool connected);
// Presses or releases one button: the bit of the button word and its pressure byte (0xFF / 0).
void set_pad_button(Ps2HostPadState &state, PadButton b, bool pressed);
bool is_pad_button_pressed(const Ps2HostPadState &state, PadButton b);

// A stick axis in [-1, 1] (clamped; positive = right or down, as on the PS2 and in SDL) to the PS2 byte:
// 0 -> 0x80, -1 -> 0x00, +1 -> 0xFF. No dead zone here.
uint8_t stick_axis_to_byte(float v);

// ---- Device state ----------------------------------------------------------------------------------------

// Radius of the dead zone of a stick (fraction of full deflection) and level of an analog trigger at which
// the L2/R2 digital bit is set.
constexpr float kStickDeadZone = 0.25f;
constexpr float kTriggerThreshold = 0.5f;

// What a gamepad (or the keyboard folded into one) reports. Independent of SDL. Face buttons by position.
struct HostDeviceState
{
    bool connected = false;

    bool south = false, east = false, west = false, north = false; // Cross, Circle, Square, Triangle
    bool back = false, start = false;                              // Select, Start
    bool left_stick = false, right_stick = false;                  // L3, R3
    bool left_shoulder = false, right_shoulder = false;            // L1, R1
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;

    // Sticks: -1..1, x positive to the right, y positive DOWN (SDL's convention).
    float left_x = 0.f, left_y = 0.f, right_x = 0.f, right_y = 0.f;
    // Triggers: 0..1.
    float left_trigger = 0.f, right_trigger = 0.f;
};

// The pure translation. A device that is not connected gives the default Ps2HostPadState (not connected,
// nothing pressed). Sticks: circular dead zone kStickDeadZone with the rest of the range rescaled to full,
// then 0x80 centred, 0x00..0xFF at the extremes. Triggers: digital bit at >= kTriggerThreshold, pressure
// byte = round(trigger * 255) in any case.
Ps2HostPadState device_to_pad_state(const HostDeviceState &device);

// ---- Keyboard --------------------------------------------------------------------------------------------

enum class Key : uint8_t
{
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    I, // Triangle
    J, // Square
    K, // Cross
    L, // Circle
    Q, // L1
    E, // R1
    Num1, // L2
    Num3, // R2
    Enter, // Start
    Backspace, // Select
    W,
    A,
    S,
    D, // left stick
    C, // L3
    V, // R3
    Count
};

// A set of pressed keys.
struct KeySet
{
    uint32_t bits = 0;

    void set(Key k, bool down = true)
    {
        const uint32_t m = 1u << static_cast<unsigned>(k);
        bits = down ? (bits | m) : (bits & ~m);
    }
    bool test(Key k) const { return (bits >> static_cast<unsigned>(k)) & 1u; }
    void clear() { bits = 0; }
};

// The keyboard as one connected device (table in the header comment). Diagonals of WASD are normalised.
HostDeviceState keyboard_to_device(const KeySet &keys);
// keyboard_to_device() followed by device_to_pad_state() (the keyboard is always "connected").
Ps2HostPadState keyboard_to_pad_state(const KeySet &keys);

// ---- Merging ---------------------------------------------------------------------------------------------

// Two sources for the same pad (keyboard and gamepad, or a script and a player's devices): pressed on either
// means pressed. connected = either; buttons = AND (active low); pressure = the larger of the two; for each
// stick the one farther from the centre. A source that is not connected contributes nothing; has_pressure is
// set if either has it (a source without pressure counts as 0xFF for its pressed buttons).
Ps2HostPadState merge_pad_states(const Ps2HostPadState &a, const Ps2HostPadState &b);

// ---- Players to PS2 ports -------------------------------------------------------------------------------

enum class PadLayout
{
    TwoPortsNoMultitap, // players 0 and 1 in (port 0, slot 0) and (port 1, slot 0)
    MultitapPort0,      // players 0..3 in port 0, slots 0..3 (--players 4, T-306)
};

struct PortSlot
{
    bool valid = false;
    int port = 0;
    int slot = 0;
};

// The single place that decides where each player is plugged in. Which layout the game wants for four
// players is not known yet (another task). valid = false for a player the layout has no room for.
PortSlot player_to_port_slot(int player, PadLayout layout);

// ---- Vibration (T-308) ----------------------------------------------------------------------------------

// What to hand to SDL_RumbleGamepad (the plain numbers, so that this part is tested without SDL).
struct RumbleCommand
{
    uint16_t low_frequency = 0;  // the large motor, 0..65535
    uint16_t high_frequency = 0; // the small motor
    uint32_t duration_ms = 0;    // 0 with both motors 0: stop
};

// The game's two motors (the small one on/off, the large one 0..255) as SDL's two. The large motor is the
// low-frequency one, scaled 0..255 -> 0..65535; the small motor on is the high-frequency one at full strength.
RumbleCommand rumble_command(uint8_t small_motor, uint8_t large_motor);

// SDL's rumble runs for a duration and then stops by itself, the game's holds until it is changed: the command
// is renewed while the motors hold. update() is called regularly (every loop iteration of the main thread)
// with the motors as they are now and says when a command has to be sent: when the pair changes (a stop is a
// command with both motors 0) and, while it is not 0, every kRenewMs.
class RumbleSchedule
{
public:
    static constexpr uint32_t kDurationMs = 1000;
    static constexpr uint32_t kRenewMs = 400;

    bool update(uint64_t now_ms, uint8_t small_motor, uint8_t large_motor, RumbleCommand &out);
    void reset() { *this = RumbleSchedule{}; }

private:
    uint8_t m_small = 0;
    uint8_t m_large = 0;
    uint64_t m_sent_at = 0;
};

} // namespace input
