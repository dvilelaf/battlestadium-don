// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 side of the input layer (T-111): opens gamepads as they are connected and removed, follows the
// keyboard, and reads both into the plain structures of input.h. The only file pair that touches SDL.
// No game logic and no runtime calls here. Main thread only (it is fed from the SDL event loop).
//
// Players: player 0 = the first gamepad plus the keyboard (merged: pressed on either); players 1..3 = the
// following gamepads, in the order they were connected. When a gamepad is removed the later ones move up.
// A gamepad connected after the program started is picked up by its SDL_EVENT_GAMEPAD_ADDED event.
//
// Rumble (T-308): set_rumble() drives the motors of the gamepad of a player with SDL_RumbleGamepad.
// Remapping will be added here: a table that replaces the fixed one in keyboard_to_device() and a binding
// step in the translation of the gamepad.
#pragma once

#include "input.h"

#include <memory>

union SDL_Event;

namespace input
{

class SdlInput
{
public:
    static constexpr int kMaxPlayers = 4;

    SdlInput();
    ~SdlInput();
    SdlInput(const SdlInput &) = delete;
    SdlInput &operator=(const SdlInput &) = delete;

    // Starts SDL's gamepad subsystem (SDL_InitSubSystem, so it is fine if the host already started SDL) and
    // opens the gamepads that are already connected. false if SDL fails (SDL_GetError() has the reason).
    bool init();
    void shutdown();

    // Call for every event the host's event loop gets. Gamepad added/removed, key down/up, focus lost
    // (releases the keys); everything else is ignored.
    void handle_event(const SDL_Event &event);

    // The current state of a player (0..3), merged keyboard + gamepad for player 0. A player without a
    // gamepad (and not player 0) gives the default state: not connected. Player 0 is always connected: the
    // keyboard counts.
    Ps2HostPadState pad_state(int player) const;

    // T-308, vibration. The motors the game has set for a player (0..3): the small one on/off, the large one
    // 0..255. Call it regularly (every iteration of the main loop) with the current values: it sends a command
    // to the player's gamepad when the pair changes and renews it while it holds (SDL's rumble times out by
    // itself). Nothing happens for a player without a gamepad (the keyboard has no motors) and a gamepad
    // that cannot rumble is skipped without a message. Main thread only.
    void set_rumble(int player, uint8_t small_motor, uint8_t large_motor);

    int gamepad_count() const;
    KeySet keys() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace input
