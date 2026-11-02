// SPDX-License-Identifier: GPL-3.0-or-later
// The DON_SETUP_SCRIPT test hook of the first-run screens (T-704, T-709): parsing the script and applying one step.
//
//   up, down, left, right, confirm, back   a navigation key, fed once the screen stayed the same for the unchanged
//                                          frames and nothing is running
//   now:<key>                              the same key, but also while the game is being checked or prepared
//   drop:<path>                            a file dropped on the window (SDL_EVENT_DROP_FILE); the path has no comma
//   now:drop:<path>                        the same drop while busy
#pragma once

#include <string>
#include <vector>

#include "setup_screen_model.h"

namespace don_host {

struct ScriptStep {
    enum class Kind { Key, Drop };
    Kind kind = Kind::Key;
    don_setup::Nav nav = don_setup::Nav::Confirm;   // Kind::Key
    std::string path;                               // Kind::Drop
    bool now = false;                               // fed even while busy
};

// An unknown key is reported on stderr and skipped.
std::vector<ScriptStep> parse_script(const char* text);

const char* setup_screen_name(don_setup::Screen s);

#ifndef DON_SETUP_RUN_NO_SDL
// A key goes where a pressed key goes; a drop goes through setup_screen_event, as a real drop does.
void apply_script_step(don_setup::SetupModel& model, const ScriptStep& step);
#endif

}  // namespace don_host
