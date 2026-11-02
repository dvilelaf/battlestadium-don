// Tests of the startup check (T-646). Everything happens in fresh folders under the temp directory; the filesystem and
// process calls are faked where they could touch the real machine. memcards/ is filled with a canary and compared.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "disc_image.h"
#include "startup.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool has(const StartupReport& r, ErrorId e) {
    return std::find(r.errors.begin(), r.errors.end(), e) != r.errors.end();
}

static const std::string kLib = "library bytes";

// A healthy module: library, manifest naming `variant` and the library's real hash. Its manifest is `age_days` old.
static void make_module(const fs::path& root, const std::string& id, const std::string& variant, int age_days,
                        const std::string& sha = "") {
    put_text(root / "module" / id / "libdon_game.so", kLib);
    std::string h = sha.empty() ? sha256_hex(kLib.data(), kLib.size()) : sha;
    put_text(root / "module" / id / "manifest.json",
             "{\"program_version\":\"0.0-test\",\"abi_version\":2,\"module_id\":\"" + id + "\",\"toolchain_hash\":\"tc\","
             "\"module_sha256\":\"" + h + "\",\"verified\":true,\"variant\":\"" + variant + "\"}");
    fs::last_write_time(root / "module" / id / "manifest.json",
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * age_days));
}

static void make_game(const fs::path& root, const std::string& v) { put_text(root / "game" / v / "a.bin", "g"); }

static fs::path fresh(const fs::path& base, const std::string& name) {
    fs::path r = base / name;
    fs::create_directories(r / "memcards");
    put_text(r / "memcards/card1.ps2", "CARD-CANARY");
    return r;
}

static StartupInputs inputs(const fs::path& root) {
    StartupInputs in;
    in.root = root;
    in.is_noexec = [](const fs::path&) { return false; };
    in.exe_of_pid = [](int) { return std::string(); };
    in.kill_pid = [](int) { return false; };
    return in;
}

static void card_intact(const fs::path& root) {
    CHECK(slurp(root / "memcards/card1.ps2") == "CARD-CANARY");
    size_t n = 0;
    for (auto& e : fs::directory_iterator(root / "memcards")) { (void)e; ++n; }
    CHECK(n == 1);
}

static void test_healthy(const fs::path& base) {
    fs::path root = fresh(base, "healthy");
    make_game(root, "us");
    make_module(root, "aa11", "us", 1);
    StartupReport r = check_startup(inputs(root));
    CHECK(r.errors.empty());
    CHECK(!r.noexec && r.toolchain_ok && r.killed_pids.empty() && !r.journal_rebuilt);
    CHECK(r.missing.empty() && r.changed.empty() && !r.repair_offered && r.removable_modules.empty());
    card_intact(root);
}

static void test_e33(const fs::path& base) {
    fs::path root = fresh(base, "e33");
    StartupInputs in = inputs(root);
    fs::path asked;
    in.is_noexec = [&](const fs::path& p) { asked = p; return true; };
    StartupReport r = check_startup(in);
    CHECK(r.noexec && has(r, ErrorId::E33));
    CHECK(asked == root);
    card_intact(root);
}

// The tree-hash rule of tools/pipeline/fetch_toolchain.py: "F <path> <sha256>\n" / "L <path> <sha256 of link text>\n".
static void test_e50(const fs::path& base) {
    fs::path root = fresh(base, "e50"), tc = base / "e50_tc";
    put_text(tc / "bin/clang", "CLANG");
    fs::create_symlink("clang", tc / "bin/clang++");
    std::string list = "F bin/clang " + sha256_hex("CLANG", 5) + "\nL bin/clang++ " + sha256_hex("clang", 5) + "\n";
    put_text(tc / "TOOLCHAIN.sha256", sha256_hex(list.data(), list.size()) + "\n");
    StartupInputs in = inputs(root);
    in.toolchain_dir = tc;
    StartupReport ok = check_startup(in);
    CHECK(ok.toolchain_ok && !has(ok, ErrorId::E50));
    put_text(tc / "bin/clang", "CLANG-TAMPERED");
    StartupReport bad = check_startup(in);
    CHECK(!bad.toolchain_ok && has(bad, ErrorId::E50));
    fs::remove(tc / "TOOLCHAIN.sha256");
    CHECK(!check_startup(in).toolchain_ok);
}

// fetch_toolchain.py writes "<digest>  build/toolchain/linux (tree)\n": the stored file holds a label after the digest.
static void test_e50_label(const fs::path& base) {
    fs::path root = fresh(base, "e50label"), tc = base / "e50label_tc";
    put_text(tc / "bin/clang", "CLANG");
    std::string list = "F bin/clang " + sha256_hex("CLANG", 5) + "\n";
    put_text(tc / "TOOLCHAIN.sha256", sha256_hex(list.data(), list.size()) + "  build/toolchain/linux (tree)\n");
    StartupInputs in = inputs(root);
    in.toolchain_dir = tc;
    CHECK(check_startup(in).toolchain_ok);
}

static pid_t spawn_child(const char* a, const char* b, const char* c = nullptr) {
    pid_t p = fork();
    if (p == 0) { execlp(a, a, b, c, (char*)nullptr); _exit(127); }
    return p;
}

static std::string exe_of(pid_t p) {
    std::error_code ec;
    return fs::read_symlink("/proc/" + std::to_string(p) + "/exe", ec).string();
}

static bool alive(pid_t p) {
    int st;
    return waitpid(p, &st, WNOHANG) == 0;
}

static std::string start_of(pid_t p) {
    std::ifstream in("/proc/" + std::to_string(p) + "/stat");
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::istringstream rest(stat.substr(stat.rfind(')') + 1));
    std::string field;
    for (int i = 3; i <= 22; ++i) rest >> field;  // proc(5): field 22 is starttime
    return field;
}

static void test_e73(const fs::path& base) {
    fs::path root = fresh(base, "e73");
    pid_t ours = spawn_child("sleep", "300"), other = spawn_child("tail", "-f", "/dev/null");
    pid_t reused = spawn_child("sleep", "301"), old_format = spawn_child("sleep", "302");
    usleep(200 * 1000);  // let the children reach exec
    std::string our_exe = exe_of(ours);
    CHECK(!our_exe.empty() && our_exe != exe_of(other) && exe_of(reused) == our_exe && exe_of(old_format) == our_exe);
    CHECK(record_compiler_pid(root, ours));
    CHECK(record_compiler_pid(root, other));
    CHECK(!record_compiler_pid(root, 99999999));  // gone: nothing is written
    CHECK(slurp(root / "setup.pids") == std::to_string(ours) + " " + start_of(ours) + "\n" + std::to_string(other) + " " +
                                            start_of(other) + "\n");
    std::ofstream add(root / "setup.pids", std::ios::app);
    // Same pid and same exe as ours, but another start time: a pid reused by another run of the same compiler.
    add << reused << " " << std::to_string(std::stoull(start_of(reused)) + 1) << "\n";
    add << old_format << "\n";  // old one-field line: never ours
    add << "99999999 1\n";
    add.close();
    StartupInputs in = inputs(root);
    in.compiler_exe = our_exe;
    in.exe_of_pid = nullptr;  // the real /proc
    in.start_time_of_pid = nullptr;
    in.kill_pid = nullptr;    // the real kill: only reaches pids the test started
    StartupReport r = check_startup(in);
    usleep(100 * 1000);
    CHECK(r.killed_pids.size() == 1 && r.killed_pids[0] == ours && has(r, ErrorId::E73));
    CHECK(!alive(ours));
    CHECK(alive(other) && alive(reused) && alive(old_format));
    for (pid_t p : {other, reused, old_format}) {
        kill(p, SIGKILL);
        waitpid(p, nullptr, 0);
    }
    card_intact(root);
}

// E50 against the real bundled toolchain (DON_REAL_TOOLCHAIN_DIR, else build/toolchain/linux from the working folder):
// the hash rule of startup.cpp must agree with the one that wrote TOOLCHAIN.sha256.
static void test_e50_real() {
    const char* env = std::getenv("DON_REAL_TOOLCHAIN_DIR");
    fs::path tc = env != nullptr ? fs::path(env) : fs::path("build/toolchain/linux");
    std::error_code ec;
    if (!fs::is_directory(tc, ec)) {
        std::cout << "SKIP real toolchain E50: " << tc << " does not exist (set DON_REAL_TOOLCHAIN_DIR)\n";
        return;
    }
    fs::path base = fs::temp_directory_path() / ("don_startup_real_" + std::to_string(getpid()));
    fs::path root = fresh(base, "e50real");
    StartupInputs in = inputs(root);
    in.toolchain_dir = tc;
    StartupReport r = check_startup(in);
    CHECK(r.toolchain_ok && !has(r, ErrorId::E50));
    fs::remove_all(base, ec);
}

static void test_e75(const fs::path& base) {
    fs::path root = fresh(base, "e75");
    make_game(root, "us");
    make_module(root, "aa11", "us", 1);
    put_text(root / "setup.json", "{\"speed\":\"fa");  // cut short by a crash
    StartupReport r = check_startup(inputs(root));
    CHECK(r.journal_rebuilt && has(r, ErrorId::E75));
    CHECK(fs::exists(root / "setup.json.corrupt") && slurp(root / "setup.json.corrupt") == "{\"speed\":\"fa");
    std::string now = slurp(root / "setup.json");
    CHECK(now.find("aa11") != std::string::npos && now.rfind("{\"speed\":", 0) == 0);
    CHECK(fs::exists(root / "module/aa11/manifest.json") && fs::exists(root / "game/us/a.bin"));
    CHECK(!check_startup(inputs(root)).journal_rebuilt);  // the rebuilt file is a good journal
    card_intact(root);
}

static void test_e76(const fs::path& base) {
    fs::path root = fresh(base, "e76");
    make_game(root, "us");
    make_module(root, "aa11", "us", 1);
    make_module(root, "bb22", "jp", 1);  // game/jp/ was deleted
    make_game(root, "eu");                // no module names eu
    StartupReport r = check_startup(inputs(root));
    CHECK(has(r, ErrorId::E76));
    CHECK(std::find(r.missing.begin(), r.missing.end(), "game/jp") != r.missing.end());
    CHECK(std::find(r.missing.begin(), r.missing.end(), "module for eu") != r.missing.end());
    CHECK(r.missing.size() == 2);  // the healthy variant is not listed
    fs::remove(root / "module/aa11/libdon_game.so");
    r = check_startup(inputs(root));
    CHECK(std::find(r.missing.begin(), r.missing.end(), "module/aa11/libdon_game.so") != r.missing.end());
    CHECK(fs::exists(root / "game/eu/a.bin"));  // nothing was touched
    card_intact(root);
}

static void test_e77(const fs::path& base) {
    fs::path root = fresh(base, "e77");
    make_game(root, "us");
    make_module(root, "aa11", "us", 1, std::string(64, '0'));  // manifest hash is not the library's
    make_game(root, "jp");
    make_module(root, "bb22", "jp", 1);
    StartupReport r = check_startup(inputs(root));
    CHECK(has(r, ErrorId::E77) && r.repair_offered);
    CHECK(r.changed.size() == 1 && r.changed[0] == "aa11");
    CHECK(slurp(root / "module/aa11/libdon_game.so") == kLib);  // report only
    card_intact(root);
}

static void test_e80(const fs::path& base) {
    fs::path root = fresh(base, "e80");
    const char* names[] = {"m1", "m2", "m3", "m4"};
    int ages[] = {90, 5, 40, 31};
    for (int i = 0; i < 4; ++i) { make_game(root, names[i]); make_module(root, names[i], names[i], ages[i]); }
    StartupReport r = check_startup(inputs(root));
    CHECK(has(r, ErrorId::E80));
    CHECK((r.removable_modules == std::vector<std::string>{"m1", "m3"}));  // oldest first; m2 and m4 are the newest two
    CHECK(fs::exists(root / "module/m1/manifest.json"));  // listed, not removed
    // all old: the two newest stay
    fs::path old = fresh(base, "e80b");
    for (int i = 0; i < 3; ++i) { make_game(old, names[i]); make_module(old, names[i], names[i], 100 + i); }
    r = check_startup(inputs(old));
    CHECK(r.removable_modules.size() == 1 && r.removable_modules[0] == "m3");
    // exactly two modules, both old: nothing
    fs::path two = fresh(base, "e80c");
    for (int i = 0; i < 2; ++i) { make_game(two, names[i]); make_module(two, names[i], names[i], 200); }
    CHECK(check_startup(inputs(two)).removable_modules.empty());
    card_intact(root);
    card_intact(old);
    card_intact(two);
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_startup_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    test_healthy(tmp);
    test_e33(tmp);
    test_e50(tmp);
    test_e50_label(tmp);
    test_e73(tmp);
    test_e50_real();
    test_e75(tmp);
    test_e76(tmp);
    test_e77(tmp);
    test_e80(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
