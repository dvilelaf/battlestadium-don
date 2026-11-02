// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen's font, embedded in the binary (T-523, docs/knowledge/22-netplay.md 7.3).
//
// Nunito, SIL Open Font License 1.1 (OFL.txt sits beside the .ttf files in this folder).
// Origin: the owner's LAS Recomp start screen uses one rounded sans for everything; this port
// uses Nunito from the official source, github.com/googlefonts/nunito (the pinned commit
// 8c6a9bb9732545b9ed53f29ec5e1ab0ff53c4e6f). That repository ships the variable font
// fonts/variable/Nunito[wght].ttf; the two static instances below (ExtraBold 800 for the title
// and the menu, Regular 400 for the small text) were instantiated from it with fonttools
// (varLib.instancer) and are committed here so the build needs no font tool.
//
// CMake reads the .ttf files at configure time and turns them into byte arrays
// (src/host/ui/fonts/embed_fonts.cmake -> ui/fonts/fonts_embedded.cpp in the build folder), so
// the font ships inside the binary and no font file is looked up at run time.
#pragma once

namespace don_host::ui::fonts
{

extern const unsigned char kExtraBold[]; // Nunito-ExtraBold.ttf (menu + title)
extern const unsigned int kExtraBoldSize;
extern const unsigned char kRegular[];   // Nunito-Regular.ttf (small text)
extern const unsigned int kRegularSize;

} // namespace don_host::ui::fonts
