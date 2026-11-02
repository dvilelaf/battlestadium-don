// SPDX-License-Identifier: GPL-3.0-or-later
// widgets.cpp: see widgets.h. All coordinates are logical points (ImGui's space); the font scale
// (height / 1080) is applied through ImGui::GetIO().FontGlobalScale and the layout scale (height / 720)
// through don_ui_S(). The palette, the feathered menu band and the settings rows come from the owner's
// LAS Recomp launcher (see widgets.h).
#include "widgets.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace don_host::ui
{
namespace
{

float g_display_w = 0.0f;
float g_display_h = 0.0f;

ImU32 col(const ImVec4 &c) { return ImGui::ColorConvertFloat4ToU32(c); }

float x0() { return don_ui_S(90.0f); }         // the left content margin
float content_y() { return don_ui_S(285.0f); } // where the menu / rows start
float content_width() { return std::max(0.0f, g_display_w - 2.0f * x0()); }

// The soft vignette: the edges fall off a little (7.3 "soft vignette").
void vignette(ImDrawList *dl)
{
    const float w = g_display_w, h = g_display_h, m = don_ui_S(90.0f);
    const ImU32 a = IM_COL32(0, 0, 0, 0), b = IM_COL32(0, 0, 0, 56);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, m), b, b, a, a);         // top
    dl->AddRectFilledMultiColor(ImVec2(0, h - m), ImVec2(w, h), a, a, b, b);     // bottom
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(m, h), b, a, a, b);         // left
    dl->AddRectFilledMultiColor(ImVec2(w - m, 0), ImVec2(w, h), a, b, b, a);     // right
}

// The shadow banner fading in from the left behind the menu (launcher.mm).
void shadow_banner(ImDrawList *dl)
{
    const float stops[][2] = {{0.0f, 225}, {0.3f, 215}, {0.44f, 165}, {0.55f, 95}, {0.64f, 35}, {0.74f, 0}};
    for (int i = 0; i + 1 < static_cast<int>(sizeof stops / sizeof stops[0]); ++i)
    {
        const ImU32 a = IM_COL32(6, 10, 34, static_cast<int>(stops[i][1]));
        const ImU32 b = IM_COL32(6, 10, 34, static_cast<int>(stops[i + 1][1]));
        dl->AddRectFilledMultiColor(ImVec2(g_display_w * stops[i][0], 0),
                                    ImVec2(g_display_w * stops[i + 1][0], g_display_h), a, b, b, a);
    }
}

// ---- settings rows' description state ------------------------------------------
std::string g_desc_sel, g_desc_hover, g_desc_first, g_desc_shown;
bool g_in_pane = false; // don_opt_begin .. don_opt_end: rows are inside the settings left pane

// The row body (option_rows.mm's row()): draws the box + label + value and handles the click / steppers.
// Returns 2 (clicked), -1 / +1 (‹ › stepped) or 0.
int row(const char *label, const std::string &value, bool changed, bool selected, bool stepped,
        const char *desc, bool upper)
{
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
    ImGui::PopStyleColor();
    const bool hov = ImGui::IsItemHovered();
    const bool hot = hov || selected;

    if (selected)
        g_desc_sel = desc ? desc : "";
    if (hov)
        g_desc_hover = desc ? desc : "";
    if (g_desc_first.empty() && desc)
        g_desc_first = desc;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float r = 9.0f, pad = don_ui_S(16.0f), fs = ImGui::GetFontSize();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hot ? IM_COL32(255, 255, 255, 22) : IM_COL32(0, 0, 0, 40), r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), hot ? kGoldSoft : IM_COL32(255, 255, 255, 38), r,
                hot ? 2.0f : 1.0f, 0);

    std::string t = label;
    if (upper)
        for (char &c : t)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const float ty = p.y + (h - fs) * 0.5f;
    dl->AddText(ImVec2(p.x + pad, ty), kRowLabel, t.c_str());
    float right = p.x + w - pad;

    if (stepped && !value.empty())
    {
        // ◀ value ▶ drawn as triangles (Nunito has no arrow glyphs); a click on one steps.
        const float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        const float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        const ImU32 ac = hot ? kGoldSoft : IM_COL32(170, 170, 160, 255);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked)
        {
            const float mx = ImGui::GetIO().MousePos.x, hit = gap;
            if (mx >= lx - hit && mx <= lx + aw + hit)
                return -1;
            if (mx >= rx - hit && mx <= rx + aw + hit)
                return 1;
        }
        right = rx - gap;
    }
    if (!value.empty())
    {
        const float vw = ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(right - vw, ty), changed ? IM_COL32(255, 255, 255, 255) : kMuted, value.c_str());
    }
    ImGui::PopID();
    return clicked ? 2 : 0;
}

} // namespace

void don_ui_frame_begin(float width, float height)
{
    g_display_w = width;
    g_display_h = height;
    don_ui_set_scale(height / 720.0f);
    // The fonts are loaded at their 1080 px sizes; FontScaleMain scales them with the window height.
    ImGui::GetStyle().FontScaleMain = (height > 0.0f) ? height / 1080.0f : 1.0f;
}

void don_ui_background()
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(g_display_w, g_display_h), col(kBgTL), col(kBgTR),
                                col(kBgBR), col(kBgBL));
    vignette(dl);
    shadow_banner(dl);
}

void don_ui_screen(const char *title, bool gradient_title, bool compact)
{
    don_ui_background();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(g_display_w, g_display_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##screen", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleColor();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float title_y = don_ui_S(compact ? 60.0f : 120.0f);
    const float cont_y = don_ui_S(compact ? 160.0f : 285.0f);

    // Dummy items (not SetCursorPos) so the window's content boundary grows with the layout.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(x0(), title_y)); // the top margin, and the left x for the title
    ImGui::SetCursorPosX(x0());
    ImGui::PushFont(g_font_title, g_font_title->LegacySize);
    const char *t = title ? title : "";
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (gradient_title)
        don_text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(255, 232, 120, 255),
                    IM_COL32(255, 138, 48, 255), t, don_ui_S(3.0f));
    else
        don_text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p, kGold, kGold, t, don_ui_S(2.0f));
    ImGui::Dummy(ImGui::CalcTextSize(t));
    ImGui::PopFont();

    const float gap = cont_y - ImGui::GetCursorPosY();
    if (gap > 0.0f)
        ImGui::Dummy(ImVec2(x0(), gap)); // the gap to the content
    ImGui::SetCursorPosX(x0());
    ImGui::PopStyleVar();
}

void don_ui_frame_end()
{
    ImGui::End();
}

float don_ui_x0() { return x0(); }
float don_ui_content_y() { return content_y(); }

void don_text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                 float shadow)
{
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f})
            dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom)
        return;
    const float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    const ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
    for (int i = v0; i < dl->VtxBuffer.Size; ++i)
    {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

int don_menu_item(const char *label, bool selected, bool enabled)
{
    ImGui::SetCursorPosX(x0());
    ImGui::PushFont(g_font_menu, g_font_menu->LegacySize);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const ImVec2 sz = ImGui::CalcTextSize(label);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40.0f, sz.y));
    ImGui::PopStyleColor();
    const bool hot = selected || ImGui::IsItemHovered();
    ImGui::PopID();

    // The ‹ › arrow x positions (for click-stepping, launcher.mm's PLAY).
    const char *lt = strstr(label, "\xe2\x80\xb9"), *rt = strstr(label, "\xe2\x80\xba");
    const float lx = lt ? at.x + ImGui::CalcTextSize(label, lt).x : 0.0f;
    const float rx = rt ? at.x + ImGui::CalcTextSize(label, rt).x : 0.0f;
    const float aw = lt ? ImGui::CalcTextSize("\xe2\x80\xb9").x : 0.0f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled)
    {
        // A shadow band behind the item (launcher.mm's menu_item), feathered, with a warm glow.
        const float xw = ImGui::GetWindowPos().x;
        const float x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
        const float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
        const ImU32 c = kBandDark, g = kBandGlow, n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
        for (int pass = 0; pass < 2; ++pass)
        {
            const ImU32 a = pass ? g : c, z = pass ? ng : n;
            dl->AddRectFilledMultiColor(ImVec2(xw, y1), ImVec2(x1, y2), a, a, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(xw, y0), ImVec2(x1, y1), z, z, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(xw, y2), ImVec2(x1, y3), a, a, z, z);
            dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
        }
    }
    const ImU32 c = !enabled ? kTextDim : hot ? kGold : kText;
    const ImU32 cb = hot && enabled ? kGoldHot : c;
    don_text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, c, cb, label, enabled ? sz.y * 0.05f : 0.0f);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, sz.y * 0.16f)); // generous but bounded line spacing
    ImGui::PopStyleVar();

    int result = 0;
    if (clicked && enabled)
    {
        result = 2;
        if (lt && rt)
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float pad = sz.y * 0.35f;
            const bool in_y = m.y >= at.y - pad && m.y <= at.y + sz.y + pad;
            if (in_y && m.x >= lx - pad && m.x <= lx + aw + pad)
                result = -1;
            else if (in_y && m.x >= rx - pad && m.x <= rx + aw + pad)
                result = 1;
        }
    }
    return result;
}

void don_opt_begin(const char *id)
{
    g_desc_sel.clear();
    g_desc_hover.clear();
    g_desc_first.clear();
    g_in_pane = true;
    ImGui::PushID(id);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.48f, avail.y), ImGuiChildFlags_None,
                      ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, don_ui_S(4.0f)));
}

void don_opt_section(const char *title)
{
    std::string t = title;
    for (char &c : t)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1)
        ImGui::Dummy(ImVec2(0, don_ui_S(10.0f)));
    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.52f, 1.0f), "%s", t.c_str());
    ImGui::PopFont();
}

int don_opt_row(const char *label, const std::string &value, bool changed, bool selected, bool stepped,
                const char *desc)
{
    if (!g_in_pane)
        ImGui::SetCursorPosX(x0());
    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    const int r = row(label, value, changed, selected, stepped, desc, true);
    ImGui::PopFont();
    return r;
}

void don_opt_note(const char *desc)
{
    if (desc && desc[0])
        g_desc_first = desc;
}

void don_opt_end()
{
    ImGui::PopStyleVar();
    ImGui::EndChild();
    if (!g_desc_hover.empty())
        g_desc_shown = g_desc_hover;
    else if (!g_desc_sel.empty())
        g_desc_shown = g_desc_sel;
    else if (g_desc_shown.empty())
        g_desc_shown = g_desc_first;

    ImGui::SameLine(0, 0);
    const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const float gap = don_ui_S(24.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        IM_COL32(236, 194, 84, 110), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(don_ui_S(14.0f), don_ui_S(6.0f)));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopID();
    g_in_pane = false;
}

int don_ui_dropdown(const char *id, const char *const *options, int n, int current)
{
    int picked = -1;
    if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove))
    {
        ImGui::PushFont(g_font_small, g_font_small->LegacySize);
        for (int i = 0; i < n; ++i)
            if (ImGui::Selectable(options[i], i == current))
                picked = i;
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    return picked;
}

bool don_action_row(const char *label, const std::string &value, bool selected, bool upper)
{
    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    std::string t = label;
    if (upper)
        for (char &c : t)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const int r = row(t.c_str(), value, false, selected, false, nullptr, false);
    ImGui::PopFont();
    return r == 2;
}

void don_player_row(const char *name, bool local, int ping_ms)
{
    ImGui::SetCursorPosX(x0());
    const float w = content_width();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float h = don_ui_S(36.0f);
    const float r = 8.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 12), r);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 38), r);

    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    const ImU32 dot = local ? kGoldSoft : kGreen;
    dl->AddCircleFilled(ImVec2(pos.x + don_ui_S(20.0f), pos.y + h * 0.5f), don_ui_S(5.0f), dot, 20);

    std::string n = name ? name : "";
    if (local)
        n += " (you)";
    const float fs = ImGui::GetFontSize();
    dl->AddText(ImVec2(pos.x + don_ui_S(36.0f), pos.y + (h - fs) * 0.5f), kRowLabel, n.c_str());

    std::string ping = (ping_ms >= 0) ? (std::to_string(ping_ms) + " ms") : std::string("—");
    const ImVec2 ps = ImGui::CalcTextSize(ping.c_str());
    dl->AddText(ImVec2(pos.x + w - don_ui_S(16.0f) - ps.x, pos.y + (h - fs) * 0.5f), kMuted, ping.c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0.0f, h + don_ui_S(6.0f)));
}

bool don_copy_card(const std::string &text)
{
    ImGui::SetCursorPosX(x0());
    const float w = content_width();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float pad = don_ui_S(16.0f), btn = don_ui_S(36.0f);
    const float text_w = w - 2.0f * pad - btn - don_ui_S(12.0f);

    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    const float fs = ImGui::GetFontSize();
    const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, text_w);
    const float h = std::max(size.y + 2.0f * pad, btn + 2.0f * pad);

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 10), 9.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 38), 9.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x + pad, pos.y + pad), kRowLabel, text.c_str(),
                nullptr, text_w);

    const ImVec2 bpos(pos.x + w - pad - btn, pos.y + pad);
    dl->AddRectFilled(bpos, ImVec2(bpos.x + btn, bpos.y + btn), IM_COL32(255, 255, 255, 16), 8.0f);
    const char *copy = "COPY";
    const ImVec2 cs = ImGui::CalcTextSize(copy);
    dl->AddText(ImVec2(bpos.x + (btn - cs.x) * 0.5f, bpos.y + (btn - fs) * 0.5f), kRowLabel, copy);
    ImGui::PopFont();

    ImGui::InvisibleButton("##copycard", ImVec2(w, h));
    const bool clicked = ImGui::IsItemClicked();
    ImGui::Dummy(ImVec2(0.0f, h + don_ui_S(8.0f)));
    return clicked;
}

void don_caption(const char *text, ImU32 color)
{
    ImGui::SetCursorPosX(x0());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_font_small, g_font_small->LegacySize);
    const float w = content_width();
    const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, w);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x, pos.y), color, text, nullptr, w);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, size.y + don_ui_S(4.0f)));
}

void don_spinner(float radius, float thickness)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 c(pos.x + radius, pos.y + radius);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float t = static_cast<float>(ImGui::GetTime());
    const float a0 = t * 2.4f;
    const float a1 = a0 + 3.14159265f * 1.4f;
    dl->PathClear();
    dl->PathArcTo(c, radius, a0, a1, 24);
    dl->PathStroke(kGoldSoft, thickness);
    ImGui::Dummy(ImVec2(0.0f, radius * 2.0f + don_ui_S(8.0f)));
}

void don_status_dot(ImU32 color, float radius)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(ImVec2(pos.x + radius, pos.y + radius), radius, color, 20);
    ImGui::Dummy(ImVec2(radius * 2.0f + don_ui_S(6.0f), radius * 2.0f));
}

void don_ui_fade_overlay(float alpha)
{
    if (alpha <= 0.0f)
        return;
    if (alpha > 1.0f)
        alpha = 1.0f;
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(g_display_w, g_display_h),
                                                  IM_COL32(0x12, 0x22, 0x28, static_cast<int>(alpha * 255.0f)));
}

} // namespace don_host::ui
