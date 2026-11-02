// The text and numbers the setup screen shows (SetupModel::view() and the menus) and the small helpers behind them.
// Split from setup_screen_model.cpp, which holds the states and transitions.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "setup_screen_model.h"

namespace don_setup {

std::string sanitize_utf8(const std::string& s);  // invalid UTF-8 and control characters become U+FFFD
std::string base_name(const std::string& path);   // the file name of a path with / or \ separators, sanitised
int percent_of(const Progress& p);                // 0..99 until Done, then 100

// Replaces each "{key}" of `text` by its value (A4 placeholders); unknown keys stay as they are.
std::string fill_placeholders(std::string text, const std::vector<std::pair<std::string, std::string>>& values);

}  // namespace don_setup
