// SPDX-License-Identifier: GPL-3.0-or-later
// Widgets and shared drawing for the start screen (T-523, docs/knowledge/22-netplay.md 7.3), the owner's
// LAS Recomp style, copied from LAS Recomp (runtime/ui/launcher.mm's text_fx and
// menu_item, runtime/ui/option_rows.mm's two-pane settings rows) and adapted to SDL_Renderer + Dear ImGui
// and to this port's model (the model owns the selection: each widget takes `selected` and returns clicks,
// it does not drive ImGui's own navigation). Everything is drawn with the window's draw list so the screen
// has no ImGui title bars and no debug look.
#pragma once

#include "imgui.h"
#include "theme.h"

#include <string>

namespace don_host::ui
{

// Once per frame, before any other widget: the window size (logical points). Sets the layout scale
// (height / 720) and the font scale (height / 1080).
void don_ui_frame_begin(float width, float height);

// The background (teal-navy gradient + vignette + the shadow banner), drawn to the background draw list
// so it sits behind every window. don_ui_screen() calls it; the Settings / Error screens call it before
// opening their own panel window.
void don_ui_background();

// The background (teal-navy gradient + vignette + the shadow banner behind the menu), then the title
// top-left and the cursor for the content that follows. Close with don_ui_frame_end(). gradient_title
// draws the title with the gold-to-orange gradient (the Main screen); otherwise flat gold. compact uses
// less space above and below the title (the lobby / join / error screens), so everything fits at 720 px.
void don_ui_screen(const char *title, bool gradient_title = false, bool compact = false);
void don_ui_frame_end();

// The left content margin and the content start y of the current screen (90 and 285 at 720 px).
float don_ui_x0();
float don_ui_content_y();

// Start-screen text (launcher.mm's text_fx): a soft drop shadow, then the text with a top-to-bottom
// colour gradient (top == bottom: flat).
void don_text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom,
                 const char *text, float shadow);

// A big menu line (launcher.mm's menu_item): gold text over a feathered dark band with a warm glow when
// `selected` or hovered, white otherwise, dim when disabled. The label may carry ‹ value › arrows (7.3);
// returns 2 when the body was clicked, -1 / +1 when an arrow was clicked, else 0 (0 also when disabled).
int don_menu_item(const char *label, bool selected, bool enabled = true);

// ---- the two-pane settings rows (option_rows.mm) -----------------------------------
// don_opt_begin opens the left pane (the rows); each don_opt_row is one row (uppercase label, value on
// the right, brighter when changed; ‹ › steppers when stepped); don_opt_end closes the pane and draws
// the selected / hovered row's description on the right. opt_note fills the description pane.
void don_opt_begin(const char *id);
void don_opt_section(const char *title);
// Returns 2 (clicked / activated), -1 / +1 (the ‹ › steppers clicked), or 0.
int don_opt_row(const char *label, const std::string &value, bool changed, bool selected, bool stepped,
                const char *desc);
void don_opt_note(const char *desc); // the description shown until a row is hovered / selected
void don_opt_end();

// A dropdown list for a settings row (one of a set of choices), opened next to the value. Returns the
// picked index or -1. The caller drives the model to that value.
int don_ui_dropdown(const char *id, const char *const *options, int n, int current);

// ---- rows shared by the lobby / join / error screens ---------------------------------
// A text-only action row (menu-item sized): label (uppercased when `upper`), an optional value on the
// right. Returns true when clicked / activated.
bool don_action_row(const char *label, const std::string &value, bool selected, bool upper = true);

// One lobby player row: name on the left (with a status dot; "(you)" when local), the ping on the right.
void don_player_row(const char *name, bool local, int ping_ms);

// A card holding `text` and a copy button (the join address goes to the clipboard). Returns true when copied.
bool don_copy_card(const std::string &text);

// A centred muted caption (colour overrides the default muted). Wraps at the content width.
void don_caption(const char *text, ImU32 color);

// A rotating arc spinner (radius, thickness in logical pixels) at the current cursor position.
void don_spinner(float radius, float thickness);

// A small status dot (green = ok, amber = warn, grey = neutral) at the current cursor position.
void don_status_dot(ImU32 color, float radius = 5.0f);

// The 150 ms fade overlay between screens: alpha 1 -> 0.
void don_ui_fade_overlay(float alpha);

} // namespace don_host::ui
