// SPDX-License-Identifier: GPL-3.0-or-later
// `don --prepare <image>`: see prepare_cli.h. The work is prepare_driver's; this file is the terminal around it.
#include "prepare_cli.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>

#include "prepare_driver.h"
#include "../setup/errors.h"
#include "../setup/storage.h"
#include "../setup/user_paths.h"

namespace fs = std::filesystem;
using namespace don_setup;

namespace don_host {
namespace {

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

constexpr std::chrono::seconds kPrintEvery{1};

// One progress line per second at most; `force` prints anyway (phase changes, the end).
class Printer {
public:
    void line(double fraction, const std::string& text, bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && started_ && now - last_ < kPrintEvery) return;
        started_ = true;
        last_ = now;
        std::printf("[don] prepare: %3d%% %s\n", static_cast<int>(fraction * 100.0 + 0.5), text.c_str());
    }

private:
    bool started_ = false;
    std::chrono::steady_clock::time_point last_;
};

int report_error(ErrorId id, const std::string& detail) {
    const ErrorInfo& e = error_info(id);
    std::fprintf(stderr, "[don] prepare failed: %s %s\n", e.code, e.title);
    if (e.why[0]) std::fprintf(stderr, "[don]   %s\n", e.why);
    if (e.what_to_do[0]) std::fprintf(stderr, "[don]   %s\n", e.what_to_do);
    if (!detail.empty()) std::fprintf(stderr, "[don]   detail: %s\n", detail.c_str());
    return 1;
}

}  // namespace

InstallLayout install_layout(const fs::path& program_folder) {
    InstallLayout l;
    l.root = program_folder;
    l.recomp = program_folder / "bin" / (std::string("ps2_recomp") + kExe);
    l.config = program_folder / "config";
    l.sdk = program_folder / "sdk";
    l.toolchain = program_folder / "toolchain";
    return l;
}

fs::path program_folder_of_executable() {
    const char* base = SDL_GetBasePath();
    if (base != nullptr && base[0] != '\0') return fs::path(base);
    std::error_code ec;
    const fs::path here = fs::current_path(ec);
    return ec ? fs::path(".") : here;
}

std::string missing_install_parts(const InstallLayout& l) {
    struct Part { const fs::path& path; const char* name; bool dir; };
    const Part parts[] = {{l.recomp, "bin/ps2_recomp", false},
                          {l.config / "recomp.toml", "config/recomp.toml", false},
                          {l.config / "disc_manifest.toml", "config/disc_manifest.toml", false},
                          {l.sdk / "host" / "game_module.h", "sdk/host/game_module.h", false},
                          {l.sdk / "inc" / "don_guest_macros.h", "sdk/inc/don_guest_macros.h", false},
                          {l.toolchain / "bin", "toolchain/bin", true}};
    std::string missing;
    for (const Part& p : parts) {
        std::error_code ec;
        const bool ok = p.dir ? fs::is_directory(p.path, ec) : fs::is_regular_file(p.path, ec);
        if (!ok) missing += (missing.empty() ? "" : ", ") + std::string(p.name);
    }
    return missing;
}

int run_prepare(const std::string& image, const InstallLayout& layout) {
    const std::string missing = missing_install_parts(layout);
    if (!missing.empty()) {
        std::fprintf(stderr, "[don] this install is incomplete (%s missing next to the program in %s)\n",
                     missing.c_str(), layout.root.string().c_str());
        return 2;
    }
    ErrorId roots_error = ErrorId::None;
    const UserRoots roots = user_roots(&roots_error);
    if (roots_error != ErrorId::None) { report_error(roots_error, "the user folder is not usable"); return 2; }
    std::error_code ec;
    fs::create_directories(roots.data, ec);
    InstanceLock lock = acquire_single_instance(roots.data);
    if (!lock) return report_error(ErrorId::E74, "") ;

    Printer printer;
    const PrepareOutcome r = prepare_image(image, layout, roots.data, [&](const Progress& p, bool force) {
        printer.line(p.fraction, p.detail, force);
    }, nullptr);
    if (!r.ok) return report_error(r.error == ErrorId::None ? ErrorId::E50 : r.error, r.detail);
    return 0;
}

}  // namespace don_host
