// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui compile-time overrides for the start screen (T-521, docs/knowledge/22-netplay.md 7.3).
//
// PCSX2's vendored imconfig.h (third_party/pcsx2/3rdparty/imgui/include/imconfig.h) enables
// IMGUI_ENABLE_FREETYPE and links imgui_freetype.cpp + the FreeType and plutosvg libraries. The start
// screen does not need FreeType (Roboto is a plain TTF) and AGENTS.md keeps new dependencies out of the
// build, so this config switches the core back to the self-contained stb_truetype rasterizer that ships
// inside imgui_draw.cpp. The core sources themselves are still PCSX2's copy, compiled unchanged.
//
// Dear ImGui 1.92.8 includes IMGUI_USER_CONFIG *before* imconfig.h, so a plain #undef would be overwritten.
// imconfig.h is #pragma once, so including it here marks it seen and the #undef below then wins over the
// include that follows in imgui.h.
#pragma once

#include "imconfig.h"

#undef IMGUI_ENABLE_FREETYPE
#define IMGUI_ENABLE_STB_TRUETYPE
// The screen always loads Roboto + Font Awesome; the embedded default font is never needed.
#define IMGUI_DISABLE_DEFAULT_FONT
