// SPDX-License-Identifier: GPL-3.0-or-later
// `don --setup-demo --screenshot-dir <dir>`: the setup screen against a fake engine, one PNG per state. No disc and
// no game: the numbers below are made-up demo data that only feed the model.
#include "setup_demo.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "file_browser_view.h"
#include "setup_chime.h"
#include "setup_demo_fixtures.h"
#include "setup_screen.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace
{
using namespace don_setup;

// Plays the part of the engine: takes the model's actions and feeds results back.
class FakePrepareEngine
{
public:
    Environment env;
    FakePrepareEngine()
    {
        env.free_bytes = 48ull << 30;
        env.needed_bytes = 12ull << 30;
        env.drive = "/home";
        env.cores = 8;
        env.ram_bytes = 16ull << 30;
        env.estimate_seconds = 600.0;
        env.estimate_uncertainty = 0.1;
    }
    void start(SetupModel &m) const
    {
        m.open_first_run();
        m.set_environment(env);
    }
    void pick_disc(SetupModel &m) const { m.file_chosen("/home/demo/Downloads/battle-stadium-don.iso"); }
    void identify(SetupModel &m, ErrorId error = ErrorId::None) const
    {
        DiscIdentity id;
        id.known = true;
        id.variant_id = "demo";
        id.description = "English, Japanese voices (Lord Izen patch, 2023-09-03)"; // long on purpose: the title wraps (T-715)
        id.error = error;
        m.set_disc_identity(id);
        m.take_actions();
    }
    void progress(SetupModel &m, Stage stage, double fraction, int done = 0, int total = 0) const
    {
        Progress p;
        p.stage = stage;
        p.fraction = fraction;
        p.seconds_left = env.estimate_seconds * (1.0 - fraction);
        p.parts_done = done;
        p.parts_total = total;
        m.set_progress(p);
    }
};

SDL_Window *g_demo_window = nullptr;

using don_setup::Nav;
void keys(std::initializer_list<Nav> navs, int repeat = 1)
{
    for (int r = 0; r < repeat; ++r)
        for (Nav n : navs)
            don_host::file_browser_navigate(n);
}

// The in-app browser as the demo shows it, or CHOOSE DISC IMAGE... when the system dialog is missing (E02): the app takes the model's actions.
void open_browser(SetupModel &m, bool dialog_missing)
{
    if (dialog_missing)
        m.file_picker_unavailable();
    else
        m.take_actions();
    if (!dialog_missing)
        don_host::setup_screen_handle_action(m, {Action::OpenBrowser, {}});
    for (const ActionRequest &a : m.take_actions())
        don_host::setup_screen_handle_action(m, a);
}

struct Demo
{
    const char *name;
    std::function<void(SetupModel &, FakePrepareEngine &)> build;
};

std::vector<Demo> demo_states()
{
    return {
        {"welcome", [](SetupModel &, FakePrepareEngine &) {}},
        {"welcome-found",
         [](SetupModel &m, FakePrepareEngine &) {
             m.set_discovery({{"/home/demo/Downloads/battle-stadium-don.iso", "English, Japanese voices (Lord Izen patch, 2023-09-07)"}});
         }},
        {"welcome-offer-checking",
         [](SetupModel &m, FakePrepareEngine &) { m.set_offer_check("/home/demo/Downloads/battle-stadium-don.iso", 0.4); }},
        {"welcome-low-space",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.env.free_bytes = 3ull << 30;
             m.set_environment(e.env);
         }},
        {"checking-start", [](SetupModel &m, FakePrepareEngine &e) { e.pick_disc(m); }},
        {"checking-half",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             m.set_check_progress(0.5, 2.0);
         }},
        {"decision-modified",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m, ErrorId::E17);
         }},
        {"decision-space",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.env.free_bytes = 3ull << 30;
             m.set_environment(e.env);
             e.pick_disc(m);
             e.identify(m);
         }},
        {"decision-battery",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.env.on_battery = setup_demo_fake_battery_state().on_battery;  // from the fake sysfs, as the app reads it
             m.set_environment(e.env);
             e.pick_disc(m);
             e.identify(m);
         }},
        {"decision-low-memory",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.env.low_memory = true;
             m.set_environment(e.env);
             e.pick_disc(m);
             e.identify(m);
         }},
        {"preparing-start",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
         }},
        {"preparing-copy",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Copy, 0.05);
         }},
        {"preparing-translate",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Translate, 0.2);
         }},
        {"preparing-build",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Build, 0.6, 1876, 3000);
         }},
        {"preparing-verify",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Verify, 0.95);
         }},
        {"preparing-stop-confirm",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Build, 0.6, 1876, 3000);
             m.request_close();
         }},
        {"ready-after-preparing",  // the title follows the progress, then the end flashes the window and chimes
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             for (const double f : {0.0, 0.25, 0.5, 0.75})
             {
                 e.progress(m, Stage::Build, f, static_cast<int>(f * 3000), 3000);
                 don_host::setup_screen_effects(g_demo_window, m.view());
             }
             e.progress(m, Stage::Done, 1.0);
         }},
        {"resume",
         [](SetupModel &m, FakePrepareEngine &) { m.open_resume(37, 380.0); }},
        {"error-e08-incomplete-disc",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             m.set_error_id(ErrorId::E08, {{"have", "3.1"}, {"total", "4.2"}});
         }},
        {"error-e31-disk-full",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             e.progress(m, Stage::Build, 0.6, 1876, 3000);
             m.set_error_id(ErrorId::E31, {{"gb", "1.5"}});
         }},
        {"error-e50-program-files",
         [](SetupModel &m, FakePrepareEngine &e) {
             e.pick_disc(m);
             e.identify(m);
             m.set_error_id(ErrorId::E50);
         }},
        {"browser-places", [](SetupModel &m, FakePrepareEngine &) { open_browser(m, false); }},
        {"browser-iso-focused",
         [](SetupModel &m, FakePrepareEngine &) {
             open_browser(m, false);
             keys({Nav::Confirm});         // into Downloads
             keys({Nav::Down}, 4);         // three folders, then archive.gz, then the iso
         }},
        {"browser-scrolled",
         [](SetupModel &m, FakePrepareEngine &) {
             open_browser(m, false);
             keys({Nav::Confirm});
             keys({Nav::Down}, 14);
         }},
        {"browser-e02-fallback",
         [](SetupModel &m, FakePrepareEngine &) {
             open_browser(m, true);
             keys({Nav::Down, Nav::Confirm});  // into Games
             keys({Nav::Down});                 // the long file name
         }},
    };
}

// The same size the start screen uses for its screenshots.
void window_size(int &w, int &h)
{
    w = 1280;
    h = 720;
    if (const char *s = std::getenv("DON_START_SCREEN_SIZE"))
        if (int a = 0, b = 0; std::sscanf(s, "%dx%d", &a, &b) == 2 && a > 0 && b > 0)
        {
            w = a;
            h = b;
        }
}

bool save_frame(SDL_Renderer *renderer, const std::filesystem::path &path)
{
    SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface)
    {
        std::fprintf(stderr, "[don] setup demo: cannot read the pixels: %s\n", SDL_GetError());
        return false;
    }
    const bool ok = SDL_SavePNG(surface, path.string().c_str());
    if (!ok)
        std::fprintf(stderr, "[don] setup demo: cannot write %s: %s\n", path.string().c_str(), SDL_GetError());
    SDL_DestroySurface(surface);
    return ok;
}

} // namespace

int don_setup_demo(const std::string &shots_dir)
{
    if (shots_dir.empty())
    {
        std::fprintf(stderr, "[don] --setup-demo needs --screenshot-dir <dir>\n");
        return 2;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        std::fprintf(stderr, "[don] setup demo: SDL could not start: %s\n", SDL_GetError());
        return 1;
    }
    int w = 0, h = 0;
    window_size(w, h);
    SDL_Window *window = SDL_CreateWindow("Battle Stadium D.O.N", w, h, 0);
    g_demo_window = window;
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer)
    {
        std::fprintf(stderr, "[don] setup demo: no window or renderer: %s\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    don_host::ui::don_ui_apply_theme();
    if (!don_host::ui::don_ui_load_fonts())
        std::fprintf(stderr, "[don] setup demo: could not build the Nunito fonts\n");

    std::error_code ec;
    std::filesystem::create_directories(shots_dir, ec);
    don_host::setup_demo_make_browser_tree();
    int written = 0, index = 0;
    for (const Demo &d : demo_states())
    {
        FakePrepareEngine engine;
        SetupModel model;
        don_host::file_browser_close();
        engine.start(model);
        d.build(model, engine);
        for (int frame = 0; frame < 3; ++frame)
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                ImGui_ImplSDL3_ProcessEvent(&ev);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui_ImplSDL3_NewFrame();
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui::NewFrame();
            don_host::ui::don_ui_frame_begin(static_cast<float>(w), static_cast<float>(h));
            don_host::setup_screen_draw(model, model.view());
            don_host::setup_screen_effects(window, model.view());
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (frame == 2)
            {
                char name[96];
                std::snprintf(name, sizeof(name), "%02d-%s.png", ++index, d.name);
                if (save_frame(renderer, std::filesystem::path(shots_dir) / name))
                    ++written;
            }
            SDL_RenderPresent(renderer);
        }
    }
    std::printf("[don] setup demo: %d screenshots in %s\n", written, shots_dir.c_str());

    don_host::setup_screen_end();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    return written == index ? 0 : 1;
}
