// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_sdl.h"

#include "steam_detect.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace input
{
namespace
{
bool key_for_scancode(SDL_Scancode sc, Key &out)
{
    switch (sc)
    {
    case SDL_SCANCODE_UP: out = Key::ArrowUp; return true;
    case SDL_SCANCODE_DOWN: out = Key::ArrowDown; return true;
    case SDL_SCANCODE_LEFT: out = Key::ArrowLeft; return true;
    case SDL_SCANCODE_RIGHT: out = Key::ArrowRight; return true;
    case SDL_SCANCODE_I: out = Key::I; return true;
    case SDL_SCANCODE_J: out = Key::J; return true;
    case SDL_SCANCODE_K: out = Key::K; return true;
    case SDL_SCANCODE_L: out = Key::L; return true;
    case SDL_SCANCODE_Q: out = Key::Q; return true;
    case SDL_SCANCODE_E: out = Key::E; return true;
    case SDL_SCANCODE_1: out = Key::Num1; return true;
    case SDL_SCANCODE_3: out = Key::Num3; return true;
    case SDL_SCANCODE_RETURN: out = Key::Enter; return true;
    case SDL_SCANCODE_BACKSPACE: out = Key::Backspace; return true;
    case SDL_SCANCODE_W: out = Key::W; return true;
    case SDL_SCANCODE_A: out = Key::A; return true;
    case SDL_SCANCODE_S: out = Key::S; return true;
    case SDL_SCANCODE_D: out = Key::D; return true;
    case SDL_SCANCODE_C: out = Key::C; return true;
    case SDL_SCANCODE_V: out = Key::V; return true;
    default: return false;
    }
}

float axis(SDL_Gamepad *pad, SDL_GamepadAxis a)
{
    return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, a)) / 32767.f, -1.f, 1.f);
}

HostDeviceState read_gamepad(SDL_Gamepad *pad)
{
    HostDeviceState d;
    d.connected = true;
    auto b = [&](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
    d.south = b(SDL_GAMEPAD_BUTTON_SOUTH);
    d.east = b(SDL_GAMEPAD_BUTTON_EAST);
    d.west = b(SDL_GAMEPAD_BUTTON_WEST);
    d.north = b(SDL_GAMEPAD_BUTTON_NORTH);
    d.back = b(SDL_GAMEPAD_BUTTON_BACK);
    d.start = b(SDL_GAMEPAD_BUTTON_START);
    d.left_stick = b(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    d.right_stick = b(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    d.left_shoulder = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    d.right_shoulder = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    d.dpad_up = b(SDL_GAMEPAD_BUTTON_DPAD_UP);
    d.dpad_down = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    d.dpad_left = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    d.dpad_right = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    d.left_x = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    d.left_y = axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    d.right_x = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    d.right_y = axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    d.left_trigger = std::clamp(axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0.f, 1.f);
    d.right_trigger = std::clamp(axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0.f, 1.f);
    return d;
}

const char *joystick_type_name(SDL_JoystickType type)
{
    switch (type)
    {
    case SDL_JOYSTICK_TYPE_GAMEPAD: return "gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL: return "wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade_stick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight_stick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD: return "dance_pad";
    case SDL_JOYSTICK_TYPE_GUITAR: return "guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT: return "drum_kit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: return "arcade_pad";
    case SDL_JOYSTICK_TYPE_THROTTLE: return "throttle";
    default: return "unknown";
    }
}

const char *gamepad_type_name(SDL_GamepadType type)
{
    switch (type)
    {
    case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
    case SDL_GAMEPAD_TYPE_XBOX360: return "xbox360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "xboxone";
    case SDL_GAMEPAD_TYPE_PS3: return "ps3";
    case SDL_GAMEPAD_TYPE_PS4: return "ps4";
    case SDL_GAMEPAD_TYPE_PS5: return "ps5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "switch_pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "switch_joycon_left";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "switch_joycon_right";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "switch_joycon_pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE: return "gamecube";
    default: return "unknown";
    }
}
} // namespace

struct SdlInput::Impl
{
    bool started = false;
    Uint64 start_ticks = 0; // SDL_GetTicks() when init() succeeded, for the hotplug "t=..." lines
    bool steam_logged = false;
    KeySet keys;
    std::vector<SDL_Gamepad *> pads; // index = player
    RumbleSchedule rumble[kMaxPlayers];
    // Last known name per joystick id (filled by log_devices), so a remove line can name a device that is
    // already gone, and the ids whose GAMEPAD_REMOVED was seen but whose JOYSTICK_REMOVED has not arrived
    // yet (SDL sends GAMEPAD_REMOVED first; the JOYSTICK_REMOVED that follows must not log twice).
    std::unordered_map<SDL_JoystickID, std::string> names;
    std::unordered_set<SDL_JoystickID> removed_gamepads;

    SDL_Gamepad *find(SDL_JoystickID id) const
    {
        for (SDL_Gamepad *p : pads)
            if (SDL_GetGamepadID(p) == id)
                return p;
        return nullptr;
    }

    void open(SDL_JoystickID id)
    {
        if (find(id) || static_cast<int>(pads.size()) >= kMaxPlayers)
            return;
        if (SDL_Gamepad *p = SDL_OpenGamepad(id))
            pads.push_back(p);
    }

    void close(SDL_JoystickID id)
    {
        auto it = std::find_if(pads.begin(), pads.end(), [&](SDL_Gamepad *p) { return SDL_GetGamepadID(p) == id; });
        if (it == pads.end())
            return;
        SDL_CloseGamepad(*it);
        pads.erase(it);
        // The later gamepads move up a player: their schedules start again (the next call sends the pair).
        for (RumbleSchedule &r : rumble)
            r.reset();
    }

    int pad_index_for(SDL_JoystickID id) const
    {
        for (int i = 0; i < static_cast<int>(pads.size()); ++i)
            if (SDL_GetGamepadID(pads[i]) == id)
                return i;
        return -1;
    }

    double elapsed_s() const { return static_cast<double>(SDL_GetTicks() - start_ticks) / 1000.0; }

    void log_steam_once()
    {
        if (steam_logged)
            return;
        steam_logged = true;
        std::fprintf(stderr, "[don] input: Steam client running: %s\n", steam_client_running() ? "yes" : "no");
    }

    // One [don] line per joystick SDL reports, with everything a controller report needs: name, VID:PID,
    // GUID, the joystick type, whether SDL has a gamepad mapping (and which), the OS path, and the pad slot
    // our input gave it ("none" when it is not used).
    void log_one_device(SDL_JoystickID id)
    {
        const char *name = SDL_GetJoystickNameForID(id);
        const SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
        Uint16 vendor = 0, product = 0;
        SDL_GetJoystickGUIDInfo(guid, &vendor, &product, nullptr, nullptr);
        char guid_str[33];
        SDL_GUIDToString(guid, guid_str, sizeof(guid_str));
        const bool is_gamepad = SDL_IsGamepad(id);
        const char *path = SDL_GetJoystickPathForID(id);
        const int pad = is_gamepad ? pad_index_for(id) : -1;

        std::fprintf(stderr, "[don] input: \"%s\" vid:pid=%04x:%04x guid=%s type=%s gamepad=%s", name ? name : "?",
                     static_cast<unsigned>(vendor), static_cast<unsigned>(product), guid_str,
                     joystick_type_name(SDL_GetJoystickTypeForID(id)), is_gamepad ? "yes" : "no");
        if (is_gamepad)
            std::fprintf(stderr, " gp-type=%s", gamepad_type_name(SDL_GetGamepadTypeForID(id)));
        if (path != nullptr && path[0] != '\0')
            std::fprintf(stderr, " path=%s", path);
        if (pad >= 0)
            std::fprintf(stderr, " pad=%d\n", pad);
        else
            std::fprintf(stderr, " pad=none\n");
    }

    // The full list at this moment (start and every hotplug): one line per joystick, or "no controllers".
    void log_devices()
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks(&count);
        names.clear();
        if (count == 0)
            std::fprintf(stderr, "[don] input: no controllers\n");
        for (int i = 0; ids != nullptr && i < count; ++i)
        {
            const SDL_JoystickID id = ids[i];
            const char *name = SDL_GetJoystickNameForID(id);
            names[id] = name != nullptr ? name : "?";
            log_one_device(id);
            // A joystick SDL cannot map as a gamepad is not opened by our input: say so, separately, so a
            // report sees at a glance why a device that SDL lists is never used.
            if (!SDL_IsGamepad(id))
                std::fprintf(stderr, "[don] input: %s has no gamepad mapping; not used\n", name != nullptr ? name : "?");
        }
        SDL_free(ids);
    }

    const char *known_name(SDL_JoystickID id) const
    {
        const auto it = names.find(id);
        return it != names.end() ? it->second.c_str() : nullptr;
    }

    void log_added(SDL_JoystickID id)
    {
        const char *name = SDL_GetJoystickNameForID(id);
        std::fprintf(stderr, "[don] input: +\"%s\" (t=%.1fs)\n", name != nullptr ? name : "?", elapsed_s());
    }

    void log_removed(SDL_JoystickID id)
    {
        const char *name = known_name(id);
        std::fprintf(stderr, "[don] input: -\"%s\" (t=%.1fs)\n", name != nullptr ? name : "?", elapsed_s());
    }
};

SdlInput::SdlInput() : m_impl(std::make_unique<Impl>()) {}
SdlInput::~SdlInput() { shutdown(); }

bool SdlInput::init()
{
    if (m_impl->started)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
        return false;
    m_impl->started = true;
    m_impl->start_ticks = SDL_GetTicks();
    m_impl->log_steam_once();
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; ++i)
        m_impl->open(ids[i]);
    SDL_free(ids);
    m_impl->log_devices();
    return true;
}

void SdlInput::shutdown()
{
    if (!m_impl->started)
        return;
    for (SDL_Gamepad *p : m_impl->pads)
    {
        SDL_RumbleGamepad(p, 0, 0, 0); // a pad left shaking after the program ends
        SDL_CloseGamepad(p);
    }
    m_impl->pads.clear();
    m_impl->keys.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
    m_impl->started = false;
}

void SdlInput::handle_event(const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_EVENT_JOYSTICK_ADDED:
        // SDL sends GAMEPAD_ADDED right after for a joystick it can map as a gamepad; that handler logs it
        // once the gamepad is open, so the pad slot is already known. A joystick without a mapping is logged
        // here (and never used).
        if (!SDL_IsGamepad(e.jdevice.which))
        {
            m_impl->log_added(e.jdevice.which);
            m_impl->log_devices();
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        m_impl->open(e.gdevice.which);
        m_impl->log_added(e.gdevice.which);
        m_impl->log_devices();
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        m_impl->log_removed(e.gdevice.which);
        m_impl->close(e.gdevice.which);
        m_impl->log_devices();
        m_impl->removed_gamepads.insert(e.gdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        // SDL sends GAMEPAD_REMOVED first for a gamepad; it has already logged and re-listed the devices.
        if (m_impl->removed_gamepads.erase(e.jdevice.which) == 0)
        {
            m_impl->log_removed(e.jdevice.which);
            m_impl->log_devices();
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        Key k;
        if (!e.key.repeat && key_for_scancode(e.key.scancode, k))
            m_impl->keys.set(k, e.key.down);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_impl->keys.clear();
        break;
    default:
        break;
    }
}

Ps2HostPadState SdlInput::pad_state(int player) const
{
    if (player < 0 || player >= kMaxPlayers)
        return Ps2HostPadState{};
    Ps2HostPadState pad;
    if (player < static_cast<int>(m_impl->pads.size()))
        pad = device_to_pad_state(read_gamepad(m_impl->pads[player]));
    if (player == 0)
        return merge_pad_states(keyboard_to_pad_state(m_impl->keys), pad);
    return pad;
}

void SdlInput::set_rumble(int player, uint8_t small_motor, uint8_t large_motor)
{
    if (player < 0 || player >= kMaxPlayers || player >= static_cast<int>(m_impl->pads.size()))
        return;
    RumbleCommand command;
    if (m_impl->rumble[player].update(SDL_GetTicks(), small_motor, large_motor, command))
        SDL_RumbleGamepad(m_impl->pads[player], command.low_frequency, command.high_frequency, command.duration_ms);
}

int SdlInput::gamepad_count() const { return static_cast<int>(m_impl->pads.size()); }
KeySet SdlInput::keys() const { return m_impl->keys; }

} // namespace input
