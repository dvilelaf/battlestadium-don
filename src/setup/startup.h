// Startup integrity and recovery (T-646): one check of the user folder and of the program's own tools, run at every
// start before any setup screen. It says what the screens must do; it removes nothing (E80 lists, storage.cpp removes)
// and never reads, changes or deletes anything under memcards/.
//   E33 user folder on a `noexec` filesystem      E75 corrupt setup.json: rebuilt from the module manifests
//   E50 bundled toolchain hash differs            E76 a part is missing (game/<v>/, a module): redo only that
//   E73 compilers left running: killed            E77 files differ from their manifest: REPAIR is offered
//   E80 modules unused for 30 days, newest two kept
// Layout and manifests: src/setup/storage.h. The journal is setup.json (prepare_engine.h); the pids of the compilers
// the preparation started are recorded beside it in setup.pids, one "<pid> <start_time>" line each (start_time is
// the process start time of the platform layer, platform/pid_check.h: field 22 of /proc/<pid>/stat on Linux,
// GetProcessTimes on Windows, so a pid reused by another process does not count; the journal reader is strict,
// so they cannot ride inside setup.json).
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup_types.h"

namespace don_setup {

struct StartupInputs {
    std::filesystem::path root;           // the user folder (bulk root)
    std::filesystem::path toolchain_dir;  // build/toolchain/<platform>; empty: E50 is not checked
    std::filesystem::path compiler_exe;   // the compiler the preparation starts; a recorded pid is ours if its exe is this
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    int unused_days = 30;                 // E80: a module not used for this long may go ...
    size_t keep_newest = 2;               // ... but the newest this many always stay

    // Injectable system calls; null means the real one.
    std::function<bool(const std::filesystem::path&)> is_noexec;  // statvfs ST_NOEXEC of the folder holding the path
    std::function<std::string(int)> exe_of_pid;                   // platform::exe_of_pid (pid_check.h), empty when the pid is gone
    std::function<std::string(int)> start_time_of_pid;            // platform::start_time_of_pid, empty when the pid is gone
    std::function<bool(int)> kill_pid;                            // platform::kill_pid (SIGKILL / TerminateProcess)
};

struct StartupReport {
    bool noexec = false;                      // E33
    bool toolchain_ok = true;                 // E50 when false (also when TOOLCHAIN.sha256 is missing)
    std::vector<int> killed_pids;             // E73
    bool journal_rebuilt = false;             // E75; the corrupt file is kept as setup.json.corrupt[.N]
    std::vector<std::string> missing;         // E76: "game/<v>", "module for <v>", "module/<id>/libdon_game.so"
    std::vector<std::string> changed;         // E77: modules whose library hash differs from their manifest
    bool repair_offered = false;              // E77 (and E76 parts that need REPAIR)
    std::vector<std::string> removable_modules;  // E80: ids, oldest first; the caller removes them
    std::vector<ErrorId> errors;              // E33, E50, E73, E75, E76, E77, E80 that apply, in that order
};

StartupReport check_startup(const StartupInputs& in);

// Appends "<pid> <start_time>" to root/setup.pids for a compiler the preparation just started (the prepare engine calls
// it). False, with nothing written, when the process is already gone or the file cannot be written.
bool record_compiler_pid(const std::filesystem::path& root, int pid);

}  // namespace don_setup
