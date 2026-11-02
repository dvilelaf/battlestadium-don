// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen's view (T-517, T-523, docs/knowledge/22-netplay.md sections 7.2 and 7.3): an SDL3 window
// drawn with Dear ImGui over the state machine of start_screen_model.h, in the owner's LAS Recomp style
// (text-only menu items lower-left with a gold highlight band, two-pane settings rows with descriptions and
// Back, a calm dark background, one rounded font). Keyboard, mouse and gamepad are turned into the model's
// keys; the network sits behind StartScreenNetLayer below. The look reuses the owner's code
// (LAS Recomp: runtime/ui/launcher.mm, runtime/ui/option_rows.mm), adapted to
// SDL_Renderer and to this model (the model owns the selection).
#include "start_screen.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include "net/game_hash.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "net/upnp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace don_host
{
namespace
{

const char *screen_name(Screen s)
{
    switch (s)
    {
    case Screen::Menu: return "menu";
    case Screen::LocalSetup: return "play";
    case Screen::NameEdit: return "name";
    case Screen::HostSetup: return "host-setup";
    case Screen::HostLobby: return "host-lobby";
    case Screen::JoinSetup: return "join-setup";
    case Screen::Connecting: return "connecting";
    case Screen::ClientLobby: return "client-lobby";
    case Screen::Settings: return "settings";
    case Screen::Error: return "error";
    case Screen::Done: return "done";
    }
    return "screen";
}

Key key_from_scancode(SDL_Keycode key, bool &handled)
{
    handled = true;
    switch (key)
    {
    case SDLK_UP: return Key::Up;
    case SDLK_DOWN: return Key::Down;
    case SDLK_LEFT: return Key::Left;
    case SDLK_RIGHT: return Key::Right;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        return Key::Confirm;
    case SDLK_ESCAPE: return Key::Back;
    case SDLK_BACKSPACE: return Key::Backspace;
    default: break;
    }
    handled = false;
    return Key::Confirm;
}

bool button_to_key(Uint8 button, Key &out)
{
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: out = Key::Up; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: out = Key::Down; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: out = Key::Left; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: out = Key::Right; return true;
    case SDL_GAMEPAD_BUTTON_SOUTH: out = Key::Confirm; return true;
    case SDL_GAMEPAD_BUTTON_EAST: out = Key::Back; return true;
    default: return false;
    }
}

std::string uppercase(const std::string &s)
{
    std::string t = s;
    for (char &c : t)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return t;
}

// The index of the highlighted line, or -1.
int selected_line(const StartScreenView &v)
{
    for (std::size_t i = 0; i < v.lines.size(); ++i)
        if (v.lines[i].selected)
            return static_cast<int>(i);
    return -1;
}

// Moves the model's cursor to `target` (by the shortest wrap) and confirms. Used for mouse clicks on the
// menu, which is the only screen whose lines are a list the cursor walks.
void menu_activate(StartScreenModel &model, const StartScreenView &v, int target)
{
    const int cur = selected_line(v);
    const int n = static_cast<int>(v.lines.size());
    if (n <= 1)
    {
        model.key(Key::Confirm);
        return;
    }
    const int down = (target - cur + n) % n;
    const int up = (cur - target + n) % n;
    if (down <= up)
        for (int i = 0; i < down; ++i)
            model.key(Key::Down);
    else
        for (int i = 0; i < up; ++i)
            model.key(Key::Up);
    model.key(Key::Confirm);
}

// Moves the Settings cursor to `target` (by the shortest wrap), so a click can then change that control.
void settings_select(StartScreenModel &model, const StartScreenView &v, int target)
{
    constexpr int n = 6; // name, fullscreen, renderer, vibration, net delay, host port
    const int cur = v.settings_item;
    const int down = (target - cur + n) % n;
    const int up = (cur - target + n) % n;
    if (down <= up)
        for (int i = 0; i < down; ++i)
            model.key(Key::Down);
    else
        for (int i = 0; i < up; ++i)
            model.key(Key::Up);
}

// Sets the net delay (item 4: Auto = 0, then 1..10) to `target` by the shortest wrap of Left/Right.
void settings_set_delay(StartScreenModel &model, int target)
{
    const int cur = model.net_delay();
    const int diff = (target - cur + 11) % 11;
    const Key step = (diff <= 5) ? Key::Right : Key::Left;
    const int times = (diff <= 5) ? diff : 11 - diff;
    for (int i = 0; i < times; ++i)
        model.key(step);
}

// "Players can join at 203.0.113.9:7877 (or your domain)." -> "203.0.113.9:7877".
std::string extract_join_address(const std::string &line)
{
    const std::size_t at = line.rfind("at ");
    std::size_t start = (at == std::string::npos) ? 0 : at + 3;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
    std::size_t end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != ';' && line[end] != '(' && line[end] != ')' &&
           line[end] != ',' && line[end] != '\n' && line[end] != '.')
        ++end;
    if (end > start && line[end - 1] == '.')
        --end;
    return line.substr(start, end - start);
}

// Opens the full-screen transparent window and dims it (the Settings / Error panels sit on it).
void open_dimmed_screen()
{
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##dim", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 110));
}

// The drawn X in the corner of a panel (launcher.mm's close button); true when clicked / B / Esc.
bool close_button()
{
    const float sc = ImGui::GetIO().DisplaySize.y / 1080.0f;
    const float bx = 44.0f * sc;
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool back = ImGui::InvisibleButton("##close", ImVec2(bx, bx));
    ImGui::PopStyleColor();
    const bool hot = ImGui::IsItemHovered();
    const ImU32 c = hot ? ui::kGoldSoft : IM_COL32(220, 216, 205, 255);
    const float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot)
        dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8.0f * sc);
    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), c, t);
    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), c, t);
    return back;
}

// ---- the screens ------------------------------------------------------------------

void render_menu(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen("Battle Stadium D.O.N", true);

    for (std::size_t i = 0; i < v.lines.size(); ++i)
    {
        const ViewLine &line = v.lines[i];
        const char *label;
        switch (i)
        {
        case 0: label = "PLAY"; break;
        case 1: label = "HOST ONLINE"; break;
        case 2: label = "JOIN ONLINE"; break;
        case 3: label = "SETTINGS"; break;
        default: label = "QUIT"; break;
        }
        const int r = ui::don_menu_item(label, line.selected, !line.dim);
        if (r == 2)
            menu_activate(model, v, static_cast<int>(i));
        else if (r == -1)
            model.key(Key::Left);
        else if (r == 1)
            model.key(Key::Right);
    }

    // Footer (lower left): the status line, the player's name and the build id. Lower right: "Online".
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float dh = ImGui::GetIO().DisplaySize.y, x0 = ui::don_ui_x0();
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    const float lh = ImGui::GetTextLineHeight();
    float y = dh - ui::don_ui_S(118.0f);
    // A drawn check mark (Nunito has no ✓) + "Game files ready.".
    dl->AddLine(ImVec2(x0 + lh * 0.1f, y + lh * 0.55f), ImVec2(x0 + lh * 0.35f, y + lh * 0.8f), ui::kGreen, lh * 0.12f);
    dl->AddLine(ImVec2(x0 + lh * 0.35f, y + lh * 0.8f), ImVec2(x0 + lh * 0.85f, y + lh * 0.2f), ui::kGreen, lh * 0.12f);
    dl->AddText(ImVec2(x0 + lh * 1.3f, y), ui::kGreen, "Game files ready.");
    y += lh + ui::don_ui_S(6.0f);
    // "Player: <name>" then the build id (from the model's footnote).
    std::string first = v.footnote, rest;
    const std::size_t nl = v.footnote.find('\n');
    if (nl != std::string::npos)
    {
        first = v.footnote.substr(0, nl);
        rest = v.footnote.substr(nl + 1);
    }
    dl->AddText(ImVec2(x0, y), ui::kMutedDim, first.c_str());
    if (!rest.empty())
    {
        y += lh + ui::don_ui_S(2.0f);
        dl->AddText(ImVec2(x0, y), ui::kMutedDim, rest.c_str());
    }
    // Bottom-right: "Online: ready".
    const float right = ImGui::GetWindowPos().x + ImGui::GetIO().DisplaySize.x - ui::don_ui_S(60.0f);
    const ImVec2 os = ImGui::CalcTextSize("Online: ready");
    dl->AddText(ImVec2(right - os.x, dh - ui::don_ui_S(60.0f)), ui::kMutedDim, "Online: ready");
    ImGui::PopFont();

    ui::don_ui_frame_end();
}

// A simple labelled text box (the name / address field); `caret` draws a blinking cursor.
void field_box(const char *label, const std::string &text, bool caret)
{
    ImGui::SetCursorPosX(ui::don_ui_x0());
    if (label && label[0])
    {
        ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetCursorScreenPos(), ui::kMuted, label);
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() + ui::don_ui_S(6.0f)));
        ImGui::PopFont();
    }
    ImGui::SetCursorPosX(ui::don_ui_x0());
    const float w = ImGui::GetIO().DisplaySize.x - 2.0f * ui::don_ui_x0();
    const float h = ui::don_ui_S(52.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 10), 8.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), ui::kGoldSoft, 8.0f, 2.0f, 0);
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    const float fs = ImGui::GetFontSize();
    const std::string shown = text.empty() ? " " : text;
    dl->AddText(ImVec2(pos.x + ui::don_ui_S(16.0f), pos.y + (h - fs) * 0.5f), ui::kRowLabel, shown.c_str());
    if (caret)
    {
        const float tw = ImGui::CalcTextSize(shown.c_str()).x;
        dl->AddRectFilled(ImVec2(pos.x + ui::don_ui_S(16.0f) + tw + 2.0f, pos.y + (h - fs) * 0.5f + 2.0f),
                          ImVec2(pos.x + ui::don_ui_S(16.0f) + tw + 4.0f, pos.y + (h + fs) * 0.5f - 2.0f),
                          ui::kGold);
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, h + ui::don_ui_S(12.0f)));
}

void render_local_setup(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen("Play", false, true);
    const std::string count = std::to_string(v.local_players) + (v.local_players == 1 ? " player" : " players");
    const int r = ui::don_opt_row("Players", count, v.local_players != 1, true, true,
                                  "How many play on this PC. 3 or 4 use a multitap.");
    if (r == -1)
        model.key(Key::Left);
    else if (r == 1)
        model.key(Key::Right);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("START", false))
        model.key(Key::Confirm);
    if (ui::don_menu_item("BACK", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_host_setup(StartScreenModel &model, const StartScreenView &v)
{
    (void)v;
    ui::don_ui_screen("Host online", false, true);
    const std::string count = std::to_string(model.last_players()) + " players";
    const int r = ui::don_opt_row("Players", count, model.last_players() != 2, true, true,
                                  "How many players will join, including you.");
    if (r == -1)
        model.key(Key::Left);
    else if (r == 1)
        model.key(Key::Right);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("START HOSTING", false))
        model.key(Key::Confirm);
    if (ui::don_menu_item("BACK", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_host_lobby(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen("Host online", false, true);

    std::string first = v.footnote, rest;
    const std::size_t nl = v.footnote.find('\n');
    if (nl != std::string::npos)
    {
        first = v.footnote.substr(0, nl);
        rest = v.footnote.substr(nl + 1);
    }
    if (!first.empty())
    {
        if (first.find("join") != std::string::npos)
        {
            if (ui::don_copy_card(first))
                SDL_SetClipboardText(extract_join_address(first).c_str());
        }
        else if (first.find("did not open") != std::string::npos || first.find("forward") != std::string::npos ||
                 first.find("router") != std::string::npos)
            ui::don_caption(first.c_str(), ui::kWarn);
        else
            ui::don_caption(first.c_str(), ui::kMuted);
    }
    if (!rest.empty())
        ui::don_caption(rest.c_str(), ui::kWarn);

    for (const LobbyPlayer &p : v.players)
        ui::don_player_row(p.name.c_str(), p.local, p.ping_ms);

    if (!v.note.empty())
    {
        ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(8.0f)));
        ui::don_caption(v.note.c_str(), ui::kMuted);
    }

    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("START GAME", v.can_start, v.can_start))
        model.key(Key::Confirm);
    if (ui::don_menu_item("CANCEL", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_join_setup(StartScreenModel &model, const StartScreenView &v)
{
    (void)v;
    ui::don_ui_screen("Join online", false, true);
    field_box("HOST ADDRESS (IP OR DOMAIN)", model.field(), false);
    ui::don_caption("An IP address or a domain name, with an optional :port.", ui::kMuted);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("CONNECT", false))
        model.key(Key::Confirm);
    if (ui::don_menu_item("BACK", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_connecting(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen("Join online", false, true);
    ui::don_spinner(ui::don_ui_S(16.0f), ui::don_ui_S(3.0f));
    ui::don_caption(("Connecting to " + model.address() + "...").c_str(), ui::kRowLabel);
    if (!v.note.empty())
        ui::don_caption(v.note.c_str(), ui::kMuted);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("CANCEL", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_client_lobby(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen("Join online", false, true);
    for (const LobbyPlayer &p : v.players)
        ui::don_player_row(p.name.c_str(), p.local, p.ping_ms);
    if (!v.note.empty())
        ui::don_caption(v.note.c_str(), ui::kRowLabel);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("LEAVE", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

void render_error(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_background();
    open_dimmed_screen();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float sc = ds.y / 1080.0f;
    const float w = std::min(ds.x * 0.6f, 820.0f * sc);
    const ImVec2 wp = ImGui::GetWindowPos();
    const char *msg = v.note.empty() ? "Something went wrong." : v.note.c_str();

    // Size the panel to its wrapped message.
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    const ImVec2 msize = ImGui::CalcTextSize(msg, nullptr, false, w - 2.0f * 28.0f * sc);
    ImGui::PopFont();
    const float h = (52.0f + 14.0f + 14.0f + 44.0f + 20.0f + 20.0f) * sc + msize.y;

    ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.045f, 0.05f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * sc, 20.0f * sc));
    ImGui::BeginChild("##error", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::PushFont(ui::g_font_menu, ui::g_font_menu->LegacySize);
    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.4f, 1.0f), "Could not continue");
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushFont(ui::g_font_small, ui::g_font_small->LegacySize);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(msg);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 14.0f * sc));
    const float bw = 180.0f * sc;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
    if (ImGui::Button("OK", ImVec2(bw, 44.0f * sc)))
        model.key(Key::Confirm);

    ImGui::EndChild();
    ImGui::End(); // the dimmed screen
}

void render_settings(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_background();
    open_dimmed_screen();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float sc = ds.y / 1080.0f;
    const float w = std::min(ds.x * 0.9f, 1500.0f * sc);
    const float h = ds.y - 2.0f * 18.0f * sc;
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f * sc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * sc, 24.0f * sc));
    ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::PushFont(ui::g_font_menu, ui::g_font_menu->LegacySize);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "Settings");
    ImGui::PopFont();
    bool back = close_button();

    static const char *const kRendererNames[] = {"Vulkan", "Software"};
    const char *const kDelayNames[] = {"Auto", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};

    ui::don_opt_begin("##settings_rows");

    // NAME (text field)
    if (v.settings_name_editing)
    {
        const std::string val = std::string(model.field()) + "_";
        ui::don_opt_row("Name", val, false, true, false, "Type a name (Enter: done, Esc: cancel). Empty uses your PC user name.");
        ui::don_opt_note("Type a name (Enter: done, Esc: cancel). Empty uses your PC user name.");
    }
    else
    {
        const std::string val = v.lines.empty() ? std::string() : v.lines[0].text.substr(6); // after "Name: "
        const int r = ui::don_opt_row("Name", val, false, v.settings_item == 0, false,
                                      "The name other players see when you host or join.");
        if (r == 2)
        {
            settings_select(model, v, 0);
            model.key(Key::Confirm);
        }
    }

    // FULLSCREEN (toggle)
    {
        const int r = ui::don_opt_row("Fullscreen", v.settings_fullscreen ? "On" : "Off", v.settings_fullscreen,
                                      v.settings_item == 1, false, "Open the game window fullscreen.");
        if (r != 0)
        {
            settings_select(model, v, 1);
            model.key(Key::Right);
        }
    }

    // RENDERER (dropdown)
    {
        const int r = ui::don_opt_row("Renderer", kRendererNames[v.settings_renderer], v.settings_renderer != 0,
                                      v.settings_item == 2, true, "Vulkan is faster; Software is the fallback.");
        if (r == 2)
            ImGui::OpenPopup("##renderer_dd");
        else if (r != 0)
        {
            settings_select(model, v, 2);
            model.key(r < 0 ? Key::Left : Key::Right);
        }
        const int pick = ui::don_ui_dropdown("##renderer_dd", kRendererNames, 2, v.settings_renderer);
        if (pick >= 0 && pick != v.settings_renderer)
        {
            settings_select(model, v, 2);
            model.key(Key::Right);
        }
    }

    // VIBRATION (toggle)
    {
        const int r = ui::don_opt_row("Vibration", v.settings_vibration ? "On" : "Off", !v.settings_vibration,
                                      v.settings_item == 3, false, "Rumble on the gamepads.");
        if (r != 0)
        {
            settings_select(model, v, 3);
            model.key(Key::Right);
        }
    }

    // ONLINE INPUT DELAY (dropdown)
    {
        const int r = ui::don_opt_row("Online input delay", kDelayNames[v.settings_net_delay],
                                      v.settings_net_delay != 0, v.settings_item == 4, true,
                                      "Extra frames of delay so every player stays in sync. Auto measures the ping.");
        if (r == 2)
            ImGui::OpenPopup("##delay_dd");
        else if (r != 0)
        {
            settings_select(model, v, 4);
            model.key(r < 0 ? Key::Left : Key::Right);
        }
        const int pick = ui::don_ui_dropdown("##delay_dd", kDelayNames, 11, v.settings_net_delay);
        if (pick >= 0 && pick != v.settings_net_delay)
        {
            settings_select(model, v, 4);
            settings_set_delay(model, pick);
        }
    }

    // HOST PORT (typed number)
    {
        const char *const kPortDesc =
            "The UDP port other players connect to when you host. Forward this port on your router if it does "
            "not open it by itself. Players who join type your address as address:port when it is not 7877.";
        const std::string val = std::to_string(model.port());
        const int r = ui::don_opt_row("Host port", v.settings_port_editing ? std::string(model.field()) + "_" : val,
                                      model.port() != 7877, v.settings_item == 5, false, kPortDesc);
        if (r == 2)
        {
            settings_select(model, v, 5);
            model.key(Key::Confirm); // enter the port field
        }
    }

    // BACK (7.3): a row after the last setting, same style; click goes back (the X stays too).
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(8.0f)));
    if (ui::don_opt_row("Back", "", false, false, false, "Return to the main menu.") == 2)
        back = true;

    ui::don_opt_end();

    ImGui::EndChild();
    ImGui::End(); // the dimmed screen
    if (back)
        model.key(Key::Back);
}

void render_name_edit(StartScreenModel &model, const StartScreenView &v)
{
    (void)v;
    ui::don_ui_screen("Your name", false, true);
    field_box("Name", model.field(), true);
    ui::don_caption("1 to 15 characters. Empty uses your PC user name.", ui::kMuted);
    ImGui::Dummy(ImVec2(0.0f, ui::don_ui_S(20.0f)));
    if (ui::don_menu_item("OK", false))
        model.key(Key::Confirm);
    if (ui::don_menu_item("BACK", false))
        model.key(Key::Back);
    ui::don_ui_frame_end();
}

// A screen the view does not yet know (added later by the model): draw its lines as menu items and its
// note as a caption, so nothing is ever invisible.
void render_generic(StartScreenModel &model, const StartScreenView &v)
{
    ui::don_ui_screen(uppercase(v.title).c_str(), false);
    for (std::size_t i = 0; i < v.lines.size(); ++i)
    {
        const ViewLine &line = v.lines[i];
        if (ui::don_menu_item(uppercase(line.text).c_str(), line.selected, !line.dim))
            menu_activate(model, v, static_cast<int>(i));
    }
    if (!v.note.empty())
        ui::don_caption(v.note.c_str(), ui::kMuted);
    if (!v.footnote.empty())
        ui::don_caption(v.footnote.c_str(), ui::kMuted);
    ui::don_ui_frame_end();
}

void render(StartScreenModel &model, const StartScreenView &v)
{
    switch (v.screen)
    {
    case Screen::Menu: render_menu(model, v); break;
    case Screen::LocalSetup: render_local_setup(model, v); break;
    case Screen::NameEdit: render_name_edit(model, v); break;
    case Screen::HostSetup: render_host_setup(model, v); break;
    case Screen::HostLobby: render_host_lobby(model, v); break;
    case Screen::JoinSetup: render_join_setup(model, v); break;
    case Screen::Connecting: render_connecting(model, v); break;
    case Screen::ClientLobby: render_client_lobby(model, v); break;
    case Screen::Settings: render_settings(model, v); break;
    case Screen::Error: render_error(model, v); break;
    case Screen::Done: break;
    default: render_generic(model, v); break;
    }
}

} // namespace

StartScreenOutcome don_run_start_screen(const StartScreenRequest &request, StartScreenNet *net)
{
    StartScreenOutcome outcome;
    StartScreenModel model(net, request.remembered);

    std::string shots = request.shots_dir;
    if (shots.empty())
    {
        if (const char *dir = std::getenv("DON_START_SCREEN_SHOT"))
            shots = dir;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
    {
        std::fprintf(stderr, "[don] the start screen could not start SDL: %s\n", SDL_GetError());
        return outcome;
    }

    // The first size: 16:9, first 1280x720, or the largest of 1280x720 / 1600x900 / 1920x1080 that fits 75 %
    // of the display (7.3).
    int win_w = 1280, win_h = 720;
    int win_x = SDL_WINDOWPOS_CENTERED, win_y = SDL_WINDOWPOS_CENTERED;
    int ndisplays = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&ndisplays);
    if (displays && ndisplays > 0)
    {
        SDL_Rect bounds;
        if (SDL_GetDisplayUsableBounds(displays[0], &bounds))
        {
            for (int cand_w : {1920, 1600, 1280})
            {
                const int cand_h = cand_w * 9 / 16;
                if (cand_w <= static_cast<int>(bounds.w * 0.75f) && cand_h <= static_cast<int>(bounds.h * 0.75f))
                {
                    win_w = cand_w;
                    win_h = cand_h;
                    break;
                }
            }
            win_x = bounds.x + (bounds.w - win_w) / 2;
            win_y = bounds.y + (bounds.h - win_h) / 2;
        }
        SDL_free(displays);
    }
    // Test aid: DON_START_SCREEN_SIZE=WxH forces the first size (the screenshots use it for an exact size).
    if (const char *size_env = std::getenv("DON_START_SCREEN_SIZE"))
    {
        if (int w2 = 0, h2 = 0; std::sscanf(size_env, "%dx%d", &w2, &h2) == 2 && w2 > 0 && h2 > 0)
        {
            win_w = w2;
            win_h = h2;
        }
    }

    SDL_Window *window = SDL_CreateWindow("Battle Stadium D.O.N", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (window)
        SDL_SetWindowPosition(window, win_x, win_y);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer)
    {
        std::fprintf(stderr, "[don] the start screen could not open a window: %s\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
        return outcome;
    }
    SDL_StartTextInput(window);

    // Dear ImGui.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the game, and no debug look
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual); // the view maps gamepads itself
    ImGui_ImplSDLRenderer3_Init(renderer);
    ui::don_ui_apply_theme();
    if (!ui::don_ui_load_fonts())
        std::fprintf(stderr, "[don] the start screen could not build the Nunito fonts\n");

    // Every gamepad already there; the view closes its handles before returning (input::SdlInput opens the
    // same devices again for the game).
    std::vector<SDL_Gamepad *> opened_pads;
    int pads = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&pads))
    {
        for (int i = 0; i < pads; ++i)
            if (SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]))
                opened_pads.push_back(pad);
        SDL_free(ids);
    }

    // The 150 ms fade between screens.
    const bool fade = shots.empty();
    double last_switch = ImGui::GetTime();
    Screen last_screen = Screen::Done; // Done is never drawn: the first screen always fades in
    std::string last_shot_key;
    int shot_index = 0;

    bool running = true;
    Uint64 last = SDL_GetTicks();
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                model.key(Key::Quit);
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                model.key(Key::Quit);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad *pad = SDL_OpenGamepad(event.gdevice.which))
                    opened_pads.push_back(pad);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            {
                Key k;
                if (button_to_key(event.gbutton.button, k))
                    model.key(k);
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            {
                // The left stick also works as the d-pad (7.3: confirm / back / d-pad / stick).
                const float v = event.gaxis.value / 32767.0f;
                static bool held[4] = {false, false, false, false}; // left, right, up, down
                if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
                {
                    const bool x = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
                    const int neg = x ? 0 : 2, pos = neg + 1;
                    const Key keys[4] = {Key::Left, Key::Right, Key::Up, Key::Down};
                    for (int i : {neg, pos})
                    {
                        const float m = (i == neg) ? -v : v;
                        const bool now = held[i] ? m > 0.3f : m > 0.5f;
                        if (now != held[i])
                        {
                            held[i] = now;
                            if (now)
                                model.key(keys[i]);
                        }
                    }
                }
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            {
                if (event.key.repeat)
                    break;
                // F11 and Alt+Enter toggle fullscreen.
                if (event.key.key == SDLK_F11 ||
                    (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
                {
                    const bool full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
                    SDL_SetWindowFullscreen(window, !full);
                    break;
                }
                bool handled = false;
                const Key k = key_from_scancode(event.key.key, handled);
                if (handled)
                    model.key(k);
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
                model.text(event.text.text);
                break;
            default:
                break;
            }
        }

        const Uint64 now = SDL_GetTicks();
        const int ms = static_cast<int>(now - last);
        last = now;
        model.tick(ms);

        const StartScreenView v = model.view();
        if (v.screen != last_screen)
        {
            last_screen = v.screen;
            last_switch = ImGui::GetTime();
        }

        io.DeltaTime = static_cast<float>(ms) / 1000.0f;
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();

        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        ui::don_ui_frame_begin(static_cast<float>(w), static_cast<float>(h));
        render(model, v);
        if (fade)
        {
            const float t = static_cast<float>(ImGui::GetTime() - last_switch);
            if (t < 0.15f)
                ui::don_ui_fade_overlay(1.0f - t / 0.15f);
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        // DON_START_SCREEN_SHOT: one BMP per distinct screen (the fade is off while shooting).
        if (!shots.empty() && v.screen != Screen::Done)
        {
            std::string key = screen_name(v.screen);
            key += "|" + v.title;
            if (v.screen == Screen::Error)
                key += "|" + v.note;
            if (key != last_shot_key)
            {
                last_shot_key = key;
                SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
                if (!surface)
                {
                    std::fprintf(stderr, "[don] cannot read the screen's pixels: %s\n", SDL_GetError());
                }
                else
                {
                    std::error_code ec;
                    std::filesystem::create_directories(shots, ec);
                    char name[128];
                    std::snprintf(name, sizeof(name), "%02d-%s.bmp", ++shot_index, screen_name(v.screen));
                    const std::filesystem::path path = std::filesystem::path(shots) / name;
                    if (!SDL_SaveBMP(surface, path.string().c_str()))
                        std::fprintf(stderr, "[don] cannot write %s: %s\n", path.string().c_str(), SDL_GetError());
                    SDL_DestroySurface(surface);
                }
            }
        }
        SDL_RenderPresent(renderer);

        if (model.result() != StartScreenResult::None)
            running = false;
        else
            SDL_Delay(16);
    }

    outcome.kind = (model.result() == StartScreenResult::Quit) ? StartScreenOutcomeKind::Quit
                                                               : StartScreenOutcomeKind::PlayLocal;
    if (model.result() == StartScreenResult::Net)
    {
        outcome.kind = StartScreenOutcomeKind::Net;
        outcome.host = model.host();
        outcome.players = model.players();
        outcome.port = model.port();
    }
    outcome.name = model.name();
    outcome.address = model.address();
    outcome.last_players = model.last_players();
    // PLAY has no player count: the game asks itself. The pad layout follows the controllers connected now (the
    // keyboard is one): more than two take the multitap one in host.cpp.
    int connected = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&connected))
        SDL_free(ids);
    outcome.local_players = (1 + connected > 4) ? 4 : 1 + connected;
    outcome.fullscreen = model.fullscreen();
    outcome.renderer = model.renderer();
    outcome.vibration = model.vibration();
    outcome.net_delay = model.net_delay();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_StopTextInput(window);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    for (SDL_Gamepad *pad : opened_pads)
        SDL_CloseGamepad(pad);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
    return outcome;
}

// ---- the real layer (netplay.h + upnp.h) ---------------------------------

// The game files' hash, the check HELLO makes ("another language variant"). It is computed only when the
// player actually hosts or joins; a failure (no game folder, e.g. the synthetic guest) is one [don-net] line
// and 0, so a local run is never blocked and two machines without the files still agree with each other.
static uint64_t game_hash(const std::string &elf_path)
{
    std::string error;
    const uint64_t hash = don_net::hash_game_files(std::filesystem::path(elf_path), error);
    if (!error.empty())
        std::fprintf(stderr, "[don-net] %s\n", error.c_str());
    return hash;
}

StartScreenNetLayer::StartScreenNetLayer(std::string build_id, std::string elf_path, int delay_ms)
    : m_build_id(std::move(build_id)), m_elf_path(std::move(elf_path)), m_delay_ms(delay_ms)
{
}

StartScreenNetLayer::~StartScreenNetLayer()
{
    upnp_wait(); // the discovery thread may still hold the router's answer
    reset();
}

void StartScreenNetLayer::upnp_wait()
{
    if (m_upnp_thread.joinable())
        m_upnp_thread.join();
}

void StartScreenNetLayer::post(const NetEvent &e)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(e);
}

// BYE to the peers, the UPnP mapping removed, the lobby forgotten. Every exit of the layer goes through here
// (the player backing out, the window closing, the destructor), so nothing of a session outlives the screen.
void StartScreenNetLayer::reset()
{
    if (m_netplay)
    {
        if (!m_silent)
            m_netplay->cancel();
        m_netplay.reset();
    }
    if (m_upnp_open)
    {
        don_net::upnp_close();
        m_upnp_open = false;
    }
    m_silent = true;
    m_welcomed = false;
    m_lobby.clear();
    m_local_player = -1;
}

void StartScreenNetLayer::host_begin(uint16_t port, int players, const std::string &name, int net_delay)
{
    upnp_wait();
    reset();
    m_silent = false;
    m_host = true;
    m_name = name;
    m_local_player = 0; // the host is the wire's player 0 (the model draws its own line)
    m_session = don_net::NetSession{};

    std::string error;
    std::unique_ptr<don_net::Transport> transport = don_net::open_udp(port, error);
    if (!transport)
    {
        // "bind port 7877: Address already in use": another copy of the game, or another program.
        NetEvent e;
        e.type = NetEvent::Type::HostBindFailed;
        e.error = error.find("in use") != std::string::npos ? NetError::PortInUse : NetError::Other;
        e.text = error;
        post(e);
        m_silent = true; // nothing to poll: the screen shows the Error state
        return;
    }

    don_net::NetConfig config;
    config.host = true;
    config.port = port;
    config.players = players;
    // 7.3: the Settings' delay wins over the layer's automatic default; a command-line --net-delay wins over it.
    config.delay = (m_delay_ms > 0) ? m_delay_ms : net_delay;
    config.name = name;
    config.build_id = m_build_id;
    config.data_hash = game_hash(m_elf_path);
    m_netplay = std::make_unique<don_net::Netplay>(std::move(config), std::move(transport));
    if (!m_netplay->start(error))
    {
        NetEvent e;
        e.type = NetEvent::Type::ConnectFailed;
        e.error = NetError::Other;
        e.text = error;
        post(e);
        m_silent = true;
        return;
    }

    NetEvent bound;
    bound.type = NetEvent::Type::HostBound;
    post(bound);

    m_upnp_thread = std::thread([this, port]() {
        const don_net::UpnpResult upnp = don_net::upnp_open(port);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_upnp_open = upnp.mapped;
        }
        NetEvent e;
        e.type = upnp.mapped ? NetEvent::Type::UpnpMapped : NetEvent::Type::UpnpFailed;
        const std::string local = upnp.local_ip.empty() ? don_net::lan_ipv4() : upnp.local_ip;
        e.ip = upnp.mapped ? upnp.external_ip : local; // the router's public address, or this PC's LAN address
        e.name = local;                                 // this PC's LAN address, for the lobby's line
        e.text = upnp.message;
        post(e);
    });
}

void StartScreenNetLayer::join_begin(const std::string &address, const std::string &name)
{
    upnp_wait();
    reset();
    m_silent = false;
    m_host = false;
    m_name = name;
    m_local_player = -1;
    m_session = don_net::NetSession{};

    std::string error;
    std::unique_ptr<don_net::Transport> transport = don_net::open_udp(0, error);
    if (!transport)
    {
        NetEvent e;
        e.type = NetEvent::Type::ConnectFailed;
        e.error = NetError::Other;
        e.text = error;
        post(e);
        m_silent = true;
        return;
    }

    don_net::NetConfig config;
    config.host = false;
    config.port = 0;
    config.join = address;
    config.players = 0;
    config.delay = m_delay_ms;
    config.name = name;
    config.build_id = m_build_id;
    config.data_hash = game_hash(m_elf_path);
    m_netplay = std::make_unique<don_net::Netplay>(std::move(config), std::move(transport));
    if (!m_netplay->start(error))
    {
        NetEvent e;
        e.type = NetEvent::Type::ConnectFailed;
        e.error = NetError::Resolve;
        e.text = error;
        post(e);
        m_silent = true;
    }
}

void StartScreenNetLayer::host_start()
{
    if (m_netplay)
        m_netplay->request_start(); // measures the delay (up to a second) and sends START to each client
}

void StartScreenNetLayer::cancel()
{
    upnp_wait();
    reset();
}

std::unique_ptr<don_net::Netplay> StartScreenNetLayer::take_netplay()
{
    upnp_wait();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_upnp_open = false;
    }
    m_silent = true; // the caller owns the session now: this layer sends no BYE for it
    return std::move(m_netplay);
}

NetEvent StartScreenNetLayer::failure_event(const std::string &error) const
{
    NetEvent e;
    e.text = error;
    if (error.rfind("rejected:", 0) == 0)
    {
        e.type = NetEvent::Type::Rejected;
        e.text = error.substr(std::string("rejected: ").size());
        if (e.text.find("another language variant") != std::string::npos)
            e.reason = don_net::kRejectData;
        else if (e.text.find("full") != std::string::npos)
            e.reason = don_net::kRejectFull;
        else if (e.text.find("already started") != std::string::npos)
            e.reason = don_net::kRejectStarted;
        else
            e.reason = don_net::kRejectBuild; // "host <id> / you <id>"
        return e;
    }
    if (error.find("resolve") != std::string::npos)
    {
        e.type = NetEvent::Type::ConnectFailed;
        e.error = NetError::Resolve;
        return e;
    }
    if (error.find("host left") != std::string::npos)
    {
        e.type = NetEvent::Type::HostLeft;
        e.error = NetError::HostLeft;
        return e;
    }
    e.type = NetEvent::Type::ConnectFailed;
    e.error = NetError::Other;
    return e;
}

void StartScreenNetLayer::report_lobby(std::vector<NetEvent> &out)
{
    const std::vector<don_net::LobbyPeer> list = m_netplay->lobby();
    for (const don_net::LobbyPeer &old : m_lobby)
    {
        if (old.player == m_local_player)
            continue;
        const bool still = std::any_of(list.begin(), list.end(), [&old](const don_net::LobbyPeer &p) {
            return p.player == old.player;
        });
        if (!still)
        {
            NetEvent e;
            e.type = NetEvent::Type::PlayerLeft;
            e.player = old.player;
            out.push_back(e);
        }
    }
    for (const don_net::LobbyPeer &p : list)
    {
        if (p.player == m_local_player)
            continue;
        const bool changed = std::none_of(m_lobby.begin(), m_lobby.end(), [&p](const don_net::LobbyPeer &old) {
            return old.player == p.player && old.name == p.name && old.ping_ms == p.ping_ms;
        });
        if (changed)
        {
            NetEvent e;
            e.type = NetEvent::Type::PlayerJoined;
            e.player = p.player;
            e.name = p.name;
            e.ping_ms = p.ping_ms;
            out.push_back(e);
        }
    }
    m_lobby = list;
}

void StartScreenNetLayer::poll(std::vector<NetEvent> &out)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.insert(out.end(), m_events.begin(), m_events.end());
        m_events.clear();
    }
    if (!m_netplay || m_silent)
        return;

    don_net::NetSession session;
    std::string error;
    switch (m_netplay->poll(session, error))
    {
    case don_net::HandshakeState::Connecting:
        break; // the model counts the seconds of its own 10 s timeout
    case don_net::HandshakeState::Lobby:
        if (m_host)
        {
            for (std::string &n : m_netplay->take_notices())
            {
                NetEvent e;
                e.type = NetEvent::Type::PlayerNotice;
                e.text = std::move(n);
                out.push_back(std::move(e));
            }
        }
        else if (!m_welcomed)
        {
            m_welcomed = true;
            m_local_player = m_netplay->welcome_player(); // WELCOME's index, not the name-guess
            NetEvent e;
            e.type = NetEvent::Type::WelcomeReceived;
            e.name = m_netplay->host_name();
            e.player = m_local_player;
            e.players = m_netplay->connected_players();
            out.push_back(e);
        }
        report_lobby(out);
        break;
    case don_net::HandshakeState::Started:
    {
        m_session = session;
        m_silent = true; // the screen ends here; the caller takes the session
        NetEvent e;
        e.type = NetEvent::Type::SessionStarted;
        e.players = session.players;
        out.push_back(e);
        break;
    }
    case don_net::HandshakeState::Failed:
        m_silent = true;
        out.push_back(failure_event(error));
        break;
    }
}

void don_show_net_session_end(const std::string &reason)
{
    std::fprintf(stderr, "[don-net] %s\n", reason.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Battle Stadium D.O.N - online game", reason.c_str(), nullptr);
}

} // namespace don_host
