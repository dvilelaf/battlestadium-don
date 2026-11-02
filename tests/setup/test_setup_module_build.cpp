// Module build (T-690): unit grouping, argv shape, module id, manifest round trip, and a whole build against a fake
// toolchain (shell scripts). With --gen --sdk --toolchain --out [--jobs N] [--variant V] it instead builds the real
// module through the same code (the "real build" run); without them that part is skipped with a message.
#include "module_build.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include "module_manifest.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static fs::path g_dir;

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static void make_gen(const fs::path& gen, int small, int big) {
    for (int i = 0; i < small; ++i) {
        char n[32];
        std::snprintf(n, sizeof n, "f%03d.cpp", i);
        write(gen / n, "int f" + std::to_string(i) + "(){return 0;}\n");
    }
    for (int i = 0; i < big; ++i) write(gen / ("zbig" + std::to_string(i) + ".cpp"), std::string(kModuleBigBytes, ' '));
    write(gen / "notes.txt", "ignored");
}

static void test_units() {
    fs::path gen = g_dir / "units";
    fs::remove_all(gen);
    make_gen(gen, 70, 2);  // 70 small -> 32 + 32 + 6, two big alone
    std::string err;
    auto u = plan_module_units(gen, err);
    CHECK(u.size() == 5);
    if (u.size() == 5) {
        CHECK(u[0].name == "batch_00000" && u[0].files.size() == 32 && u[0].files[0].filename() == "f000.cpp");
        CHECK(u[1].name == "batch_00001" && u[1].files.size() == 32 && u[1].files[0].filename() == "f032.cpp");
        CHECK(u[2].name == "batch_00002" && u[2].files.size() == 6);
        CHECK(u[3].name == "big_zbig0" && u[3].files.size() == 1 && u[4].name == "big_zbig1");
    }
    fs::path unity = write_unity_file(u[0], g_dir / "units_out");
    std::ifstream in(unity);
    std::string line;
    std::getline(in, line);
    CHECK(line == "#include \"" + (gen / "f000.cpp").string() + "\"");
    fs::path empty = g_dir / "units_empty";
    fs::create_directories(empty);
    CHECK(plan_module_units(empty, err).empty() && !err.empty());
}

// Argv strings use the host's separators; the expectations are written with '/'.
static std::string fwd(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}
static void fwd(std::vector<std::string>& v) { for (std::string& s : v) s = fwd(s); }

static void test_argv_and_id() {
    ModulePaths p{"/g", "/s", "/t", ModulePlatform::Linux};   // the Linux argv, whatever the host
    fs::create_directories(g_dir / "tc_argv/lib/clang/17/include");
    p.toolchain = g_dir / "tc_argv";
    auto a = module_compile_argv(p, "/u/x.cpp", "/o/x.o");
    fwd(a);
    CHECK(a.front() == fwd((p.toolchain / "bin/clang++").string()));
    auto has = [&](const std::string& s) { return std::find(a.begin(), a.end(), s) != a.end(); };
    CHECK(has("-std=gnu++20") && has("-nostdinc") && has("-ffp-contract=off") && has("-march=x86-64-v3") &&
          has("-DDON_MODULE_IOP") && has("-O2") && has("-fvisibility=hidden"));
    CHECK(has("/s/game_module/iop_shim") && has("/s/inc") && has("/s/host") && has("/g"));
    CHECK(a[a.size() - 4] == "-c" && a[a.size() - 3] == "/u/x.cpp" && a.back() == "/o/x.o");
    CHECK(has(fwd((p.toolchain / "lib/clang/17/include").string())));
    auto l = module_link_argv(p, {"/o/a.o", "/o/b.o"}, "/o/out.so");
    fwd(l);
    CHECK(l.front() == fwd((p.toolchain / "bin/ld.lld").string()) && l.back() == "/o/b.o");
    CHECK(std::find(l.begin(), l.end(), "--eh-frame-hdr") != l.end() && std::find(l.begin(), l.end(), "-shared") != l.end());
    std::string id = module_id_of("aa", "bb", 1);
    CHECK(id.size() == 16 && id == module_id_of("aa", "bb", 1) && id != module_id_of("aa", "bb", 2) &&
          id != module_id_of("ab", "bb", 1));
}

static void test_manifest_roundtrip() {
    DonModuleManifest m;
    m.program_version = "1.0.0";
    m.abi_version = 3;
    m.module_id = "0123456789abcdef";
    m.toolchain_hash = "tc\"q";
    m.module_sha256 = "ff";
    m.verified = true;
    m.variant = "en";
    std::string err;
    fs::path f = g_dir / "manifest.json";
    CHECK(don_write_module_manifest(f.string(), m, err));
    DonModuleManifest back;
    CHECK(don_read_module_manifest(f.string(), back, err));
    CHECK(back.program_version == "1.0.0" && back.abi_version == 3 && back.module_id == m.module_id &&
          back.toolchain_hash == m.toolchain_hash && back.module_sha256 == "ff" && back.verified && back.variant == "en");
    CHECK(!don_write_module_manifest((g_dir / "no/such/dir/m.json").string(), m, err));
}

// Fake clang++/ld.lld: write the file named after -o; the compiler fails on a unity file that includes zbroken.cpp.
static void make_fake_toolchain(const fs::path& tc) {
    const std::string body =
        "#!/bin/sh\nout=\nprev=\nsrc=\nfor a in \"$@\"; do [ \"$prev\" = -o ] && out=$a; case $a in *.cpp) src=$a;; esac; prev=$a; done\n"
        "if [ -n \"$src\" ] && grep -q zbroken \"$src\" 2>/dev/null; then echo 'boom: BROKEN' >&2; exit 1; fi\n"
        "echo fake > \"$out\"\n";
    write(tc / "bin/clang++", body);
    write(tc / "bin/ld.lld", body);
    fs::permissions(tc / "bin/clang++", fs::perms::owner_all);
    fs::permissions(tc / "bin/ld.lld", fs::perms::owner_all);
    fs::create_directories(tc / "lib/clang/17/include");
    write(tc / "TOOLCHAIN.sha256", "abc123  linux\n");
}

static void make_sdk(const fs::path& sdk) {
    write(sdk / "host/game_module.h", "constexpr uint32_t kDonGameModuleAbiVersion = 7;\n");
    write(sdk / "game_module/module_entry.cpp", "int entry;\n");
}

static void test_fake_build() {
    fs::path root = g_dir / "fake_root", gen = g_dir / "fake_gen", tc = g_dir / "fake_tc", sdk = g_dir / "fake_sdk";
    for (auto& d : {root, gen, tc, sdk}) fs::remove_all(d);
    make_fake_toolchain(tc);
    make_sdk(sdk);
    make_gen(gen, 40, 0);
    ModuleBuildOptions o;
    o.paths = {gen, sdk, tc};
    o.root = root;
    o.variant = "en-jpvoice";
    o.program_version = "9.9";
    o.max_jobs = 2;
    int ticks = 0;
    o.on_progress = [&](const Progress&) { ++ticks; };
    ModuleBuildResult r = build_module(o);
    CHECK(r.ok && r.module_id.size() == 16);
    DonModuleManifest m;
    std::string err;
    CHECK(don_read_module_manifest((root / "module" / r.module_id / "manifest.json").string(), m, err));
    CHECK(m.verified && m.abi_version == 7 && m.toolchain_hash == "abc123" && m.variant == "en-jpvoice" &&
          m.program_version == "9.9" && m.module_id == r.module_id && m.module_sha256.size() == 64);
    CHECK(fs::exists(root / "module" / r.module_id / "libdon_game.so") && !fs::exists(root / "module" / (r.module_id + ".tmp")));
    CHECK(ticks > 0);
    // The same tree again: the finished module is left alone.
    CHECK(build_module(o).ok);

    // A failing unit stops the build and keeps its stderr in a file.
    fs::path gen2 = g_dir / "fake_gen2", root2 = g_dir / "fake_root2";
    fs::remove_all(gen2);
    fs::remove_all(root2);
    make_gen(gen2, 40, 0);
    write(gen2 / "zbroken.cpp", "x\n");
    o.paths.gen = gen2;
    o.root = root2;
    ModuleBuildResult bad = build_module(o);
    CHECK(!bad.ok && bad.error.find("batch_00001") != std::string::npos);
    bool kept = false;
    for (auto& e : fs::recursive_directory_iterator(root2)) {
        if (e.path().extension() != ".err") continue;
        std::ifstream in(e.path());
        std::string t((std::istreambuf_iterator<char>(in)), {});
        kept = kept || t.find("boom") != std::string::npos;
    }
    CHECK(kept);
    CHECK(!fs::exists(root2 / "module" / bad.module_id));
}

// A stop request during the build (T-649): within ~2 s no compiler is left running and no new unit starts; the journal
// keeps the finished units, and the next build compiles only the rest.
static void test_stop_and_resume() {
    fs::path root = g_dir / "stop_root", gen = g_dir / "stop_gen", tc = g_dir / "stop_tc", sdk = g_dir / "stop_sdk";
    for (auto& d : {root, gen, tc, sdk}) fs::remove_all(d);
    make_fake_toolchain(tc);
    make_sdk(sdk);
    make_gen(gen, 320, 0);
    const std::string slow =   // a compiler that takes a while and has a child of its own, like a real driver
        "#!/bin/sh\nout=\nprev=\nfor a in \"$@\"; do [ \"$prev\" = -o ] && out=$a; prev=$a; done\nsleep 0.2\necho fake > \"$out\"\n";
    write(tc / "bin/clang++", slow);
    write(tc / "bin/ld.lld", slow);
    ModuleBuildOptions o;
    o.paths = {gen, sdk, tc};
    o.root = root;
    o.variant = "en";
    o.program_version = "9.9";
    o.max_jobs = 2;
    std::atomic<bool> stop{false};
    o.stop = &stop;
    o.on_progress = [&](const Progress& p) { if (p.parts_done >= 3) stop = true; };
    const auto t0 = std::chrono::steady_clock::now();
    ModuleBuildResult first = build_module(o);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!first.ok && first.stopped && first.compiled >= 3 && took < 5.0);
    CHECK(!fs::exists(root / "module" / first.module_id));
    // The compilers that were running are gone: a hanging one would not return within the limit of the next call.
    stop = false;
    o.on_progress = nullptr;
    ModuleBuildResult second = build_module(o);
    CHECK(second.ok && !second.stopped && second.skipped >= first.compiled && second.skipped + second.compiled == 12);

    // A compiler that never ends is killed by the stop, process group and all, and the build returns promptly.
    fs::path root2 = g_dir / "hang_root";
    fs::remove_all(root2);
    const std::string hang = "#!/bin/sh\nsleep 60\n";
    write(tc / "bin/clang++", hang);
    write(tc / "bin/ld.lld", hang);
    o.root = root2;
    stop = false;
    int polls = 0;
    o.on_progress = [&](const Progress&) { if (++polls == 5) stop = true; };
    const auto t1 = std::chrono::steady_clock::now();
    ModuleBuildResult killed = build_module(o);
    CHECK(killed.stopped && std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() < 2.0);
}

static int real_build(int argc, char** argv) {
    ModuleBuildOptions o;
    o.variant = "en-jpvoice";
    o.program_version = "0.0-dev";
    fs::path out;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--gen") o.paths.gen = argv[i + 1];
        else if (k == "--sdk") o.paths.sdk = argv[i + 1];
        else if (k == "--toolchain") o.paths.toolchain = argv[i + 1];
        else if (k == "--out") o.root = argv[i + 1];
        else if (k == "--jobs") o.max_jobs = size_t(std::stoul(argv[i + 1]));
        else if (k == "--variant") o.variant = argv[i + 1];
    }
    int last = -1;
    o.on_progress = [&](const Progress& p) {
        int pct = int(p.fraction * 100);
        if (pct / 5 != last) { last = pct / 5; std::cerr << "[build] " << pct << "% " << p.parts_done << "/" << p.parts_total << "\n"; }
    };
    ModuleBuildResult r = build_module(o);
    if (!r.ok) { std::cerr << "build failed: " << r.error << "\n"; return 1; }
    std::cout << "module " << r.module_id << " in " << r.module_dir.string() << "\n";
    return 0;
}

int main(int argc, char** argv) {
    g_dir = fs::temp_directory_path() / "don_module_build_test";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    test_units();
    test_argv_and_id();
    test_manifest_roundtrip();
#ifndef _WIN32   // the fake toolchains are /bin/sh scripts; the Windows toolchain is run for real by the packaged build
    test_fake_build();
    test_stop_and_resume();
#else
    std::cerr << "module_build: fake-toolchain builds skipped, the fake compilers are shell scripts\n";
#endif
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    if (argc >= 9) return real_build(argc, argv);
    std::cout << "real build skipped: pass --gen --sdk --toolchain --out\n";
    return 0;
}
