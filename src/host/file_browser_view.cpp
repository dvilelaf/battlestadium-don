// SPDX-License-Identifier: GPL-3.0-or-later
// See file_browser_view.h. Drawing reuses setup_screen.h's text helpers and the menu's gold band.
#include "file_browser_view.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "setup_screen.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace don_host
{
namespace
{
float S(float v) { return ui::don_ui_S(v); }

constexpr float kRowFactor = 1.7f;       // row height as a multiple of the small font size
constexpr float kVerdictGap = 24.0f;     // least space between a name and its verdict (unscaled)
constexpr const char *kPlaces = "Places";  // BrowserView::title of the start list (file_browser.h)

struct State
{
    BrowserRouter router;
};

State &st()
{
    static State s;
    return s;
}

void on_dialog(void *userdata, const char *const *files, int)
{
    DialogResult r;
    r.seq = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(userdata));
    if (!files)
        r.failed = true;  // SDL reports an error as a null list
    else if (*files)
        r.path = *files;
    st().router.deliver_dialog(r);
}

// The end of a long path, with "..." in place of the start, so the folder you are in stays visible.
std::string tail_to_width(std::string s, float width)
{
    if (text_width(s) <= width)
        return s;
    while (s.size() > 4 && text_width("..." + s) > width)
        s.erase(0, 1);
    return "..." + s;
}

std::string head_to_width(std::string s, float width)
{
    if (text_width(s) <= width)
        return s;
    while (s.size() > 4 && text_width(s + "...") > width)
    {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)  // never cut inside a UTF-8 sequence
            s.pop_back();
    }
    return s + "...";
}
} // namespace

BrowserRouter &file_browser_router() { return st().router; }

bool file_browser_active() { return st().router.browser_active(); }

void file_browser_set_start_places(std::vector<std::filesystem::path> places)
{
    st().router.set_start_places(std::move(places));
}

void file_browser_home(std::string &home, std::string &user)
{
    const char *h = SDL_GetUserFolder(SDL_FOLDER_HOME);
    const char *u = std::getenv("USER");
    if (!u)
        u = std::getenv("USERNAME");
    home = h ? h : "";
    user = u ? u : "";
}

void file_browser_open()
{
    std::string home, user;
    file_browser_home(home, user);
    st().router.open_browser(home, user);
}

void file_browser_close() { st().router.close_browser(); }

std::optional<std::string> file_browser_navigate(don_setup::Nav nav) { return st().router.navigate(nav); }

void file_browser_draw(ImVec2 size, don_setup::SetupModel &model)
{
    BrowserRouter &router = st().router;
    FileBrowserModel *b = router.browser();
    if (!b)
        return;
    ui::don_ui_screen("CHOOSE YOUR DISC IMAGE", true, true);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float x0 = ui::don_ui_x0(), width = size.x - 2 * x0;
    float y = ImGui::GetCursorScreenPos().y;

    // Bottom: the keys, in words (Nunito has no arrows).
    const std::string hint = "UP / DOWN: choose   ENTER: open or pick   BACKSPACE or gamepad B: back   ESC: close";
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    const float fs = ImGui::GetFontSize(), hint_h = ImGui::CalcTextSize(hint.c_str(), nullptr, false, width).y + S(4.0f);
    ImGui::PopFont();
    const float hint_y = size.y - S(36.0f) - hint_h;
    text_at(ImVec2(x0, hint_y), ui::kMuted, hint, width);

    const float rh = fs * kRowFactor;
    const std::size_t rows_fit = static_cast<std::size_t>(std::max(1.0f, (hint_y - y - S(48.0f)) / rh));
    b->set_visible_rows(rows_fit);
    const BrowserView v = b->view();

    y += text_at(ImVec2(x0, y), ui::kMuted, tail_to_width(v.title, width), width);
    y += S(10.0f);
    if (!v.empty_reason.empty())
        text_at(ImVec2(x0, y), ui::kMuted, v.empty_reason, width);

    const float gap = S(kVerdictGap);
    ImGuiIO &io = ImGui::GetIO();
    const float left = ImGui::GetWindowPos().x;
    int hovered = -1;
    int row_index = -1;
    for (const BrowserRow &r : v.rows)
    {
        ++row_index;
        if (io.MousePos.x >= left && io.MousePos.x < x0 + width && io.MousePos.y >= y - S(2.0f) &&
            io.MousePos.y < y + rh - S(4.0f))
            hovered = row_index;
        const ImU32 col = r.selected ? ui::kGold : (r.enterable ? ui::kText : ui::kMutedDim);
        if (r.selected)
        {
            // The same band the menu puts behind its focused item.
            const float xw = ImGui::GetWindowPos().x;
            dl->AddRectFilled(ImVec2(xw, y - S(2.0f)), ImVec2(x0 + width, y + rh - S(4.0f)), ui::kBandDark);
            dl->AddRectFilled(ImVec2(xw, y - S(2.0f)), ImVec2(x0 + width, y + rh - S(4.0f)), ui::kBandGlow);
        }
        const float verdict_w = r.verdict.empty() ? 0.0f : text_width(r.verdict);
        // A folder gets a trailing "/"; the start places are full paths already and stay as they are.
        const bool at_places = v.title == kPlaces;
        const std::string name = head_to_width(r.is_folder && !at_places ? r.label + "/" : r.label,
                                               width - verdict_w - (verdict_w > 0.0f ? gap : 0.0f));
        text_at(ImVec2(x0, y), col, name, width);
        if (verdict_w > 0.0f)
            text_at(ImVec2(x0 + width - verdict_w, y), r.selected ? ui::kText : ui::kMuted, r.verdict, width);
        y += rh;
    }
    ui::don_ui_frame_end();

    // Mouse, last: a click can close the browser. Hover focuses a row only when the mouse moved, so it does not undo
    // the keys; the wheel moves the focus one row per notch.
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (hovered >= 0 && (moved || click))
        route_browser_mouse(model, router, hovered, click);
    for (int notch = static_cast<int>(std::fabs(io.MouseWheel)); notch > 0 && router.browser_active(); --notch)
        route_browser_nav(model, router, io.MouseWheel > 0.0f ? don_setup::Nav::Up : don_setup::Nav::Down, false);
}

void file_dialog_open(std::uint64_t seq)
{
    // The system chooser, in the player's home, listing disc images first; "All files" stays because a disc image can
    // carry any name and the classifier decides from the content. The in-app browser opens only when this reports
    // an error (route_dialog_result).
    // Static: the dialog may read the filters after this function returns.
    static const std::vector<DialogFilter> filters = disc_dialog_filters();
    static const std::vector<SDL_DialogFileFilter> sdl_filters = [] {
        std::vector<SDL_DialogFileFilter> v;
        for (const DialogFilter &f : filters)
            v.push_back({f.name.c_str(), f.pattern.c_str()});
        return v;
    }();
    std::string home, user;
    file_browser_home(home, user);
    SDL_ShowOpenFileDialog(on_dialog, reinterpret_cast<void *>(static_cast<std::uintptr_t>(seq)), nullptr,
                           sdl_filters.data(), static_cast<int>(sdl_filters.size()), home.empty() ? nullptr : home.c_str(),
                           false);
}

std::optional<DialogResult> file_dialog_poll() { return st().router.take_dialog_result(); }

} // namespace don_host
