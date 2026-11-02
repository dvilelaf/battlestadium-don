// SPDX-License-Identifier: GPL-3.0-or-later
// Drawing of the first-run setup screen: see setup_screen.h. Only the view's strings are drawn.
#include "setup_screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <optional>

#include "file_browser_view.h"
#include "imgui.h"
#include "setup_chime.h"
#include "setup_title_wrap.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace don_host
{
namespace
{
using don_setup::Mark;
using don_setup::Row;
using don_setup::Screen;
using don_setup::View;

float S(float v) { return ui::don_ui_S(v); }


} // namespace

void setup_screen_draw(don_setup::SetupModel &model, const View &v)
{
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    // The system dialog's answer: a path goes to the model, "" is a cancel, a failure opens the in-app browser (E02).
    // An answer to an older request (a drop or the browser came first) is ignored by the router.
    if (const auto answer = file_dialog_poll())
        route_dialog_result(model, file_browser_router(), *answer);
    if (file_browser_active())
    {
        file_browser_draw(size, model);
        return;
    }
    if (v.error_overlay)
    {
        // The error card covers the screen: only the background is drawn behind it, never the screen's text.
        ui::don_ui_screen("", true, true);
        ui::don_ui_frame_end();
        draw_error_card(model, v, size);
        return;
    }
    // A title that starts with "✓" is drawn with spaces in its place and the tick drawn over them.
    std::string title = v.title;
    const float title_size = ui::g_font_title->LegacySize * ImGui::GetStyle().FontScaleMain;
    const float x0 = ui::don_ui_x0();
    float tick_size = 0.0f;
    float tick_room = 0.0f; // the width the tick takes at the start of the first line
    std::size_t tick_at = std::string::npos;
    if (const std::size_t t = title.find(kTick); t != std::string::npos)
    {
        title.erase(t, std::char_traits<char>::length(kTick));
        if (t < title.size() && title[t] == ' ')
            title.erase(t, 1);
        tick_size = title_size;
        tick_room = tick_width(tick_size) + tick_size * 0.2f;
        tick_at = t;
    }
    // A title is never cut by the window edge: when it is wider than the space between the margins it wraps at a space.
    const auto text_width = [&](const std::string &s) {
        return ui::g_font_title->CalcTextSizeA(title_size, 100000.0f, 0.0f, s.c_str()).x;
    };
    const std::vector<std::string> lines =
        don_setup::wrap_title(title, size.x - 2.0f * x0, tick_at == 0 ? tick_room : 0.0f, text_width);
    title.clear();
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        std::string line = lines[i];
        if (tick_at != std::string::npos && i == 0)
        {
            const float space = text_width(" ");
            line.insert(std::min(tick_at, line.size()), static_cast<std::size_t>(std::ceil(tick_room / space)), ' ');
        }
        title += (i ? "\n" : "") + line;
    }
    ui::don_ui_screen(title.c_str(), true, true);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (tick_size > 0.0f)
        draw_tick(dl, ImVec2(x0, S(60.0f)), tick_size, ui::kGreen);
    if (lines.size() > 1)
        ImGui::Dummy(ImVec2(0.0f, S(16.0f))); // a second title line pushes the screen down; keep a gap under it
    float y = ImGui::GetCursorScreenPos().y;
    const bool has_panel = !v.panel.empty();
    const float panel_w = has_panel ? S(360.0f) : 0.0f;
    const float left_w = size.x - 2 * x0 - panel_w - (has_panel ? S(40.0f) : 0.0f);

    if (!v.steps.empty())
        y += draw_rail(ImVec2(x0, y), v.steps);
    const float top = y;

    if (!v.banner.empty())
        y += text_at(ImVec2(x0, y), ui::kGreen, v.banner, left_w);
    for (const std::string &b : v.body)
        y += text_at(ImVec2(x0, y), ui::kText, b, left_w);
    if (v.bar >= 0.0)
    {
        draw_bar(ImVec2(x0, y + S(4.0f)), left_w, v.bar);
        y += S(24.0f);
    }
    if (v.percent >= 0 || !v.time_text.empty())
    {
        std::string line = v.percent >= 0 ? std::to_string(v.percent) + " %" : "";
        if (!v.time_text.empty())
            line += (line.empty() ? "" : "  ·  ") + v.time_text;
        y += text_at(ImVec2(x0, y), ui::kText, line, left_w);
    }
    if (!v.status.empty())
        y += text_at(ImVec2(x0, y), ui::kMuted, v.status, left_w);
    if (v.screen == Screen::Decision && !v.error.code.empty())
        y += text_at(ImVec2(x0, y), ui::kMutedDim, v.error.code, left_w);
    if (!v.link.empty())
    {
        y += text_at(ImVec2(x0, y), ui::kText, v.link, left_w);
    }

    if (has_panel)
    {
        const ImVec2 pp(size.x - x0 - panel_w, top);
        float ph = 0.0f;
        ph += text_at(pp, ui::kText, v.panel_title, panel_w, true);
        ph += draw_rows(ImVec2(pp.x, pp.y + ph), panel_w, v.panel);
        (void)ph;
    }

    // Notes and hint sit at the bottom edge; the menu follows the body.
    float bottom = size.y - S(36.0f);
    for (auto it = v.notes.rbegin(); it != v.notes.rend(); ++it)
    {
        ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
        const float h = ImGui::CalcTextSize(it->c_str(), nullptr, false, left_w + panel_w).y + S(4.0f);
        ImGui::PopFont();
        bottom -= h;
        text_at(ImVec2(x0, bottom), ui::kMutedDim, *it, left_w + panel_w);
    }
    if (!v.hint.empty())
    {
        ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
        const float h = ImGui::CalcTextSize(v.hint.c_str(), nullptr, false, left_w + panel_w).y + S(4.0f);
        ImGui::PopFont();
        bottom -= h;
        text_at(ImVec2(x0, bottom), ui::kMuted, v.hint, left_w + panel_w);
    }

    ImGui::SetCursorScreenPos(ImVec2(x0, y + S(10.0f)));
    draw_menu(model, v, left_w);
    ui::don_ui_frame_end();
}

void setup_screen_effects(SDL_Window *window, const View &v)
{
    static SetupEffects effects;
    static const bool trace = std::getenv("DON_TRACE_SETUP") != nullptr;
    const EffectsDecision d = effects.update(v);
    if (d.set_title)
    {
        if (trace)
            std::printf("[don] window title: %s\n", d.title.c_str());
        if (window)
            SDL_SetWindowTitle(window, d.title.c_str());
    }
    if (d.finished)
    {
        if (trace)
            std::printf("[don] flash\n");
        if (window)
            SDL_FlashWindow(window, SDL_FLASH_UNTIL_FOCUSED);
        chime_play();
    }
    chime_poll();
}

void setup_screen_end()
{
    chime_shutdown();
}

bool setup_screen_handle_action(don_setup::SetupModel &model, const don_setup::ActionRequest &request)
{
    (void)model;
    std::string home, user;
    file_browser_home(home, user);
    return route_action(file_browser_router(), request, home, user,
                        [](std::uint64_t seq) { file_dialog_open(seq); });
}

namespace
{
// The key or button as a model direction; nothing for the keys the setup screens do not use.
std::optional<don_setup::Nav> nav_of(const SDL_Event &e)
{
    using don_setup::Nav;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        switch (e.gbutton.button)
        {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return Nav::Up;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Nav::Down;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Nav::Left;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Nav::Right;
        case SDL_GAMEPAD_BUTTON_SOUTH: return Nav::Confirm;
        case SDL_GAMEPAD_BUTTON_EAST: return Nav::Back;
        default: return std::nullopt;
        }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
        switch (e.key.key)
        {
        case SDLK_UP: return Nav::Up;
        case SDLK_DOWN: return Nav::Down;
        case SDLK_LEFT: return Nav::Left;
        case SDLK_RIGHT: return Nav::Right;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: return Nav::Confirm;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE: return Nav::Back;
        default: return std::nullopt;
        }
    return std::nullopt;
}
} // namespace

void setup_screen_event(don_setup::SetupModel &model, const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: model.request_close(); break;
    case SDL_EVENT_DROP_FILE:
        route_drop(model, file_browser_router(), e.drop.data ? e.drop.data : "");
        break;
    default:
        if (const auto nav = nav_of(e))
        {
            const bool escape = e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE;
            if (!route_browser_nav(model, file_browser_router(), *nav, escape))
                model.navigate(*nav);
        }
        break;
    }
}

} // namespace don_host
