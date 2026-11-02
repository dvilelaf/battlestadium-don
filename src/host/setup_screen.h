// SPDX-License-Identifier: GPL-3.0-or-later
// The first-run setup screen's drawing (A3 in docs/tasks/M6-public-release.md): draws a don_setup::View with the
// start screen's theme and widgets and maps input to SetupModel calls. It owns no text: every string is the view's.
#pragma once

#include <SDL3/SDL.h>

#include "setup_screen_model.h"

namespace don_host
{

// Once per frame, between ImGui::NewFrame() and ImGui::Render(), after ui::don_ui_frame_begin().
// Mouse hover and clicks on the menu go to model.select() / model.activate().
void setup_screen_draw(don_setup::SetupModel &model, const don_setup::View &view);

// One SDL event: keyboard and gamepad to model.navigate(), a dropped file to model.file_chosen(), the window's
// close button to model.request_close().
void setup_screen_event(don_setup::SetupModel &model, const SDL_Event &event);

} // namespace don_host

// --- drawing parts, defined in setup_screen_parts.cpp (used by setup_screen.cpp only) ---
#include <string>
#include <vector>

#include "imgui.h"

namespace don_host
{
// The model's strings carry "✓" and "◂▸", which Nunito cannot draw; the drawing finds them and draws shapes instead.
inline constexpr const char *kTick = "\xe2\x9c\x93";                  // U+2713
inline constexpr const char *kStepper = "\xe2\x97\x82\xe2\x96\xb8"; // U+25C2 U+25B8

float tick_width(float size);
void draw_tick(ImDrawList *dl, ImVec2 p, float size, ImU32 col);
float text_at(ImVec2 p, ImU32 col, const std::string &s, float wrap, bool bold = false);  // returns the height
float text_width(const std::string &s);
float draw_rows(ImVec2 p, float width, const std::vector<don_setup::Row> &rows);
float draw_rail(ImVec2 p, const std::vector<don_setup::Row> &steps);
void draw_bar(ImVec2 p, float width, double fraction);
void draw_menu(don_setup::SetupModel &model, const don_setup::View &view, float left_w);
void draw_error_card(don_setup::SetupModel &model, const don_setup::View &view, ImVec2 size);
} // namespace don_host
