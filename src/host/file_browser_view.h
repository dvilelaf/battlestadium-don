// SPDX-License-Identifier: GPL-3.0-or-later
// The in-app file browser on screen (A3 S1 BROWSE, A4 E02) and the system file dialog (CHOOSE DISC IMAGE...). The
// rows, verdicts and keys are FileBrowserModel's (file_browser.h); this file draws a BrowserView with the setup
// screen's helpers and owns the one browser that is open at a time. Only setup_screen.cpp and the demo use it.
#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "file_browser.h"
#include "imgui.h"
#include "setup_browser_routing.h"
#include "setup_screen_model.h"

namespace don_host
{

// --- the in-app browser ---
bool file_browser_active();
// Start places for the next file_browser_open(); empty (the default) = the S0 folders under the user's home.
void file_browser_set_start_places(std::vector<std::filesystem::path> places);
void file_browser_open();
// The home folder and the user name the start places are built from ("" when unknown).
void file_browser_home(std::string &home, std::string &user);
void file_browser_close();
// One key or button. Returns the chosen path (the browser closes); Back at the start list closes the browser.
std::optional<std::string> file_browser_navigate(don_setup::Nav nav);
// Replaces the whole screen while the browser is open; the same frame rules as setup_screen_draw. Hover, click and the
// wheel are routed to the model here (setup_browser_routing.h).
void file_browser_draw(ImVec2 size, don_setup::SetupModel &model);
// The browser and dialog state that setup_screen.cpp routes events through.
BrowserRouter &file_browser_router();

// --- the app's side (defined in setup_screen.cpp) ---
// For each action taken out of the model: OpenFilePicker shows the system dialog, OpenBrowser opens the in-app browser.
// Returns true when the action was one of those two; the app handles the others.
bool setup_screen_handle_action(don_setup::SetupModel &model, const don_setup::ActionRequest &request);

// --- the system file dialog ---
// Asks SDL for the system's open-file dialog; `seq` is the request number (BrowserRouter::begin_dialog) that its
// answer carries. The answer arrives later, maybe from another thread; read it with file_dialog_poll() once per frame.
void file_dialog_open(std::uint64_t seq);
std::optional<DialogResult> file_dialog_poll();

} // namespace don_host
