// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen's look (T-523, docs/knowledge/22-netplay.md section 7.3), the owner's LAS Recomp
// style: a calm dark teal-navy background, one rounded sans (Nunito) for everything, gold text for the
// selected menu item over a feathered dark band, and two-pane settings rows. The palette, the type scale
// and the font sizes are the values of 7.3; the colours come from the owner's LAS Recomp launcher
// (LAS Recomp, runtime/ui/launcher.mm and runtime/ui/option_rows.mm).
#pragma once

#include "imgui.h"

namespace don_host::ui
{

// ---- palette (7.3 "Background" and "Main screen") ---------------------------------
// The four corners of the full-window gradient (launcher.mm: tl #122228, tr #28463F, br #0C1418,
// bl #080C10), with a soft vignette over it.
inline constexpr ImVec4 kBgTL{0.0706f, 0.1333f, 0.1569f, 1.0f};  // #122228
inline constexpr ImVec4 kBgTR{0.1569f, 0.2745f, 0.2471f, 1.0f};  // #28463F
inline constexpr ImVec4 kBgBR{0.0471f, 0.0784f, 0.0941f, 1.0f};  // #0C1418
inline constexpr ImVec4 kBgBL{0.0314f, 0.0471f, 0.0627f, 1.0f};  // #080C10

inline constexpr ImU32 kGold = IM_COL32(255, 214, 64, 255);      // #FFD640: the selected / hovered text
inline constexpr ImU32 kGoldHot = IM_COL32(255, 160, 50, 255);   // #FFA032: its bottom gradient colour
inline constexpr ImU32 kGoldSoft = IM_COL32(236, 194, 84, 255);  // #ECC254: the settings rows' gold
inline constexpr ImU32 kText = IM_COL32(250, 250, 255, 255);     // #FAFAFF: menu text
inline constexpr ImU32 kTextDim = IM_COL32(170, 175, 200, 180);  // a disabled item
inline constexpr ImU32 kMuted = IM_COL32(205, 205, 195, 255);    // secondary text (rows, footers)
inline constexpr ImU32 kMutedDim = IM_COL32(150, 150, 142, 255); // the faintest text
inline constexpr ImU32 kRowLabel = IM_COL32(240, 236, 224, 255); // settings row label
inline constexpr ImU32 kGreen = IM_COL32(140, 230, 140, 255);    // the "ready" check
inline constexpr ImU32 kWarn = IM_COL32(255, 184, 77, 255);      // a warning line
inline constexpr ImU32 kError = IM_COL32(255, 92, 92, 255);      // an error line

// The feathered band behind a selected menu item (launcher.mm's menu_item): a dark navy fill with a
// warm gold glow fading to the right.
inline constexpr ImU32 kBandDark = IM_COL32(6, 10, 34, 150);   // #060A22
inline constexpr ImU32 kBandGlow = IM_COL32(255, 190, 80, 34); // #FFBE50 at 13 %

// ---- type scale (pixel sizes at a 1080 px window height, scaled with the window) ----
inline constexpr float kSizeTitle = 92.0f;
inline constexpr float kSizeMenu = 52.0f;
inline constexpr float kSizeSmall = 30.0f;

// ---- fonts --------------------------------------------------------------------
// Font handles after don_ui_load_fonts(); nullptr when the embedded font could not be built.
inline ImFont *g_font_title = nullptr; // Nunito ExtraBold 92
inline ImFont *g_font_menu = nullptr;  // Nunito ExtraBold 52
inline ImFont *g_font_small = nullptr; // Nunito Regular 30

// Loads Nunito from the byte arrays embedded at build time (ui/fonts/fonts_embedded.cpp). Returns
// false (and leaves the fonts null) when the atlas could not build them.
bool don_ui_load_fonts();

// Applies the palette and shape settings to the current ImGui style, and the nav flags to the ImGui IO.
void don_ui_apply_theme();

// The current layout scale: the window height / 720 (7.3: "720 = 1.0"). Fonts scale by height / 1080.
// don_ui_frame_begin sets it every frame; don_ui_set_scale lets the screenshots force one for tests.
void don_ui_set_scale(float scale);
float don_ui_scale();

// S(v) multiplies a layout constant by the current scale; used for every position and spacing.
float don_ui_S(float v);

} // namespace don_host::ui
