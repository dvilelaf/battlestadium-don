// Startup integrity and recovery (T-646). See startup.h.
#include "startup.h"
#include "path_text.h"

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>

#include "disc_image.h"
#include "module_manifest.h"
#include "module_platform.h"
#include "platform/pid_check.h"
#include "prepare_engine.h"
#include "storage.h"

namespace fs = std::filesystem;

namespace don_setup {
namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool real_noexec(const fs::path& p) {
#if defined(ST_NOEXEC) && !defined(_WIN32)
    struct statvfs sv;
    return statvfs(path_text(p).c_str(), &sv) == 0 && (sv.f_flag & ST_NOEXEC) != 0;
#else
    (void)p;
    return false;
#endif
}

// Tree hash of the toolchain, same rule as tools/pipeline/fetch_toolchain.py tree_hash(): one line per file
// ("F <path> <sha256>") or symlink ("L <path> <sha256 of the link text>"), sorted by path, one SHA-256 over them.
// src/setup/tree_hash.h is another rule (no links, binary stream), so it cannot be reused here.
// The stored file is "<digest>  <label>\n": only the first word is the digest (as fetch_toolchain.py reads it).
bool toolchain_matches(const fs::path& dir) {
    std::string want = slurp(dir / "TOOLCHAIN.sha256");
    size_t end = want.find_first_of(" \t\r\n");
    want = want.substr(0, end);
    if (want.empty()) return false;
    std::vector<std::pair<std::string, std::string>> lines;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::none, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::string rel = it->path().lexically_relative(dir).generic_string();
        if (rel == "TOOLCHAIN.sha256") continue;
        std::error_code e2;
        fs::file_status st = fs::symlink_status(it->path(), e2);
        if (fs::is_symlink(st)) {
            std::string t = fs::read_symlink(it->path(), e2).generic_string();
            if (!e2) lines.push_back({rel, "L " + rel + " " + sha256_hex(t.data(), t.size()) + "\n"});
        } else if (fs::is_regular_file(st)) {
            lines.push_back({rel, "F " + rel + " " + sha256_of_file(it->path()) + "\n"});
        }
    }
    if (ec) return false;
    std::sort(lines.begin(), lines.end());
    std::string all;
    for (auto& l : lines) all += l.second;
    return sha256_hex(all.data(), all.size()) == want;
}

struct ModuleInfo {
    std::string id, variant, sha256;
    fs::file_time_type used;
};

std::vector<ModuleInfo> read_modules(const fs::path& root) {
    std::vector<ModuleInfo> out;
    std::error_code ec;
    fs::path dir = root / "module";
    if (fs::is_symlink(fs::symlink_status(dir, ec))) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string id = path_text(it->path().filename());
        if (id.size() > 4 && id.compare(id.size() - 4, 4, ".tmp") == 0) continue;
        std::error_code e2;
        if (!fs::is_directory(fs::symlink_status(it->path(), e2))) continue;
        fs::path mf = it->path() / "manifest.json";
        if (!fs::is_regular_file(fs::symlink_status(mf, e2))) continue;
        DonModuleManifest man;
        std::string why;
        if (!don_read_module_manifest(mf, man, why)) continue;
        ModuleInfo m{id, man.variant, man.module_sha256, fs::last_write_time(mf, e2)};
        if (!e2 && !m.variant.empty()) out.push_back(m);
    }
    return out;
}

void kill_stale_compilers(const StartupInputs& in, StartupReport& r) {
    std::ifstream f(in.root / "setup.pids");
    auto exe_of = in.exe_of_pid ? in.exe_of_pid : std::function<std::string(int)>(platform::exe_of_pid);
    auto start_of = in.start_time_of_pid ? in.start_time_of_pid : std::function<std::string(int)>(platform::start_time_of_pid);
    auto kill_it = in.kill_pid ? in.kill_pid : std::function<bool(int)>(platform::kill_pid);
    std::string line;
    while (std::getline(f, line)) {
        // "<pid> <start_time>"; a line of the old one-field format has nothing to compare, so it is never ours.
        char* end = nullptr;
        long pid = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str() || pid <= 1 || pid > 0x7fffffff) continue;
        std::string recorded(end);
        recorded.erase(0, recorded.find_first_not_of(' '));
        while (!recorded.empty() && std::isspace(static_cast<unsigned char>(recorded.back()))) recorded.pop_back();
        if (recorded.empty()) continue;
        // Only the process we started: a reused pid has another start time, or another program.
        if (start_of(static_cast<int>(pid)) != recorded) continue;
        std::string exe = exe_of(static_cast<int>(pid));
        if (exe.empty() || in.compiler_exe.empty() || path_from_text(exe) != in.compiler_exe) continue;
        if (kill_it(static_cast<int>(pid))) r.killed_pids.push_back(static_cast<int>(pid));
    }
}

// The corrupt journal is moved aside, never deleted, and a new one lists the modules whose manifests exist.
void rebuild_journal(const StartupInputs& in, const std::vector<ModuleInfo>& mods, StartupReport& r) {
    fs::path bad = in.root / "setup.json.corrupt";
    for (int n = 1; fs::exists(bad); ++n) bad = in.root / ("setup.json.corrupt." + std::to_string(n));
    std::error_code ec;
    fs::rename(in.root / "setup.json", bad, ec);
    if (ec) return;
    Journal j;
    for (const ModuleInfo& m : mods) j.done.push_back({"module/" + m.id, m.sha256});
    if (write_journal(in.root, j)) r.journal_rebuilt = true;
}

}  // namespace

StartupReport check_startup(const StartupInputs& in) {
    StartupReport r;
    auto noexec = in.is_noexec ? in.is_noexec : std::function<bool(const fs::path&)>(real_noexec);
    r.noexec = noexec(in.root);
    if (r.noexec) r.errors.push_back(ErrorId::E33);

    if (!in.toolchain_dir.empty()) {
        r.toolchain_ok = toolchain_matches(in.toolchain_dir);
        if (!r.toolchain_ok) r.errors.push_back(ErrorId::E50);
    }

    kill_stale_compilers(in, r);
    if (!r.killed_pids.empty()) r.errors.push_back(ErrorId::E73);

    std::vector<ModuleInfo> mods = read_modules(in.root);

    Journal j;
    std::error_code ec;
    if (fs::exists(in.root / "setup.json", ec) && !read_journal(in.root, j)) {
        rebuild_journal(in, mods, r);
        if (r.journal_rebuilt) r.errors.push_back(ErrorId::E75);
    }

    // E76: only the missing part is redone.
    for (const ModuleInfo& m : mods) {
        if (!fs::is_directory(in.root / "game" / m.variant, ec)) r.missing.push_back("game/" + m.variant);
        if (!fs::exists(in.root / "module" / m.id / module_file_name(), ec))
            r.missing.push_back("module/" + m.id + "/" + module_file_name());
    }
    for (const RepairItem& it : repair_plan(in.root).items)
        if (it.kind == RepairKind::VariantWithoutModule) r.missing.push_back("module for " + it.name);
    std::sort(r.missing.begin(), r.missing.end());
    r.missing.erase(std::unique(r.missing.begin(), r.missing.end()), r.missing.end());
    if (!r.missing.empty()) r.errors.push_back(ErrorId::E76);

    // E77: a library that is not what its manifest says. Report only.
    for (const ModuleInfo& m : mods) {
        fs::path so = in.root / "module" / m.id / module_file_name();
        if (!fs::exists(so, ec) || m.sha256.empty()) continue;
        if (sha256_of_file(so) != m.sha256) r.changed.push_back(m.id);
    }
    std::sort(r.changed.begin(), r.changed.end());
    if (!r.changed.empty()) {
        r.repair_offered = true;
        r.errors.push_back(ErrorId::E77);
    }

    // E80: unused for `unused_days` (the manifest's last write is the last use), except the newest `keep_newest`.
    std::sort(mods.begin(), mods.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        return a.used != b.used ? a.used < b.used : a.id < b.id;
    });
    auto limit = fs::file_time_type::clock::now() - std::chrono::hours(24 * in.unused_days) +
                 (in.now - std::chrono::system_clock::now());
    size_t removable = mods.size() > in.keep_newest ? mods.size() - in.keep_newest : 0;
    for (size_t i = 0; i < removable; ++i)
        if (mods[i].used < limit) r.removable_modules.push_back(mods[i].id);
    if (!r.removable_modules.empty()) r.errors.push_back(ErrorId::E80);
    return r;
}

bool record_compiler_pid(const fs::path& root, int pid) {
    std::string start = platform::start_time_of_pid(pid);
    if (start.empty()) return false;
    std::ofstream out(root / "setup.pids", std::ios::app);
    out << pid << " " << start << "\n";
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace don_setup
