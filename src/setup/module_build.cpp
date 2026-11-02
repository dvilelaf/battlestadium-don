#include "module_build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "disc_image.h"
#include "module_manifest.h"
#include "path_text.h"
#include "platform/process.h"
#include "prepare_engine.h"
#include "tree_hash.h"

namespace fs = std::filesystem;

namespace don_setup {

namespace {

// make_ninja.py FLAGS, one argument each ("-isystem" and "-I" take the next entry).
const char* const kCompileFlags[] = {
    "-std=gnu++20", "-nostdinc", "-nostdinc++", "-ffreestanding", "-fno-rtti", "-fno-threadsafe-statics",
    "-fvisibility=hidden", "-fno-stack-protector", "-ffp-contract=off", "-march=x86-64-v3", "-fno-crash-diagnostics",
    "-DNDEBUG", "-DDON_GAME_MODULE_BUILD", "-DDON_MODULE_IOP"};

// Optimisation level (make_ninja.py GAME_OPT / REGISTRATION_OPT, the same values). Measured under T-752 on the Linux
// developer recipe, 4-player ougi fight (tools/run/perf_fight.py, 7800 blanks, same state hash at every level), module
// build at -j4 on 16 threads, game thread CPU per blank / module build time: -O1 10.93 ms / 637 s, -O2 10.55 ms / 776 s,
// -O3 11.08 ms / 785 s. -O2 is the fastest game thread. The registration unit holds register_functions.cpp, a 7 MB table
// that runs no game instruction: it stays at -O1 (the monolithic Release build did the same, CMakeLists.txt).
const char* const kGameOpt = "-O2";
const char* const kRegistrationOpt = "-O1";
const char* const kRegistrationUnit = "big_register_functions";
const char* const kLinkFlags[] = {"-shared", "--hash-style=gnu", "-z", "now", "--no-undefined-version", "--eh-frame-hdr"};
// lld-link, as make_ninja.py --target windows: a DLL with no entry point and no default libraries, hence no imports at all.
// /Brepro: no timestamp in the header, so that the same inputs give the same bytes.
const char* const kWindowsLinkFlags[] = {"/DLL", "/NOENTRY", "/NODEFAULTLIB", "/MACHINE:X64", "/Brepro"};

constexpr size_t kStderrKeep = 1u << 20;
constexpr auto kPollSleep = std::chrono::milliseconds(20);
// The link is a few seconds against minutes of compiling: it weighs this share of the compile bytes.
constexpr double kLinkShare = 0.05;

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream t;
    t << in.rdbuf();
    return t.str();
}

fs::path tool(const fs::path& toolchain, const char* name) { return toolchain / "bin" / name; }
bool windows(const ModulePaths& p) { return p.platform == ModulePlatform::Windows; }

// Runs each job as a child process; the compiler's stderr goes to <output>.err, kept when the job fails.
class ProcessRunner : public JobRunner {
public:
    std::map<std::string, std::vector<std::string>> argv;  // by job id

    bool run(const Job& job, Speed speed) override {
        if (!start(job, speed)) return false;
        State s;
        while ((s = check(job)) == State::Running) std::this_thread::sleep_for(kPollSleep);
        return s == State::Done;
    }
    bool async() const override { return true; }
    bool start(const Job& job, Speed speed) override {
        SpawnOptions so;
        so.argv = argv.at(job.id);
        so.env = {"LC_ALL=C"};
        so.tmpdir = path_text(tmp);
        so.nice = speed == Speed::Gentle ? 10 : 0;
        so.stderr_limit = kStderrKeep;
        Process p;
        std::string why;
        if (!spawn(so, p, why)) {
            write_err(job, why);
            return false;
        }
        running_[job.id] = p;
        return true;
    }
    State check(const Job& job) override {
        auto it = running_.find(job.id);
        if (it == running_.end()) return State::Failed;
        ExitStatus st;
        if (!poll(it->second, st)) return State::Running;
        const bool ok = st.exited && st.code == 0 && fs::exists(job.output);
        const std::string err = it->second.stderr_tail;
        running_.erase(it);
        std::error_code ec;
        if (ok && err.empty()) fs::remove(err_path(job), ec);
        else write_err(job, err.empty() ? "exit code " + std::to_string(st.code) + ", signal " + std::to_string(st.signal) : err);
        return ok ? State::Done : State::Failed;
    }
    // A stop request: the compiler's whole process group dies (it may have spawned children) and is reaped.
    void cancel(const Job& job) override {
        auto it = running_.find(job.id);
        if (it == running_.end()) return;
        kill_group(it->second);
        wait(it->second);
        running_.erase(it);
        std::error_code ec;
        fs::remove(job.output, ec);
        fs::remove(err_path(job), ec);
    }
    fs::path tmp;
    static fs::path err_path(const Job& j) { fs::path p = j.output; return p += ".err"; }

private:
    void write_err(const Job& j, const std::string& text) { std::ofstream(err_path(j), std::ios::binary) << text; }
    std::map<std::string, Process> running_;
};

uint32_t abi_version_of(const fs::path& sdk) {
    // The loader (src/host/game_module.cpp) compares the manifest with this constant, not with DON_ABI_VERSION of
    // the guest header (the layout of the guest side only).
    const std::string t = slurp(sdk / "host" / "game_module.h");
    const std::string key = "kDonGameModuleAbiVersion = ";
    const size_t at = t.find(key);
    return at == std::string::npos ? 0 : uint32_t(std::strtoul(t.c_str() + at + key.size(), nullptr, 10));
}

std::string first_word(const std::string& s) { return s.substr(0, s.find_first_of(" \t\r\n")); }

}  // namespace

std::vector<ModuleUnit> plan_module_units(const fs::path& gen, std::string& error) {
    std::vector<std::pair<std::string, uintmax_t>> sources;
    std::error_code ec;
    for (fs::directory_iterator it(gen, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".cpp" && it->is_regular_file(ec))
            sources.push_back({path_text(it->path().filename()), it->file_size(ec)});
    std::sort(sources.begin(), sources.end());
    std::vector<ModuleUnit> units, bigs;
    ModuleUnit batch;
    size_t batches = 0;
    auto flush = [&] {
        if (batch.files.empty()) return;
        char name[32];
        std::snprintf(name, sizeof name, "batch_%05zu", batches++);
        batch.name = name;
        units.push_back(std::move(batch));
        batch = ModuleUnit();
    };
    for (const auto& s : sources) {
        if (s.second >= kModuleBigBytes) {
            bigs.push_back({"big_" + path_text(path_from_text(s.first).stem()), {gen / path_from_text(s.first)}, s.second});
            continue;
        }
        batch.files.push_back(gen / path_from_text(s.first));
        batch.bytes += s.second;
        if (batch.files.size() == kModuleBatch) flush();
    }
    flush();
    units.insert(units.end(), bigs.begin(), bigs.end());
    if (units.empty()) error = "no .cpp source in " + path_text(gen);
    return units;
}

fs::path write_unity_file(const ModuleUnit& unit, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / path_from_text(unit.name + ".cpp");
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    for (const auto& file : unit.files) f << "#include \"" << path_text(file) << "\"\n";
    f.flush();
    return f ? out : fs::path();
}

fs::path module_resource_include(const fs::path& toolchain) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(toolchain / "lib" / "clang", ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_directory(it->path() / "include", ec)) found.push_back(it->path() / "include");
    std::sort(found.begin(), found.end());
    return found.empty() ? fs::path() : found.front();
}

std::vector<std::string> module_compile_argv(const ModulePaths& p, const fs::path& source, const fs::path& object) {
    std::vector<std::string> a = {path_text(tool(p.toolchain, compiler_file_name(p.platform)))};
    if (windows(p)) a.push_back(std::string("--target=") + module_target_triple(p.platform));
    for (const char* f : kCompileFlags) a.push_back(f);
    a.push_back(path_text(source.stem()) == kRegistrationUnit ? kRegistrationOpt : kGameOpt);
    a.insert(a.end(), {"-isystem", path_text(module_resource_include(p.toolchain))});
    for (const fs::path& inc : {p.gen, p.sdk / "game_module" / "iop_shim", p.sdk / "inc", p.sdk / "game_module",
                                p.sdk / "host"})
        a.insert(a.end(), {"-I", path_text(inc)});
    a.push_back("-ffile-prefix-map=" + path_text(p.sdk) + "=.");
    a.insert(a.end(), {"-c", path_text(source), "-o", path_text(object)});
    return a;
}

std::vector<std::string> module_link_argv(const ModulePaths& p, const std::vector<fs::path>& objects,
                                          const fs::path& output) {
    std::vector<std::string> a = {path_text(tool(p.toolchain, linker_file_name(p.platform)))};
    if (windows(p)) {
        for (const char* f : kWindowsLinkFlags) a.push_back(f);
        a.push_back("/OUT:" + path_text(output));
    } else {
        for (const char* f : kLinkFlags) a.push_back(f);
        a.insert(a.end(), {"-o", path_text(output)});
    }
    for (const auto& o : objects) a.push_back(path_text(o));
    return a;
}

std::string module_link_response_text(const std::vector<fs::path>& objects) {
    std::string t;
    for (const auto& o : objects) {
        const std::string s = path_text(o);
        t += '"';
        size_t slashes = 0;
        for (const char c : s) {
            if (c == '\\') { ++slashes; t += c; continue; }
            if (c == '"') t.append(slashes + 1, '\\');  // the pending backslashes double, then the quote is escaped
            slashes = 0;
            t += c;
        }
        t.append(slashes, '\\');  // backslashes before the closing quote double too
        t += "\"\n";
    }
    return t;
}

bool prepare_module_link(const ModulePaths& p, const std::vector<fs::path>& objects, const fs::path& output,
                         const fs::path& rsp, std::vector<std::string>& argv, std::string& why) {
    if (!windows(p)) {
        argv = module_link_argv(p, objects, output);
        return true;
    }
    argv = module_link_argv(p, {}, output);
    {
        std::ofstream f(rsp, std::ios::binary);
        f << module_link_response_text(objects);
        if (!f) { why = "cannot write the link response file " + path_text(rsp); return false; }
    }
    argv.push_back("@" + path_text(rsp));
    return true;
}

fs::path module_failure_err_path(const std::string& detail, const fs::path& work, const fs::path& module_file) {
    const size_t at = detail.find("cc:");
    if (at == std::string::npos) { fs::path e = module_file; return e += ".err"; }
    return work / "obj" / path_from_text(detail.substr(at + 3) + ".o.err");
}

std::string module_id_of(const std::string& tree_hash, const std::string& toolchain_hash, uint32_t abi_version) {
    // The compile recipe is part of the id: a module built with other flags is "built for another version" and is rebuilt.
    const std::string text = tree_hash + "\n" + toolchain_hash + "\n" + std::to_string(abi_version) + "\n" +
                             "opt=" + kGameOpt + "," + kRegistrationUnit + "=" + kRegistrationOpt + "\n";
    return sha256_hex(text.data(), text.size()).substr(0, 16);
}

ModuleBuildResult build_module(const ModuleBuildOptions& o) {
    ModuleBuildResult r;
    ModulePaths paths = o.paths;
    std::string why;
    const std::string tree = tree_hash(paths.gen, why);
    if (tree.empty()) { r.error = why; return r; }
    const std::string tc = first_word(slurp(paths.toolchain / "TOOLCHAIN.sha256"));
    const uint32_t abi = abi_version_of(paths.sdk);
    if (tc.empty()) { r.error = "toolchain has no TOOLCHAIN.sha256: " + path_text(paths.toolchain); return r; }
    if (abi == 0) { r.error = "no kDonGameModuleAbiVersion in the sdk's host/game_module.h: " + path_text(paths.sdk); return r; }
    if (module_resource_include(paths.toolchain).empty()) { r.error = "toolchain has no lib/clang/*/include"; return r; }
    r.module_id = module_id_of(tree, tc, abi);
    r.module_dir = o.root / "module" / r.module_id;
    DonModuleManifest have;
    if (don_read_module_manifest(r.module_dir / "manifest.json", have, why) && have.verified) {
        r.ok = true;
        return r;
    }

    const std::vector<ModuleUnit> units = plan_module_units(paths.gen, why);
    if (units.empty()) { r.error = why; return r; }
    const fs::path work = o.root / "build" / "tmp" / r.module_id;
    std::error_code ec;
    fs::create_directories(work / "obj", ec);
    ProcessRunner runner;
    runner.tmp = work / "tmp";
    fs::create_directories(runner.tmp, ec);

    std::vector<Job> jobs;
    std::vector<fs::path> objects;
    double total = 0;
    auto add = [&](const std::string& name, const fs::path& source, double weight) {
        const fs::path obj = work / "obj" / path_from_text(name + ".o");
        jobs.push_back({"cc:" + name, weight, obj, Stage::Build, false});
        runner.argv[jobs.back().id] = module_compile_argv(paths, source, obj);
        objects.push_back(obj);
        total += weight;
    };
    add("entry", paths.sdk / "game_module" / "module_entry.cpp", double(fs::file_size(paths.sdk / "game_module" / "module_entry.cpp", ec)));
    if (windows(paths))  // __chkstk and _fltused, which the CRT would give (src/game_module/win_runtime.cpp)
        add("win_runtime", paths.sdk / "game_module" / "win_runtime.cpp", double(fs::file_size(paths.sdk / "game_module" / "win_runtime.cpp", ec)));
    for (const auto& u : units) {
        const fs::path unity = write_unity_file(u, work / "unity");
        if (unity.empty()) { r.error = "cannot write " + path_text(work / "unity" / path_from_text(u.name)); return r; }
        add(u.name, unity, double(u.bytes));
    }
    const fs::path so = work / module_file_name(paths.platform);
    jobs.push_back({"link", total * kLinkShare, so, Stage::Build, true});
    if (!prepare_module_link(paths, objects, so, work / "link.rsp", runner.argv["link"], why)) { r.error = why; return r; }

    size_t cores = std::max(1u, std::thread::hardware_concurrency());
    if (o.max_jobs > 0) cores = std::min(cores, o.max_jobs);
    PrepareEngine engine(work, jobs, runner);
    engine.set_max_parallel(cores);
    auto report = [&] {
        r.skipped = engine.proven();
        r.compiled = engine.ran();
        if (std::getenv("DON_TRACE_SETUP") != nullptr)
            std::fprintf(stderr, "[don] setup build: %zu units skipped (already done), %zu compiled, %zu total\n", r.skipped,
                         r.compiled, jobs.size());
    };
    while (!engine.finished() && !(engine.failed() && engine.idle())) {
        if (o.stop != nullptr && o.stop->load()) {
            engine.halt();
            r.stopped = true;
            report();
            return r;
        }
        const Progress p = engine.poll();
        if (o.on_progress) o.on_progress(p);
        if (!engine.finished()) std::this_thread::sleep_for(kPollSleep);
    }
    report();
    if (engine.failed()) {
        const Progress p = engine.poll();
        r.error = p.detail + "; stderr in " + path_text(module_failure_err_path(p.detail, work, so));
        return r;
    }

    DonModuleManifest m;
    m.program_version = o.program_version;
    m.abi_version = abi;
    m.module_id = r.module_id;
    m.toolchain_hash = tc;
    m.module_sha256 = sha256_of_file(so);
    m.verified = !m.module_sha256.empty();
    m.variant = o.variant;
    const fs::path tmp_dir = o.root / "module" / (r.module_id + ".tmp");
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);
    fs::copy_file(so, tmp_dir / module_file_name(paths.platform), fs::copy_options::overwrite_existing, ec);
    if (ec) { r.error = "cannot copy the module: " + ec.message(); return r; }
    if (!don_write_module_manifest(tmp_dir / "manifest.json", m, why)) { r.error = why; return r; }
    fs::remove_all(r.module_dir, ec);
    fs::rename(tmp_dir, r.module_dir, ec);
    if (ec) { r.error = "cannot move the module into place: " + ec.message(); return r; }
    fs::remove_all(work, ec);
    r.ok = true;
    return r;
}

}  // namespace don_setup
