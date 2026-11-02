// SPDX-License-Identifier: GPL-3.0-or-later
// Line breaking of a setup screen's title (T-715): a title is never cut by the window edge, it wraps at a space.
// No ImGui here, so the model tests include it; the caller passes the text measure.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace don_setup
{
// Splits `text` at spaces so that each line is at most `max_width` wide by `width`. The first line has `first_indent`
// less room (the tick is drawn there). A word wider than a line stays whole on its own line; nothing is dropped.
inline std::vector<std::string> wrap_title(const std::string &text, float max_width, float first_indent,
                                           const std::function<float(const std::string &)> &width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i <= text.size())
    {
        const std::size_t end = std::min(text.find(' ', i), text.size());
        const std::string word = text.substr(i, end - i);
        const std::string tried = line.empty() ? word : line + " " + word;
        const float room = max_width - (lines.empty() ? first_indent : 0.0f);
        if (!line.empty() && width(tried) > room)
        {
            lines.push_back(line);
            line = word;
        }
        else
            line = tried;
        i = end + 1;
    }
    lines.push_back(line);
    return lines;
}
} // namespace don_setup
