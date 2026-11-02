// SPDX-License-Identifier: GPL-3.0-or-later
// The one prepare driver: see prepare_driver.h.
#include "prepare_driver.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "build_id.h"
#include "game_module.h"
#include "../setup/estimate.h"
#include "../setup/module_build.h"
#include "../setup/module_front.h"
#include "../setup/module_manifest.h"
#include "../setup/prepare_engine.h"
#include "../setup/storage.h"

namespace fs = std::filesystem;
using namespace don_setup;

namespace don_host {
namespace {


std::string read_marker(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_marker(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    f.close();
    return static_cast<bool>(f);
}

// The installed module that serves every edition: verified, of this program's ABI and version and built with this
// install's toolchain. The program file is the same in every edition (one SHA-256 in config/discs.toml), so the tree
// it generates, and with it the module, is the same too; the id (a hash of tree, toolchain and ABI) is not
// recomputed here so that it cannot disagree with what the build half wrote. Empty when there is none: the full
// build then runs.
std::string reusable_module(const fs::path& root, const InstallLayout& l) {
    std::ifstream t(l.toolchain / "TOOLCHAIN.sha256");
    std::string toolchain;
    t >> toolchain;
    if (toolchain.empty()) return {};
    std::error_code ec;
    std::string found;
    for (const auto& entry : fs::directory_iterator(root / "module", ec)) {
        const std::string id = entry.path().filename().string();
        if (!fs::is_directory(entry.symlink_status(ec)) || (id.size() > 4 && id.compare(id.size() - 4, 4, ".tmp") == 0))
            continue;
        DonModuleManifest m;
        std::string why;
        if (don_read_module_manifest(entry.path() / "manifest.json", m, why) && m.verified &&
            m.abi_version == kDonGameModuleAbiVersion && m.toolchain_hash == toolchain &&
            m.program_version == don_build_id() && (found.empty() || id > found))
            found = id;
    }
    return found;
}

// What identifies the image a front run works on: its path, size and modification time. A finished run of another
// image must not stand in for this one.
std::string image_key(const fs::path& image) {
    std::error_code ec;
    const uintmax_t size = fs::is_directory(image, ec) ? 0 : fs::file_size(image, ec);
    const auto when = fs::last_write_time(image, ec).time_since_epoch().count();
    return fs::absolute(image, ec).string() + "\n" + std::to_string(size) + "\n" + std::to_string(when) + "\n";
}

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Copy: return "copy";
        case Stage::Translate: return "translate";
        case Stage::Build: return "build";
        case Stage::Verify: return "verify";
        case Stage::Done: return "done";
        case Stage::Failed: return "failed";
    }
    return "?";
}

// DON_TRACE_SETUP=1: "[don] setup progress <percent> <stage>", at most once a second and at every phase change.
class ProgressTrace {
public:
    void note(const Progress& p) {
        if (std::getenv("DON_TRACE_SETUP") == nullptr) return;
        const auto now = std::chrono::steady_clock::now();
        if (p.stage == last_stage_ && now - last_ < std::chrono::seconds(1)) return;
        last_ = now;
        last_stage_ = p.stage;
        std::printf("[don] setup progress %d %s\n", static_cast<int>(p.fraction * 100.0), stage_name(p.stage));
        std::fflush(stdout);
    }

private:
    std::chrono::steady_clock::time_point last_{};
    Stage last_stage_ = Stage::Failed;
};

Progress step_of(Stage stage, double fraction, const std::string& text) {
    Progress p;
    p.stage = stage;
    p.fraction = fraction;
    p.detail = text;
    return p;
}

// Runs the two front jobs. A job's output is a marker file whose hash the journal records:
//   disc.done       the variant id, once the image is identified and its files extracted;
//   recompile.done  the tree hash, once the config is rendered, the recompiler has run and the tree matches.
class FrontRunner : public JobRunner {
public:
    FrontRunner(fs::path image, FrontPaths paths, const PrepareSink& sink)
        : image_(std::move(image)), paths_(std::move(paths)), sink_(sink) {}

    bool run(const Job& job, Speed) override {
        if (job.id == "disc") return run_disc(job);
        return run_recompile(job);
    }

    StageResult failure;
    FrontOutput out;
    std::string reuse_tree;   // set: a module for this program tree is installed, so the tree is not generated again

private:
    bool fail(const StageResult& r) { failure = r; return false; }
    bool marker(const Job& job, const std::string& text) {
        return write_marker(job.output, text) || fail({false, ErrorId::E31, "cannot write " + job.output.string()});
    }

    bool run_disc(const Job& job) {
        auto progress = [this](double f, const std::string& d) { sink_(step_of(Stage::Copy, kShareDisc * f, d), false); };
        sink_(step_of(Stage::Copy, 0.0, "checking the disc image"), true);
        StageResult r = identify_stage(image_, progress, out);
        if (r.ok && !drop_prepared_edition(paths_.root, out.identity.variant_id))   // before the extract, see storage.h
            return fail({false, ErrorId::E32, "game/prepared.txt"});
        if (r.ok) {
            sink_(step_of(Stage::Copy, 0.0, "copying the game files"), true);
            r = extract_stage(image_, paths_, progress, out);
        }
        if (!r.ok) return fail(r);
        return marker(job, out.identity.variant_id);
    }

    bool run_recompile(const Job& job) {
        if (!reuse_tree.empty()) {
            sink_(step_of(Stage::Translate, kShareDisc + kShareRecompile, "reusing the prepared game module"), true);
            return marker(job, reuse_tree);
        }
        if (out.game_dir.empty()) {
            const std::string variant = read_marker(paths_.root / "build/tmp/front/disc.done");
            out.identity.variant_id = variant;
            out.game_dir = paths_.root / "game" / variant;
        }
        auto progress = [this](double f, const std::string& d) {
            sink_(step_of(Stage::Translate, kShareDisc + kShareRecompile * f, d), false);
        };
        sink_(step_of(Stage::Translate, kShareDisc, "translating the game code"), true);
        StageResult r = config_stage(paths_, out);
        if (r.ok) r = recompile_stage(paths_, 0, progress, out);
        if (r.ok) r = iop_native_stage(paths_, out);   // between the recompiler and the tree hash (T-694)
        if (r.ok) {
            sink_(step_of(Stage::Verify, kShareDisc + kShareRecompile, "checking the translated code"), true);
            r = verify_stage(paths_, out);
        }
        if (!r.ok) return fail(r);
        return marker(job, out.tree_hash);
    }

    fs::path image_;
    FrontPaths paths_;
    const PrepareSink& sink_;
};

}  // namespace

PrepareOutcome prepare_image(const fs::path& image, const InstallLayout& layout, const fs::path& root,
                             const PrepareSink& sink, const std::atomic<bool>* stop) {
    PrepareOutcome result;
    std::error_code ec;
    const fs::path front_dir = root / "build" / "tmp" / "front";
    // A marker is only a proof while what it stands for is still there.
    const fs::path disc_done = front_dir / "disc.done";
    const fs::path recompile_done = front_dir / "recompile.done";
    const std::string known_variant = read_marker(disc_done);
    if (!fs::is_directory(root / "game" / known_variant, ec) || known_variant.empty()) fs::remove(disc_done, ec);
    if (!fs::is_directory(front_gen_dir(root), ec)) fs::remove(recompile_done, ec);

    const std::string key = image_key(image);
    if (read_marker(front_dir / "image.key") != key) {   // another image than the last run: start over
        fs::remove(disc_done, ec);
        fs::remove(recompile_done, ec);
        write_marker(front_dir / "image.key", key);
    }
    const std::string shared_id = reusable_module(root, layout);
    const bool reuse = !shared_id.empty();

    FrontPaths paths{root, layout.root, layout.recomp};
    FrontRunner runner(image, paths, sink);
    if (reuse) runner.reuse_tree = "shared";
    std::vector<Job> jobs;
    jobs.push_back({"disc", kShareDisc, disc_done, Stage::Copy, false});
    jobs.push_back({"recompile", kShareRecompile, recompile_done, Stage::Translate, false});
    PrepareEngine engine(front_dir, jobs, runner);
    while (!engine.finished()) {
        if (stop != nullptr && stop->load()) { result.stopped = true; return result; }
        engine.poll();
        if (engine.failed()) {
            result.error = runner.failure.error == ErrorId::None ? ErrorId::E50 : runner.failure.error;
            result.detail = runner.failure.detail;
            return result;
        }
    }

    const std::string variant = read_marker(disc_done);
    result.variant = variant;
    if (reuse) {   // same program file, same tree, same module: only the game data of this edition is new
        if (!record_prepared_edition(root, variant, shared_id)) { result.error = ErrorId::E32; result.detail = "game/prepared.txt"; return result; }
        result.ok = result.reused = true;
        result.module_id = shared_id;
        sink(step_of(Stage::Done, 1.0, "done: module " + shared_id + " reused for " + variant), true);
        return result;
    }
    if (stop != nullptr && stop->load()) { result.stopped = true; return result; }
    ModuleBuildOptions o;
    o.paths = {front_gen_dir(root), layout.sdk, layout.toolchain};
    o.root = root;
    o.variant = variant;
    o.program_version = don_build_id();
    if (const char* jobs_env = std::getenv("DON_PREPARE_JOBS"); jobs_env != nullptr && jobs_env[0] != '\0')
        o.max_jobs = static_cast<size_t>(std::strtoul(jobs_env, nullptr, 10));
    o.on_progress = [&](const Progress& p) {
        Progress q = step_of(Stage::Build, kShareDisc + kShareRecompile + kShareBuild * p.fraction,
                             p.detail.empty() ? "building the game module" : p.detail);
        q.parts_done = p.parts_done;
        q.parts_total = p.parts_total;
        sink(q, false);
    };
    o.stop = stop;
    sink(step_of(Stage::Build, kShareDisc + kShareRecompile, "building the game module"), true);
    const ModuleBuildResult r = build_module(o);
    if (r.stopped) { result.stopped = true; return result; }   // the compilers are dead, the journal keeps the finished units
    if (!r.ok) { result.error = ErrorId::E53; result.detail = r.error; return result; }
    if (!record_prepared_edition(root, variant, r.module_id)) { result.error = ErrorId::E32; result.detail = "game/prepared.txt"; return result; }
    result.ok = true;
    result.module_id = r.module_id;
    sink(step_of(Stage::Done, 1.0, "done: module " + r.module_id + " for " + variant), true);
    return result;
}

// ---------------------------------------------------------------- the background job

AsyncPrepare::~AsyncPrepare() {
    stop_ = true;
    join();
}

void AsyncPrepare::stop_and_wait() {
    stop_ = true;
    join();
}

void AsyncPrepare::join() {
    if (thread_.joinable()) thread_.join();
}

// The time left from the rate of the stage that is running, measured from that stage's own start (T-718); unknown
// (negative) while the stage has done too little of its range. An earlier, faster stage never sets the rate.
double AsyncPrepare::seconds_left(double fraction, double stage_start, std::chrono::steady_clock::time_point stage_since) const {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - stage_since).count();
    return don_setup::stage_seconds_left(fraction, stage_start, elapsed, 1.0, kEtaMinStageFraction);
}

AsyncSnapshot AsyncPrepare::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void AsyncPrepare::start_identify(const fs::path& image) {
    {   // a running prepare is not interrupted by a check; a running check is short, so it is waited for
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_ && state_.was_prepare) return;
    }
    join();
    done_ = false;
    started_ = std::chrono::steady_clock::now();
    { std::lock_guard<std::mutex> lock(mutex_); state_ = AsyncSnapshot{}; state_.running = true; }
    thread_ = std::thread([this, image] {
        FrontOutput out;
        auto progress = [this](double f, const std::string& d) {
            Progress p = step_of(Stage::Copy, f, d);
            p.seconds_left = seconds_left(f, 0.0, started_);
            std::lock_guard<std::mutex> lock(mutex_);
            state_.progress = p;
        };
        identify_stage(image, progress, out);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.identity = out.identity;
        state_.running = false;
        state_.has_result = true;
        done_ = true;
    });
}

void AsyncPrepare::start_prepare(const fs::path& image, const InstallLayout& layout, const fs::path& root) {
    if (!done_) { stop_ = false; return; }   // a stopped run that has not reached its next boundary goes on
    join();
    stop_ = false;
    done_ = false;
    started_ = std::chrono::steady_clock::now();
    { std::lock_guard<std::mutex> lock(mutex_); state_ = AsyncSnapshot{}; state_.running = true; state_.was_prepare = true; }
    thread_ = std::thread([this, image, layout, root] {
        double stage_start = 0.0;
        Stage stage = Stage::Copy;
        auto stage_since = started_;
        ProgressTrace trace;
        const PrepareSink sink = [this, &stage_start, &stage, &stage_since, &trace](const Progress& p, bool) {
            if (p.stage != stage) { stage = p.stage; stage_start = p.fraction; stage_since = std::chrono::steady_clock::now(); }
            trace.note(p);
            Progress q = p;
            // Copy and translate end well before 1.0 and run faster than the build: only the build and what follows
            // extrapolate to the end of the run.
            q.seconds_left = p.stage >= Stage::Build ? seconds_left(p.fraction, stage_start, stage_since) : don_setup::kSecondsLeftUnknown;
            std::lock_guard<std::mutex> lock(mutex_);
            state_.progress = q;
        };
        PrepareOutcome outcome = prepare_image(image, layout, root, sink, &stop_);
        std::lock_guard<std::mutex> lock(mutex_);
        state_.outcome = std::move(outcome);
        state_.running = false;
        state_.has_result = true;
        done_ = true;
    });
}

}  // namespace don_host
