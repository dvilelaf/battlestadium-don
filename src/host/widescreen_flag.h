// SPDX-License-Identifier: GPL-3.0-or-later
// Widescreen (issue #2): what the host (host.cpp, gs_glue.cpp) and the game side (src/game/don_overrides.cpp) share.
//
// The game's fight camera is updated every frame by the camera routine 0x165900, which calls RenderWare's
// RwCameraSetViewWindow (0x2C45E0, wrapped) with x = tan(fov / 2) for a 4:3 picture; menus run that routine only
// when a screen is built (**Measured** with the fight_practice route: 1870 calls in 1900 blanks of fight, one to
// three per menu screen). So:
//   - the game side counts the calls that come from that routine (fight_camera_updates);
//   - the host, once per blank, calls it a fight when they came in at least 12 of the last 16 blanks, and no longer
//     a fight after 30 blanks without one (fight_active), and shows the picture 16:9 during a fight, 4:3 otherwise;
//   - during a fight, a perspective camera given a view window gets it 4/3 wider ("Hor+": more of the stage at the
//     sides, nothing stretched); the fight camera gets its window every frame. Outside a fight nothing changes, so
//     menus and their 3D backgrounds stay 4:3; the HUD's round pieces are kept round by widescreen_hud.h.
// Off in online play: the view window is guest memory, which the netplay hash compares between the PCs.
#pragma once

#include <atomic>
#include <cstdint>

namespace don_widescreen
{
// Fight detection without widescreen (fast loading needs to know a fight from a loading screen). Set by the host
// before the runtime starts; the camera hook then counts fight camera updates whatever the widescreen setting.
inline std::atomic<bool> &fight_tracking()
{
    static std::atomic<bool> on{false};
    return on;
}

// The setting. The host sets it before the runtime starts and never changes it.
inline std::atomic<bool> &enabled()
{
    static std::atomic<bool> on{false};
    return on;
}

// Calls of RwCameraSetViewWindow from the fight camera routine since the host last looked (game thread adds).
inline std::atomic<uint32_t> &fight_camera_updates()
{
    static std::atomic<uint32_t> count{0};
    return count;
}

// A fight is on screen (host writes once per blank, game thread reads).
inline std::atomic<bool> &fight_active()
{
    static std::atomic<bool> on{false};
    return on;
}
} // namespace don_widescreen
