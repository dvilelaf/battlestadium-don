// SPDX-License-Identifier: GPL-3.0-or-later
// The one prepare driver (T-649): from the player's disc image to a prepared game module. `don --prepare` (prepare_cli)
// runs it on the calling thread; the first-run screens (setup_run) run it on a worker thread and read its progress
// from the frame loop, so the window stays responsive. Same stages either way, in this order:
//   disc (identify, extract) -> config -> recompile -> iop_native -> verify -> module build (or reuse of an installed
//   module of another edition: prepare_cli.h, "Editions") -> game/prepared.txt.
// The engine's journal (<root>/build/tmp/front/setup.json) makes a stopped or killed run resume where it stopped.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "prepare_cli.h"
#include "../setup/setup_types.h"

namespace don_host {

// One progress step: stage, overall fraction 0..1, a text, and parts done/total while the module builds. `force` marks
// a phase change or the end (the terminal prints those even inside its once-a-second limit).
using PrepareSink = std::function<void(const don_setup::Progress& progress, bool force)>;

struct PrepareOutcome {
    bool ok = false;
    bool stopped = false;                              // `stop` was raised; the journal keeps the finished jobs
    don_setup::ErrorId error = don_setup::ErrorId::None;
    std::string detail;                                // what went wrong; never game data
    std::string variant;                               // game/<variant>/ prepared
    std::string module_id;
    bool reused = false;                               // an installed module served this edition
};

// The share of the displayed fraction each phase takes. Display only. Measured on the dev machine (16 cores, disc image
// in the page cache, 2026-10-26, T-649 headless run): copy 2 s, translate + iop + verify 31 s, module build ~555 s,
// i.e. about 0.3 / 5 / 94 % of the time. The shares below are a deliberate estimate on top of that: a copy from an
// image that is not cached, or on a slow disk, takes longer than 2 s, so the copy keeps 5 % and the build the rest.
constexpr double kShareDisc = 0.05;       // identify + extract
constexpr double kShareRecompile = 0.10;  // recompile + hash
constexpr double kShareBuild = 1.0 - kShareDisc - kShareRecompile;

// Blocking. `root` is the user's data folder (UserRoots::data). `stop` (may be null) is read between jobs and during the module build, where it kills
// the running compilers.
// DON_PREPARE_JOBS=<n> caps the parallel compile jobs of the build half (0 or unset: all logical cores).
PrepareOutcome prepare_image(const std::filesystem::path& image, const InstallLayout& layout,
                             const std::filesystem::path& root, const PrepareSink& sink,
                             const std::atomic<bool>* stop);

// What a background job has to show the frame loop.
struct AsyncSnapshot {
    bool running = false;
    bool has_result = false;                           // a job ended since the last start
    bool was_prepare = false;                          // the ended job: prepare (true) or identify (false)
    don_setup::Progress progress;                      // latest, with seconds_left from the elapsed time
    PrepareOutcome outcome;                            // prepare result
    don_setup::DiscIdentity identity;                  // identify result
};

// One background job at a time: identify a disc image, or prepare it. snapshot() is safe from any thread.
class AsyncPrepare {
public:
    ~AsyncPrepare();
    void start_identify(const std::filesystem::path& image);
    // Starts a prepare, or, while one still runs after request_stop(), lets it go on.
    void start_prepare(const std::filesystem::path& image, const InstallLayout& layout, const std::filesystem::path& root);
    void request_stop() { stop_ = true; }
    // Raises the stop and waits for the job to end (the running compilers are killed, the journal is kept): the close
    // of the window. The worker is joined, so nothing runs after this returns.
    void stop_and_wait();
    AsyncSnapshot snapshot();

private:
    void join();
    double seconds_left(double fraction, double stage_start, std::chrono::steady_clock::time_point stage_since) const;
    std::thread thread_;
    std::mutex mutex_;
    AsyncSnapshot state_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{true};
    std::chrono::steady_clock::time_point started_;
};

}  // namespace don_host
