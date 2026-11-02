// T-749: the setup must not throw on a user or folder name outside the ANSI code page. On Windows
// std::filesystem::path::string() throws ("No Unicode translation") for such a name; the whole run is under folders named
// with Japanese and Cyrillic characters and goes through discover, disc_image, disc_manifest, storage, prepare_engine,
// startup, module_front and the process argv. The images are synthetic; no game file is read. Runs on Linux too (UTF-8
// names are bytes there). Each section is isolated so one failure does not hide the others.
#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>

#include "discover.h"
#include "disc_image.h"
#include "module_build.h"
#include "module_front.h"
#include "module_manifest.h"
#include "path_text.h"
#include "platform/process.h"
#include "prepare_engine.h"
#include "startup.h"
#include "storage.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; }      \
    } while (0)

static void section(const char* name, const std::function<void()>& body) {
    try {
        body();
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << name << ": threw " << e.what() << "\n";
        ++g_fail;
    }
}

static const char8_t kUserU8[] = u8"ユーザー_Пользователь";
static const char8_t kDiscU8[] = u8"ディスク_Диск";
static const char8_t kArgU8[] = u8"引数 \"Ж\" 日本";

static fs::path from_u8(const char8_t* s) { return fs::path(std::u8string(s)); }

// --- a flat ISO9660 image with SYSTEM.CNF in the root (the same layout as test_setup_discover) ---
static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }

static void dir_record(Bytes& d, size_t& off, const std::string& name, uint32_t lba, uint32_t size, bool special = false,
                       uint8_t sp = 0) {
    size_t nl = special ? 1 : name.size();
    size_t len = 33 + nl + ((nl % 2) ? 0 : 1);
    d[off] = uint8_t(len);
    put32(d, off + 2, lba);
    put32(d, off + 10, size);
    d[off + 25] = special ? 2 : 0;
    d[off + 32] = uint8_t(nl);
    if (special) d[off + 33] = sp; else std::memcpy(&d[off + 33], name.data(), nl);
    off += len;
}

static std::string cnf_text() { return "BOOT2 = cdrom0:\\SLPS_256.75;1\r\nVER = 1.00\r\n"; }

static Bytes make_iso() {
    const std::string cnf = cnf_text();
    Bytes iso(20 * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    Bytes root(2048);
    size_t o = 0;
    dir_record(root, o, "", 18, 2048, true, 0);
    std::memcpy(&iso[16 * 2048 + 156], root.data(), 34);
    dir_record(root, o, "", 18, 2048, true, 1);
    dir_record(root, o, "SYSTEM.CNF;1", 19, uint32_t(cnf.size()));
    std::memcpy(&iso[18 * 2048], root.data(), 2048);
    std::memcpy(&iso[19 * 2048], cnf.data(), cnf.size());
    return iso;
}

static void write_file(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

#ifdef _WIN32
static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(s.size() - 1);
    return s;
}
#endif

// The child side of the argv section: exit 0 when the second argument is kArgU8 and the working directory is argv[3].
static int child_main(int argc, char** argv) {
    std::string arg, cwd_expected;
#ifdef _WIN32
    int wn = 0;
    wchar_t** wv = CommandLineToArgvW(GetCommandLineW(), &wn);
    if (!wv || wn < 4) return 2;
    arg = narrow(wv[2]);
    cwd_expected = narrow(wv[3]);
#else
    if (argc < 4) return 2;
    arg = argv[2];
    cwd_expected = argv[3];
#endif
    (void)argc; (void)argv;
    if (arg != std::string(reinterpret_cast<const char*>(kArgU8))) return 3;
    if (path_text(fs::current_path()) != cwd_expected) return 4;
    return 0;
}

static int run_all();

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child_main(argc, argv);
    try {
        return run_all();
    } catch (const std::exception& e) {
        std::cerr << "FAIL setup threw " << e.what() << "\n";
        return 1;
    }
}

static int run_all() {

    const fs::path base = fs::temp_directory_path() / (std::u8string(u8"don_t749_") + kUserU8);
    const fs::path disc_dir = base / from_u8(kDiscU8);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(disc_dir);
    const fs::path iso = disc_dir / (std::u8string(u8"ゲーム_Игра") + u8".iso");
    write_file(iso, make_iso());

    section("path_text", [&] {
        CHECK(path_from_text(path_text(iso)) == iso);
        CHECK(path_text(from_u8(kUserU8)) == std::string(reinterpret_cast<const char*>(kUserU8)));
    });

    section("discover", [&] {
        write_file(disc_dir / (std::u8string(u8"メモ") + u8".日本"), Bytes{'x'});  // extension outside the ANSI page
        DiscoverResult r = discover({base}, std::chrono::seconds(20));
        CHECK(!r.timed_out);
        CHECK(r.found.size() == 1 && !r.found.empty() && r.found[0].filename() == iso.filename());
        CHECK(is_this_game(iso));
    });

    section("disc_image", [&] {
        const fs::path tiny = disc_dir / (std::u8string(u8"小さい") + u8".日本");  // too small: reads the extension
        write_file(tiny, Bytes{'x', 'y'});
        DiscTable table;
        DiscIdentity id = identify_disc(tiny, table, nullptr);
        CHECK(!id.known);
        id = identify_disc(iso, table, nullptr);
        CHECK(!id.known);
        const fs::path out = base / from_u8(u8"出力");
        std::string err;
        CHECK(extract_disc(iso, out, nullptr, err));
        CHECK(fs::exists(out / "SYSTEM.CNF"));
        write_file(out / (std::u8string(u8"ファイル") + u8".txt"), Bytes{'x'});
        id = identify_disc(out, table, nullptr);  // a folder: looks its files up by name
        CHECK(!id.known);
        // the failure message names the folder: its parent is a file
        const fs::path blocker = base / from_u8(u8"ブロック");
        write_file(blocker, Bytes{'x'});
        std::string err2;
        CHECK(!extract_disc(iso, blocker / u8"サブ", nullptr, err2));
        CHECK(!err2.empty());
    });

    section("disc_manifest", [&] {
        const std::string cnf = cnf_text();
        DiscManifest m{"test", "SLPS-00000", {ManifestFile{"SYSTEM.CNF", cnf.size(), sha256_hex(cnf.data(), cnf.size())}}};
        std::string err;
        const fs::path out = base / from_u8(u8"出力2");
        CHECK(extract_disc(iso, out, nullptr, err, m));
        CHECK(fs::exists(out / "SYSTEM.CNF"));
        const fs::path blocker = base / from_u8(u8"ブロック");
        std::string err2;
        CHECK(!extract_disc(iso, blocker / u8"サブ", nullptr, err2, m));
        CHECK(!err2.empty());
    });

    section("storage", [&] {
        const fs::path root = base / from_u8(u8"データ");
        const std::string id = "abcdef0123456789";
        fs::create_directories(root / "game" / from_u8(u8"版"));  // a folder name outside the ANSI page
        fs::create_directories(root / "game" / "jp");
        fs::create_directories(root / "module" / id);
        DonModuleManifest man;
        man.program_version = "t749";
        man.abi_version = 1;
        man.module_id = id;
        man.toolchain_hash = "t";
        man.module_sha256 = "m";
        man.verified = true;
        man.variant = "jp";
        std::string why;
        CHECK(don_write_module_manifest(root / "module" / id / "manifest.json", man, why));
        DonModuleManifest back;
        CHECK(don_read_module_manifest(root / "module" / id / "manifest.json", back, why) && back.module_id == id);
        CHECK(record_prepared_edition(root, "jp", id));
        InstalledModule im = find_installed_module(root, 1);
        CHECK(im.found);
        CHECK(!installed_variants(root).variants.empty());
        (void)storage_sizes(root);
        (void)repair_plan(root);
        (void)free_up_space(root);
    });

    section("prepare_engine", [&] {
        const fs::path dir = base / from_u8(u8"準備");
        fs::create_directories(dir);
        Journal j;
        j.done.push_back({"job", "00"});
        CHECK(write_journal(dir, j));
        Journal back;
        CHECK(read_journal(dir, back) && back.done.size() == 1);
        write_file(dir / "x.bin", Bytes{'a'});
        CHECK(!sha256_of_file(dir / "x.bin").empty());
    });

    section("startup", [&] {
        StartupInputs in;
        in.root = base / from_u8(u8"起動");
        fs::create_directories(in.root / "module" / from_u8(u8"モジュール"));
        in.compiler_exe = in.root / from_u8(u8"コンパイラ");
        in.is_noexec = [](const fs::path&) { return false; };
        in.exe_of_pid = [&](int) { return path_text(in.compiler_exe); };
        in.start_time_of_pid = [](int) { return std::string("1"); };
        in.kill_pid = [](int) { return true; };
        std::ofstream(in.root / "setup.pids") << "1 1\n";
        StartupReport r = check_startup(in);
        (void)r;
    });

    section("module_front", [&] {
        const fs::path game = base / from_u8(u8"ゲーム");
        fs::create_directories(game);
        write_file(game / from_u8(u8"ファイル.irx"), Bytes{'x'});
        std::vector<uint8_t> irx;
        bool found = false;
        (void)find_adxi_irx(game, irx, found);
    });

    // T-751: the module build plan, argv and response file for a work folder named with Japanese and Cyrillic
    // characters, for both module platforms; clang is not run.
    section("module_build", [&] {
        const fs::path gen = base / from_u8(u8"生成");
        const fs::path sdk = base / from_u8(u8"ソフト");
        const fs::path tc = base / from_u8(u8"ツール");
        write_file(gen / (std::u8string(u8"小_") + u8"Ж.cpp"), Bytes(10, 'x'));
        write_file(gen / (std::u8string(u8"大_") + u8"日.cpp"), Bytes(kModuleBigBytes, 'x'));
        fs::create_directories(tc / "lib" / "clang" / "18" / "include");
        std::string err;
        const std::vector<ModuleUnit> units = plan_module_units(gen, err);
        CHECK(units.size() == 2);
        const std::string big = std::string("big_") + reinterpret_cast<const char*>(u8"大_日");
        for (const ModuleUnit& u : units) {
            if (u.name.compare(0, 4, "big_") == 0) CHECK(u.name == big);
        }
        const fs::path work = base / from_u8(u8"作業") / from_u8(u8"Работа");
        for (const ModuleUnit& u : units) {
            const fs::path unity = write_unity_file(u, work / "unity");
            CHECK(!unity.empty());
            std::ifstream in(unity, std::ios::binary);
            std::string line;
            std::getline(in, line);
            CHECK(line.find(path_text(gen)) != std::string::npos);
        }
        std::vector<fs::path> objects;
        for (const ModuleUnit& u : units) objects.push_back(work / "obj" / path_from_text(u.name + ".o"));
        for (const ModulePlatform plat : {ModulePlatform::Linux, ModulePlatform::Windows}) {
            ModulePaths mp;
            mp.gen = gen;
            mp.sdk = sdk;
            mp.toolchain = tc;
            mp.platform = plat;
            const std::vector<std::string> cc = module_compile_argv(mp, objects[0], objects[1]);
            const std::string sdk_text = path_text(sdk);
            bool has_prefix_map = false;
            for (const std::string& a : cc) has_prefix_map |= a == "-ffile-prefix-map=" + sdk_text + "=.";
            CHECK(has_prefix_map);
            const fs::path so = work / "link-out";
            const fs::path rsp = work / "link.rsp";
            std::vector<std::string> argv;
            std::string why;
            CHECK(prepare_module_link(mp, objects, so, rsp, argv, why));
            CHECK(!argv.empty() && argv[0].find(path_text(tc)) == 0);
            if (plat == ModulePlatform::Windows) {
                std::ifstream in(rsp, std::ios::binary);
                std::ostringstream t;
                t << in.rdbuf();
                CHECK(t.str() == module_link_response_text(objects));
                CHECK(t.str().find(path_text(objects[0])) != std::string::npos);
            }
        }
        const fs::path e = module_failure_err_path("cc:" + path_text(from_u8(u8"ユニット")), work, work / "x");
        CHECK(e == work / "obj" / path_from_text(path_text(from_u8(u8"ユニット")) + ".o.err"));
    });

    section("process_argv", [&] {
        // the test program copied into a folder with a non-ANSI name, run there with a non-ANSI argument
        const fs::path run_dir = base / from_u8(u8"実行");
        fs::create_directories(run_dir);
#ifdef _WIN32
        wchar_t self_w[MAX_PATH * 4];
        GetModuleFileNameW(nullptr, self_w, DWORD(sizeof self_w / sizeof self_w[0]));
        const fs::path self(self_w);
#else
        const fs::path self = fs::read_symlink("/proc/self/exe");
#endif
        fs::path name = from_u8(u8"子供");
        name += self.extension();
        const fs::path copy = run_dir / name;
        fs::copy_file(self, copy, fs::copy_options::overwrite_existing);
#ifdef _WIN32
        for (const char* dll : {"z.dll"})  // the DLLs that sit beside the test programs
            if (fs::exists(self.parent_path() / dll)) fs::copy_file(self.parent_path() / dll, run_dir / dll, fs::copy_options::overwrite_existing);
#endif
        SpawnOptions o;
        o.argv = {path_text(copy), "--child", std::string(reinterpret_cast<const char*>(kArgU8)), path_text(run_dir)};
        o.cwd = path_text(run_dir);
        o.timeout_ms = 30000;
        Process p;
        std::string err;
        CHECK(spawn(o, p, err));
        ExitStatus st = wait(p);
        CHECK(st.exited && st.code == 0);
        if (!(st.exited && st.code == 0)) std::cerr << "child exit " << st.code << " " << err << "\n";
    });

    std::cout << (g_fail ? "FAILED" : "all passed") << "\n";
    return g_fail ? 1 : 0;
}
