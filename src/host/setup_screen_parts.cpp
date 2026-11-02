// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing parts of the first-run setup screen shared by setup_screen.cpp: text with the glyphs Nunito lacks, marks,
// rows, the rail, the bar and the S5 card. Only the view's strings are drawn.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "imgui.h"
#include "setup_screen.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace don_host
{
using don_setup::Mark;
using don_setup::Row;

static float S(float v) { return ui::don_ui_S(v); }

// Layout of the S5 error card (unscaled pixels, run through S()).
constexpr float kCardPad = 32.0f;          // inner padding
constexpr float kCardSideMargin = 40.0f;   // the card keeps this far from the window's sides
constexpr float kCardMaxWidth = 760.0f;    // the old overlay's width
constexpr float kCardMinTop = 40.0f;       // least distance from the window's top
constexpr float kCardTopFraction = 0.14f;  // card top as a fraction of the window height
constexpr float kCardMenuGap = 10.0f;      // gap between the text and the menu (and below it)
constexpr float kCardRounding = 14.0f;     // corner radius
constexpr float kMenuRowFactor = 1.16f;    // don_menu_item's row: font size plus its 0.16 line spacing
constexpr ImU32 kCardDim = IM_COL32(0, 0, 0, 170);  // dim over the screen behind (the old overlay's)
constexpr ImU32 kCardBg = IM_COL32(16, 28, 34, 245);  // card fill (the old overlay's)

// The model's strings carry "✓" and "◂▸", which Nunito cannot draw (it would show "?"). They never reach the
// font: the drawing finds them and draws the shapes the project already uses (start_screen.cpp's check mark,
// widgets.cpp's triangles).

static bool has_glyphs(const std::string &s) { return s.find(kTick) != std::string::npos || s.find(kStepper) != std::string::npos; }

float tick_width(float size) { return size * 0.9f; }
static float stepper_width(float size) { return size * (0.42f * 2.0f + 0.55f); }

void draw_tick(ImDrawList *dl, ImVec2 p, float size, ImU32 col)
{
    dl->AddLine(ImVec2(p.x + size * 0.1f, p.y + size * 0.55f), ImVec2(p.x + size * 0.35f, p.y + size * 0.8f), col, size * 0.12f);
    dl->AddLine(ImVec2(p.x + size * 0.35f, p.y + size * 0.8f), ImVec2(p.x + size * 0.85f, p.y + size * 0.2f), col, size * 0.12f);
}

static void draw_stepper(ImDrawList *dl, ImVec2 p, float size, ImU32 col)
{
    const float aw = size * 0.42f, gap = size * 0.55f, cy = p.y + size * 0.52f;
    dl->AddTriangleFilled(ImVec2(p.x, cy), ImVec2(p.x + aw, cy - aw * 0.75f), ImVec2(p.x + aw, cy + aw * 0.75f), col);
    const float rx = p.x + aw + gap;
    dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx + aw, cy - aw * 0.75f), ImVec2(rx + aw, cy + aw * 0.75f), col);
}

// One line of text that may carry the glyphs above, drawn left to right with the font at `size`. Returns the
// end x. Text without glyphs goes through the font as usual.
static float draw_line_glyphs(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 col, const std::string &s)
{
    float x = p.x;
    std::size_t i = 0;
    while (i < s.size())
    {
        const std::size_t t = s.find(kTick, i), st = s.find(kStepper, i);
        const std::size_t n = std::min(t, st);
        const std::string seg = s.substr(i, n == std::string::npos ? std::string::npos : n - i);
        if (!seg.empty())
        {
            dl->AddText(font, size, ImVec2(x, p.y), col, seg.c_str());
            x += font->CalcTextSizeA(size, 100000.0f, 0.0f, seg.c_str()).x;
        }
        if (n == std::string::npos)
            break;
        if (n == t)
        {
            draw_tick(dl, ImVec2(x, p.y), size, col);
            x += tick_width(size);
            i = n + std::char_traits<char>::length(kTick);
        }
        else
        {
            draw_stepper(dl, ImVec2(x, p.y), size, col);
            x += stepper_width(size);
            i = n + std::char_traits<char>::length(kStepper);
        }
    }
    return x;
}

// Wrapped text in the small font at a screen position; returns its height.
float text_at(ImVec2 p, ImU32 col, const std::string &s, float wrap, bool bold)
{
    if (s.empty())
        return 0.0f;
    ImFont *font = bold ? ui::g_font_menu : ui::g_font_small;
    ImGui::PushFont(font, font->LegacySize);
    const float size = bold ? ImGui::GetFontSize() * 0.62f : ImGui::GetFontSize();
    if (has_glyphs(s))
    {
        draw_line_glyphs(ImGui::GetWindowDrawList(), ImGui::GetFont(), size, p, col, s);
        ImGui::PopFont();
        return size + S(6.0f);
    }
    const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(size, 100000.0f, wrap, s.c_str());
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, p, col, s.c_str(), nullptr, wrap);
    ImGui::PopFont();
    return sz.y + S(6.0f);
}

float text_width(const std::string &s)
{
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    ImGui::PopFont();
    return w;
}

// The mark of a row: a green tick, a gold ring with a dot, or a hollow grey ring.
static void draw_mark(ImDrawList *dl, ImVec2 c, float r, Mark m)
{
    switch (m)
    {
    case Mark::Done:
        dl->AddCircleFilled(c, r, ui::kGreen, 24);
        dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f),
                                              ImVec2(c.x + r * 0.5f, c.y - r * 0.4f)}.data(),
                        3, IM_COL32(12, 24, 20, 255), S(2.0f), 0);
        break;
    case Mark::Active:
        dl->AddCircle(c, r, ui::kText, 24, S(2.0f));
        dl->AddCircleFilled(c, r * 0.5f, ui::kText, 16);
        break;
    case Mark::Pending: dl->AddCircle(c, r, ui::kMutedDim, 24, S(1.5f)); break;
    case Mark::None: break;
    }
}

// Rows with marks, one per line: mark, text, and the detail on the right. Returns the height.
float draw_rows(ImVec2 p, float width, const std::vector<Row> &rows)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float y = 0.0f;
    for (const Row &r : rows)
    {
        const bool mark = r.mark != Mark::None;
        const float tx = p.x + (mark ? S(26.0f) : 0.0f);
        if (mark)
            draw_mark(dl, ImVec2(p.x + S(9.0f), p.y + y + S(11.0f)), S(8.0f), r.mark);
        const ImU32 col = r.mark == Mark::Pending ? ui::kMutedDim : ui::kText;
        float h = text_at(ImVec2(tx, p.y + y), col, r.text, width - (tx - p.x));
        if (!r.detail.empty())
            h = std::max(h, text_at(ImVec2(tx, p.y + y + h - S(4.0f)), ui::kMuted, r.detail, width - (tx - p.x)) +
                                h - S(4.0f));
        y += h + S(6.0f);
    }
    return y;
}

// The steps rail: one row of marked steps under the title.
float draw_rail(ImVec2 p, const std::vector<Row> &steps)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float x = 0.0f;
    for (const Row &r : steps)
    {
        draw_mark(dl, ImVec2(p.x + x + S(9.0f), p.y + S(11.0f)), S(8.0f), r.mark);
        const ImU32 col = r.mark == Mark::Pending ? ui::kMutedDim : ui::kText;
        text_at(ImVec2(p.x + x + S(26.0f), p.y), col, r.text, 10000.0f);
        x += S(26.0f) + text_width(r.text) + S(36.0f);
    }
    return S(34.0f);
}

void draw_bar(ImVec2 p, float width, double fraction)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float h = S(12.0f);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(255, 255, 255, 40), h / 2);
    const float f = static_cast<float>(std::min(1.0, std::max(0.0, fraction)));
    if (f > 0.0f)
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + std::max(h, width * f), p.y + h), ui::kGold, ui::kGoldHot,
                                    ui::kGoldHot, ui::kGold);
}


// The menu: hover selects, a click activates, the focused item (or the primary one while nothing is focused) is gold,
// a disabled one is muted. The main screen and the S5 card both draw their menus here. The cursor is already placed.
void draw_menu(don_setup::SetupModel &model, const don_setup::View &v, float left_w)
{
    const float x0 = ui::don_ui_x0();
    for (std::size_t i = 0; i < v.menu.size(); ++i)
    {
        const don_setup::MenuItem &m = v.menu[i];
        const bool sel = static_cast<int>(i) == v.selected;
        if (m.label.find(kStepper) != std::string::npos)
        {
            // A stepped item (SPEED, GAME VERSION): the settings row with ◀ value ▶ drawn as triangles.
            std::string label = m.label.substr(0, m.label.find(kStepper));
            const std::string &value = m.value;
            label = label.substr(0, label.find(':'));  // "SPEED: FAST " -> "SPEED"
            while (!label.empty() && label.back() == ' ')
                label.pop_back();
            ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
            const float rh = ImGui::GetFontSize() * 1.9f;
            ImGui::PopFont();
            // don_opt_row indents its rows by x0 itself, so the child starts at the current window's left edge.
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginChild("##stepper", ImVec2(x0 + std::min(left_w, S(520.0f)), rh), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const int sr = ui::don_opt_row(label.c_str(), value, false, sel || (m.primary && v.selected < 0), true, nullptr);
            const bool hov = ImGui::IsWindowHovered();
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, S(8.0f)));
            if (hov && !sel && m.enabled)
                model.select(static_cast<int>(i));
            if (sr == -1)
                model.navigate(don_setup::Nav::Left);
            else if (sr == 1)
                model.navigate(don_setup::Nav::Right);
            else if (sr == 2)
                model.activate(static_cast<int>(i));
            continue;
        }
        if (sel && !m.enabled)
        {
            // don_menu_item draws the focus band only for an enabled item; a disabled focused item (CONTINUE while the
            // space is missing) gets the same band behind its muted text, so one item always shows the focus.
            ImGui::PushFont(ui::g_font_menu, ui::g_font_menu->LegacySize);
            const ImVec2 at = ImGui::GetCursorScreenPos(), sz = ImGui::CalcTextSize(m.label.c_str());
            ImGui::PopFont();
            const float xw = ImGui::GetWindowPos().x, x2 = at.x + sz.x + sz.y * 3.5f, f = sz.y * 0.22f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(xw, at.y - f * 0.4f), ImVec2(x2, at.y + sz.y + f * 0.2f), ui::kBandDark);
            dl->AddRectFilled(ImVec2(xw, at.y - f * 0.4f), ImVec2(x2, at.y + sz.y + f * 0.2f), ui::kBandGlow);
        }
        // The primary item is the gold one; the others are white until hovered or selected.
        const int r = ui::don_menu_item(m.label.c_str(), sel || (m.primary && v.selected < 0), m.enabled);
        if (ImGui::IsItemHovered() && !sel && m.enabled)
            model.select(static_cast<int>(i));
        if (r != 0)
            model.activate(static_cast<int>(i));
    }
}

// The S5 error panel: the screen behind is dimmed and a card shows the code, what happened, why and what to do, then
// the view's menu (the ErrorId's primary action in gold, COPY REPORT). The focused item gets an outline.
void draw_error_card(don_setup::SetupModel &model, const don_setup::View &v, ImVec2 size)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##setup-error", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), size, kCardDim);

    const float pad = S(kCardPad), w = std::min(size.x - 2 * S(kCardSideMargin), S(kCardMaxWidth)), ww = w - 2 * pad;
    const ImVec2 p((size.x - w) / 2, std::max(S(kCardMinTop), size.y * kCardTopFraction));
    const std::string *texts[4] = {&v.error.code, &v.error.title, &v.error.why, &v.error.todo};
    const ImU32 cols[4] = {ui::kMutedDim, ui::kText, ui::kText, ui::kMuted};
    const bool bold[4] = {false, true, false, false};
    float hs[4];
    for (int i = 0; i < 4; ++i)
    {
        ImFont *f = bold[i] ? ui::g_font_menu : ui::g_font_small;
        ImGui::PushFont(f, f->LegacySize);
        const float sz = bold[i] ? ImGui::GetFontSize() * 0.62f : ImGui::GetFontSize();
        hs[i] = texts[i]->empty() ? 0.0f : f->CalcTextSizeA(sz, 100000.0f, ww, texts[i]->c_str()).y + S(6.0f);
        ImGui::PopFont();
    }
    // One menu row is what don_menu_item advances by: the text height plus its line spacing.
    ImGui::PushFont(ui::g_font_menu, ui::g_font_menu->LegacySize);
    const float bh = ImGui::GetFontSize() * kMenuRowFactor;
    ImGui::PopFont();
    float h = 2 * pad + static_cast<float>(v.menu.size()) * bh + S(kCardMenuGap);
    for (float x : hs)
        h += x + S(6.0f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kCardBg, S(kCardRounding));

    float y = p.y + pad;
    for (int i = 0; i < 4; ++i)
    {
        text_at(ImVec2(p.x + pad, y), cols[i], *texts[i], ww, bold[i]);
        y += hs[i] + S(6.0f);
    }
    y += S(kCardMenuGap);
    // don_menu_item indents by the screen margin; a child that starts that far left of the card's text puts it at the
    // card's padding, and the clip keeps its shadow band inside the card.
    const float x0 = ui::don_ui_x0(), cx = p.x + pad - x0;
    ImGui::SetCursorScreenPos(ImVec2(cx, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##card-menu", ImVec2(p.x + w - cx, p.y + h - y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
    draw_menu(model, v, ww);
    ImGui::PopClipRect();
    ImGui::EndChild();
    ImGui::End();
}

} // namespace don_host
