// T-737: the Windows twins of the startup pid checks (pid_check_win32.cpp) and of the disc image search roots
// (discover.cpp), run under Wine. No game file is read.
//   bash tools/pipeline/build_windows.sh setup-test   (cross-build, build/windows-setuptest)
//   tools/container/run_wine.sh wine build/windows-setuptest/test_startup_discover_win32.exe
#ifndef _WIN32
#error "Windows only"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "discover.h"
#include "platform/pid_check.h"

namespace fs = std::filesystem;

namespace {
int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

fs::path known(REFKNOWNFOLDERID id) {
    PWSTR w = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &w)) && w) p = fs::path(std::wstring(w));
    CoTaskMemFree(w);
    return p;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has(const std::vector<fs::path>& v, const fs::path& p) {
    return std::any_of(v.begin(), v.end(), [&](const fs::path& x) { return lower(x.string()) == lower(p.string()); });
}

void test_pid_checks(const std::string& self_exe) {
    using namespace don_setup::platform;
    const int me = static_cast<int>(GetCurrentProcessId());
    std::string exe = exe_of_pid(me);
    CHECK(!exe.empty());
    CHECK(lower(exe).find(lower(fs::path(self_exe).filename().string())) != std::string::npos);
    std::string t1 = start_time_of_pid(me);
    CHECK(!t1.empty());
    CHECK(start_time_of_pid(me) == t1);
    CHECK(exe_of_pid(0x7ffffff0).empty());        // no such pid
    CHECK(start_time_of_pid(0x7ffffff0).empty());
    CHECK(!kill_pid(0x7ffffff0));

    // A child process: another start time, our image path, and kill_pid ends it.
    Sleep(20);  // the creation time has 100 ns units but Wine's clock may be coarser
    std::wstring cmd = L"\"" + fs::path(self_exe).wstring() + L"\" --sleep";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    CHECK(ok);
    if (!ok) return;
    int child = static_cast<int>(pi.dwProcessId);
    std::string ce = exe_of_pid(child);
    CHECK(lower(ce) == lower(exe));
    std::string ct = start_time_of_pid(child);
    CHECK(!ct.empty() && ct != t1);
    CHECK(kill_pid(child));
    CHECK(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CHECK(code == 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CHECK(start_time_of_pid(child).empty() || start_time_of_pid(child) == ct);  // gone, or pid not yet reused
}

void test_discover_roots() {
    const fs::path docs = known(FOLDERID_Documents), dl = known(FOLDERID_Downloads), desk = known(FOLDERID_Desktop);
    std::error_code ec;
    CHECK(!docs.empty());
    fs::create_directories(docs, ec);
    fs::create_directories(dl, ec);
    fs::create_directories(desk, ec);
    std::printf("known folders: %s | %s | %s\n", docs.string().c_str(), dl.string().c_str(), desk.string().c_str());

    // PCSX2's ini: its game-list folder becomes a root.
    fs::path games = fs::temp_directory_path() / "don_t737_games";
    fs::create_directories(games, ec);
    fs::path ini = docs / "PCSX2" / "inis" / "PCSX2.ini";
    bool had_ini = fs::exists(ini);
    CHECK(!had_ini);
    fs::create_directories(ini.parent_path(), ec);
    {
        std::ofstream o(ini);
        o << "[UI]\nPaths0=ignored\n[GameList]\r\nRecursivePaths0=" << games.string() << "\r\nPaths0=" << games.string() << "\r\n";
    }
    auto roots = don_setup::discovery_roots("C:\\unused", "unused");
    CHECK(has(roots, docs));
    CHECK(has(roots, dl));
    CHECK(has(roots, desk));
    CHECK(has(roots, games));
    CHECK(std::count_if(roots.begin(), roots.end(), [&](const fs::path& x) { return lower(x.string()) == lower(games.string()); }) == 1);
    CHECK(roots.size() == 4);  // no Linux-only roots, no drive letters

    // Search: a file with a candidate extension that is not this game is not offered; an empty .iso neither.
    { std::ofstream o(games / "not_the_game.iso", std::ios::binary); o << "x"; }
    auto found = don_setup::discover(roots, std::chrono::milliseconds(5000));
    CHECK(found.found.empty());
    CHECK(!found.timed_out);
    { std::ofstream o(games / "packed.zip", std::ios::binary); o << "PK\x03\x04"; }
    found = don_setup::discover(roots, std::chrono::milliseconds(5000));
    CHECK(found.found.empty());

    fs::remove_all(games, ec);
    fs::remove(ini, ec);
    fs::remove(ini.parent_path(), ec);
    fs::remove(ini.parent_path().parent_path(), ec);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--sleep") { Sleep(60000); return 0; }
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    test_pid_checks(self);
    test_discover_roots();
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("test_startup_discover_win32: all checks passed\n");
    return 0;
}
