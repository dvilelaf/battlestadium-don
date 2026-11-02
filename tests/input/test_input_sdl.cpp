// T-111: the SDL3 side, run in the build container with SDL's dummy drivers (no gamepad, no display):
// starts the subsystem, feeds synthetic key events and checks the pad state they give.
#include "check.h"
#include "input_sdl.h"

#include <SDL3/SDL.h>

#include <cstdlib>

using namespace input;

namespace
{
SDL_Event key_event(SDL_Scancode sc, bool down, bool repeat = false)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}
} // namespace

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SdlInput in;
    CHECK(in.init());
    CHECK(in.init()); // idempotent
    CHECK_EQ(in.gamepad_count(), 0);

    // Player 0 is connected through the keyboard even without a gamepad; the others are not.
    Ps2HostPadState s = in.pad_state(0);
    CHECK_EQ(s.connected, 1);
    CHECK_EQ(s.buttons, 0xFFFF);
    CHECK_EQ(in.pad_state(1).connected, 0);
    CHECK_EQ(in.pad_state(3).connected, 0);
    CHECK_EQ(in.pad_state(4).connected, 0);
    CHECK_EQ(in.pad_state(-1).connected, 0);

    // L = Circle, Enter = Start, arrows = D-pad.
    in.handle_event(key_event(SDL_SCANCODE_L, true));
    in.handle_event(key_event(SDL_SCANCODE_RETURN, true));
    s = in.pad_state(0);
    CHECK(is_pad_button_pressed(s, PadButton::Circle));
    CHECK(is_pad_button_pressed(s, PadButton::Start));
    CHECK(!is_pad_button_pressed(s, PadButton::Cross));
    CHECK_EQ(s.pressure[pressure_index(PadButton::Circle)], 0xFF);

    // Key repeat does not release or change anything; key up releases.
    in.handle_event(key_event(SDL_SCANCODE_L, true, true));
    CHECK(is_pad_button_pressed(in.pad_state(0), PadButton::Circle));
    in.handle_event(key_event(SDL_SCANCODE_L, false));
    CHECK(!is_pad_button_pressed(in.pad_state(0), PadButton::Circle));
    CHECK(is_pad_button_pressed(in.pad_state(0), PadButton::Start));

    // A key that is not in the table is ignored.
    in.handle_event(key_event(SDL_SCANCODE_F5, true));
    CHECK(is_pad_button_pressed(in.pad_state(0), PadButton::Start));

    in.handle_event(key_event(SDL_SCANCODE_RIGHT, true));
    CHECK(is_pad_button_pressed(in.pad_state(0), PadButton::Right));

    // Losing the focus releases every key.
    SDL_Event lost;
    SDL_zero(lost);
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    in.handle_event(lost);
    CHECK_EQ(in.pad_state(0).buttons, 0xFFFF);
    CHECK_EQ(in.keys().bits, 0);

    // An added/removed event for a device that does not exist is harmless.
    SDL_Event gone;
    SDL_zero(gone);
    gone.type = SDL_EVENT_GAMEPAD_REMOVED;
    gone.gdevice.which = 12345;
    in.handle_event(gone);
    gone.type = SDL_EVENT_GAMEPAD_ADDED;
    in.handle_event(gone);
    CHECK_EQ(in.gamepad_count(), 0);

    // T-308: vibration reaches a gamepad. SDL's virtual joystick (no hardware: nothing real is ever driven) of
    // the gamepad type, with a rumble callback that records what it is asked.
    {
        struct Rumbled
        {
            int calls = 0;
            Uint16 low = 0, high = 0;
        } rumbled;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "don test pad";
        desc.userdata = &rumbled;
        desc.Rumble = [](void *user, Uint16 low, Uint16 high) -> bool {
            auto *r = static_cast<Rumbled *>(user);
            ++r->calls;
            r->low = low;
            r->high = high;
            return true;
        };
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        CHECK(id != 0);
        if (id != 0)
        {
            SDL_Event added;
            SDL_zero(added);
            added.type = SDL_EVENT_GAMEPAD_ADDED;
            added.gdevice.which = id;
            in.handle_event(added);
            CHECK_EQ(in.gamepad_count(), 1);

            // Player 0 has the gamepad: the large motor alone, then both, then off.
            in.set_rumble(0, 0, 200);
            CHECK_EQ(rumbled.calls, 1);
            CHECK_EQ(rumbled.low, 200 * 257);
            CHECK_EQ(rumbled.high, 0);
            in.set_rumble(0, 0, 200); // held, not renewed yet: no new call
            CHECK_EQ(rumbled.calls, 1);
            in.set_rumble(0, 1, 200);
            CHECK_EQ(rumbled.high, 0xFFFF);
            CHECK_EQ(rumbled.low, 200 * 257);
            const int before_stop = rumbled.calls;
            in.set_rumble(0, 0, 0);
            CHECK_EQ(rumbled.calls, before_stop + 1);
            CHECK_EQ(rumbled.low, 0);
            CHECK_EQ(rumbled.high, 0);

            // Players without a gamepad get nothing and nothing breaks (the keyboard has no motors).
            const int quiet = rumbled.calls;
            in.set_rumble(1, 1, 255);
            in.set_rumble(3, 1, 255);
            in.set_rumble(4, 1, 255);
            in.set_rumble(-1, 1, 255);
            CHECK_EQ(rumbled.calls, quiet);

            // A running motor: the renewal is the schedule's (test_input_rumble); SDL itself does not call the driver
            // again for the values it already holds, it only extends their time, so it is not visible here.
            in.set_rumble(0, 0, 90);
            CHECK_EQ(rumbled.low, 90 * 257);
            SDL_Delay(RumbleSchedule::kRenewMs + 50);
            in.set_rumble(0, 0, 90);
            CHECK_EQ(rumbled.low, 90 * 257);

            // Leaving switches the motors off.
            in.shutdown();
            CHECK_EQ(rumbled.low, 0);
            CHECK_EQ(rumbled.high, 0);
            SDL_DetachVirtualJoystick(id);
            return check_result("test_input_sdl");
        }
    }

    in.shutdown();
    return check_result("test_input_sdl");
}
