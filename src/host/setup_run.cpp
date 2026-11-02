// SPDX-License-Identifier: GPL-3.0-or-later
// The first-run setup screens driving the real preparation: see setup_run.h.
#include "setup_run.h"

#include <algorithm>
#include <fstream>
#include <thread>

#ifndef DON_SETUP_RUN_NO_SDL
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <future>

#include "file_browser_view.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "setup_chime.h"
#include "setup_screen.h"
#include "setup_script.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "game_module.h"
#include "../setup/discover.h"
#include "../setup/storage.h"
#include "../setup/user_paths.h"
#endif

namespace fs = std::filesystem;
using namespace don_setup;

namespace don_host {

// ---------------------------------------------------------------- controller (no SDL)

SetupController::SetupController(SetupModel& model, SetupBackend& backend,
                                 std::function<bool(const ActionRequest&)> ui_action)
    : model_(model), backend_(backend), ui_action_(std::move(ui_action)) {}

void SetupController::start() {
    model_.open_first_run();
    model_.set_environment(backend_.environment());
    int percent = 0;
    if (backend_.resumable(image_, percent)) {   // a stopped run: offer to continue it (A3 "Resume")
        model_.open_resume(percent, 0.0);
        return;
    }
    backend_.begin_discovery();
    searching_ = true;
}

// Identifies the next found image for the S1 offer; with none left the search ends with what was recognised.
void SetupController::next_offer_check() {
    if (offer_next_ >= offer_queue_.size()) {
        end_offer_checks(true);
        return;
    }
    const std::string& file = offer_queue_[offer_next_++];
    backend_.begin_offer_check(file);
    offer_running_ = true;
    model_.set_offer_check(file, static_cast<double>(offer_next_ - 1) / offer_queue_.size());
}

// `publish`: the model gets the recognised images (maybe none). Not after the player chose a file: the screen moved on.
void SetupController::end_offer_checks(bool publish) {
    offer_running_ = false;
    offer_queue_.clear();
    offer_next_ = 0;
    if (publish) model_.set_discovery(identified_);
}

void SetupController::feed(const AsyncSnapshot& s) {
    if (s.running) {
        if (model_.screen() == Screen::Checking)
            model_.set_check_progress(s.progress.fraction, s.progress.seconds_left);
        else if (s.was_prepare && !s.progress.detail.empty())
            model_.set_progress(s.progress);
        return;
    }
    if (!s.has_result || consumed_) return;
    consumed_ = true;
    if (!s.was_prepare) {
        model_.set_disc_identity(s.identity);
        return;
    }
    const PrepareOutcome& o = s.outcome;
    if (o.stopped) return;
    if (o.ok) {
        Progress done;
        done.stage = Stage::Done;
        done.fraction = 1.0;
        model_.set_progress(done);
        return;
    }
    const std::string percent = std::to_string(static_cast<int>(s.progress.fraction * 100.0));
    model_.set_error_id(o.error == ErrorId::None ? ErrorId::E50 : o.error,
                        {{"reason", o.detail}, {"file", o.detail}, {"percent", percent}});
    if (!model_.has_error()) {   // a silent catalogue entry: the failure is still shown
        Progress failed;
        failed.stage = Stage::Failed;
        failed.detail = o.detail;
        model_.set_progress(failed);
    }
}

void SetupController::act(const ActionRequest& a) {
    switch (a.action) {
        case Action::IdentifyDisc:
            offers_abandoned_ = true;
            end_offer_checks(false);
            image_ = a.path;
            consumed_ = false;
            backend_.identify(a.path);
            break;
        case Action::StartPrepare:
        case Action::Resume:
            consumed_ = false;
            backend_.prepare(image_);
            break;
        case Action::Cancel:
        case Action::CancelIdentify:
            backend_.stop();
            break;
        case Action::Play: end_ = SetupEnd::Play; break;
        case Action::Quit: end_ = SetupEnd::Quit; break;
        default:
            if (ui_action_) ui_action_(a);   // the file dialog and the in-app browser; the rest has no effect here
            break;
    }
}

SetupEnd SetupController::step() {
    if (searching_) {
        std::vector<Candidate> found;
        if (backend_.discovery_ready(found)) {
            searching_ = false;
            if (!offers_abandoned_) {
                offer_queue_.clear();
                for (const Candidate& c : found) offer_queue_.push_back(c.path);
                offer_next_ = 0;
                identified_.clear();
                next_offer_check();
            }
        }
    }
    if (offer_running_) {
        DiscIdentity id;
        double fraction = 0.0;
        const std::string& file = offer_queue_[offer_next_ - 1];
        if (backend_.offer_check_poll(id, fraction)) {
            offer_running_ = false;
            // Offered only what `don --prepare` would accept as is: a known edition, no modified game data (E17).
            if (id.known && id.error == ErrorId::None) {
                identified_.push_back({file, id.description});
                end_offer_checks(true);
            } else {
                next_offer_check();
            }
        } else {
            model_.set_offer_check(file, (static_cast<double>(offer_next_ - 1) + fraction) / offer_queue_.size());
        }
    }
    feed(backend_.poll());
    for (const ActionRequest& a : model_.take_actions()) act(a);
    busy_ = searching_ || offer_running_ || (!model_.has_error() &&
                           (model_.screen() == Screen::Checking || model_.screen() == Screen::Preparing));
    return end_;
}

#ifndef DON_SETUP_RUN_NO_SDL
namespace {

// A3 S0: the search for the disc image stops after 2 s (T-627).
constexpr std::chrono::milliseconds kDiscoverBudget{2000};
constexpr int kScriptFramesDefault = 180;   // a key of DON_SETUP_SCRIPT waits for this many unchanged frames
constexpr Uint32 kFrameMs = 16;

class RealBackend : public SetupBackend {
public:
    RealBackend(const InstallLayout& layout, const fs::path& root) : layout_(layout), root_(root) {}

    Environment environment() override {
        Environment e;
        std::error_code ec;
        e.free_bytes = fs::space(root_, ec).available;
        e.drive = root_.root_name().empty() ? root_.string() : root_.root_name().string();
        e.cores = static_cast<int>(std::thread::hardware_concurrency());
        return e;   // needed_bytes and the estimate stay 0 (unknown): nothing is blocked on a guess
    }
    void begin_discovery() override {
        std::string home, user;
        file_browser_home(home, user);
        discovery_ = std::async(std::launch::async, [home, user] {
            return discover(discovery_roots(home, user), kDiscoverBudget);
        });
    }
    bool discovery_ready(std::vector<Candidate>& found) override {
        if (!discovery_.valid() || discovery_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        const DiscoverResult r = discovery_.get();
        for (const fs::path& p : r.found) found.push_back({p.string(), {}});   // compressed images are never offered
        return true;
    }
    bool resumable(std::string& image, int& percent) override {
        // The journal of a stopped run: image.key (the image path on its first line) and the markers of finished jobs.
        const fs::path front = root_ / "build" / "tmp" / "front";
        std::error_code ec;
        std::ifstream key(front / "image.key");
        std::getline(key, image);
        if (image.empty() || !fs::exists(image, ec) || !fs::exists(front / "disc.done", ec)) return false;
        if (find_installed_module(root_, kDonGameModuleAbiVersion).found) return false;
        percent = static_cast<int>(100.0 * (fs::exists(front / "recompile.done", ec) ? kShareDisc + kShareRecompile : kShareDisc));
        return true;
    }
    void begin_offer_check(const std::string& image) override { offer_job_.start_identify(image); }
    bool offer_check_poll(DiscIdentity& id, double& fraction) override {
        const AsyncSnapshot s = offer_job_.snapshot();
        fraction = s.progress.fraction;
        if (s.running || !s.has_result) return false;
        id = s.identity;
        return true;
    }
    void identify(const std::string& image) override { job_.start_identify(image); }
    void prepare(const std::string& image) override { job_.start_prepare(image, layout_, root_); }
    void stop() override { job_.request_stop(); }
    void stop_and_wait() { job_.stop_and_wait(); }
    AsyncSnapshot poll() override { return job_.snapshot(); }

    bool running() { return job_.snapshot().running; }

private:
    InstallLayout layout_;
    fs::path root_;
    std::future<DiscoverResult> discovery_;
    AsyncPrepare job_;
    AsyncPrepare offer_job_;   // the S1 offer checks, apart from the player's own job
};

bool save_frame(SDL_Renderer* renderer, const fs::path& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return false;
    const bool ok = SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return ok;
}

}  // namespace

SetupEnd setup_run(const InstallLayout& layout) {
    ErrorId roots_error = ErrorId::None;
    const UserRoots roots = user_roots(&roots_error);
    if (roots_error != ErrorId::None) {
        std::fprintf(stderr, "[don] the user folder is not usable (error %d)\n", static_cast<int>(roots_error));
        return SetupEnd::Quit;
    }
    std::error_code ec;
    fs::create_directories(roots.data, ec);
    InstanceLock lock = acquire_single_instance(roots.data);   // held while preparing; the game takes its own after
    if (!lock) {
        std::fprintf(stderr, "[don] another copy of the program is preparing the game\n");
        return SetupEnd::Quit;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "[don] setup: SDL could not start: %s\n", SDL_GetError());
        return SetupEnd::Quit;
    }
    int w = 1280, h = 720;   // the size the demo and the start screen use for screenshots
    if (const char* s = std::getenv("DON_START_SCREEN_SIZE"))
        if (int a = 0, b = 0; std::sscanf(s, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { w = a; h = b; }
    SDL_Window* window = SDL_CreateWindow("Battle Stadium D.O.N", w, h, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "[don] setup: no window or renderer: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return SetupEnd::Quit;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    ui::don_ui_apply_theme();
    if (!ui::don_ui_load_fonts()) std::fprintf(stderr, "[don] setup: could not build the Nunito fonts\n");

    const char* shots_env = std::getenv("DON_SETUP_SHOTS");
    const fs::path shots = shots_env ? fs::path(shots_env) : fs::path();
    if (!shots.empty()) fs::create_directories(shots, ec);
    const char* frames_env = std::getenv("DON_SETUP_FRAMES");
    const int script_frames = frames_env ? std::max(1, std::atoi(frames_env)) : kScriptFramesDefault;
    const std::vector<ScriptStep> script = parse_script(std::getenv("DON_SETUP_SCRIPT"));
    const bool trace = std::getenv("DON_TRACE_SETUP") != nullptr;
    const char* quit_env = std::getenv("DON_SETUP_QUIT_AT");
    const int quit_at = quit_env ? std::atoi(quit_env) : -1;

    RealBackend backend(layout, roots.data);
    SetupModel model;
    SetupController controller(model, backend, [&](const ActionRequest& a) { return setup_screen_handle_action(model, a); });
    controller.start();

    SetupEnd end = SetupEnd::Running;
    size_t next_key = 0;
    int stable = 0, shot_index = 0;
    std::string last_key, last_shown;
    while (end == SetupEnd::Running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            setup_screen_event(model, ev);
        }
        end = controller.step();
        if (quit_at >= 0 && end == SetupEnd::Running) {   // test hook: the player closes the window at this percent
            const AsyncSnapshot s = backend.poll();
            if (s.running && s.was_prepare && s.progress.fraction * 100.0 >= quit_at) end = SetupEnd::Quit;
        }
        const View view = model.view();
        const std::string key = std::string(setup_screen_name(view.screen)) + (view.error_overlay ? "-error" : "") +
                                (!view.menu.empty() && view.menu[0].label == kUseOffer ? "-offer" : "") +
                                (file_browser_active() ? "-browser" : "");
        stable = key == last_key ? stable + 1 : 0;
        if (next_key < script.size() && stable >= script_frames && (script[next_key].now || !controller.busy())) {
            apply_script_step(model, script[next_key++]);
            stable = 0;
        }
        SDL_GetWindowSize(window, &w, &h);
        io.DeltaTime = static_cast<float>(kFrameMs) / 1000.0f;
        ImGui_ImplSDL3_NewFrame();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui::NewFrame();
        ui::don_ui_frame_begin(static_cast<float>(w), static_cast<float>(h));
        setup_screen_draw(model, view);
        setup_screen_effects(window, view);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        // The trace names the error code too (E-DISC-3...), so a wrong error with the same screen key is seen.
        const std::string shown = view.error_overlay ? key + " " + view.error.code : key;
        if (shown != last_shown) {
            last_shown = shown;
            if (trace) std::printf("[don] setup screen: %s\n", shown.c_str());
        }
        if (key != last_key) {
            last_key = key;
            if (!shots.empty()) {
                char name[96];
                std::snprintf(name, sizeof(name), "%02d-%s.png", ++shot_index, key.c_str());
                if (!save_frame(renderer, shots / name)) std::fprintf(stderr, "[don] setup: cannot write %s\n", name);
            }
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(kFrameMs);
    }
    const bool still_running = backend.running();
    if (still_running) backend.stop_and_wait();   // closed while preparing: the compilers die, the worker is joined
    setup_screen_end();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    if (still_running && end == SetupEnd::Quit) {   // the journal on disk is what resumes
        std::printf("[don] setup: closed while preparing; the next start continues\n");
        std::fflush(stdout);
    }
    return end;
}
#endif

}  // namespace don_host
