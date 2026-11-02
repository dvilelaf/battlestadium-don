// Windows twins of the platform layer (task T-733): single-instance lock, network-drive check and machine readers.
// Built only for Windows (cross-compiled, run under Wine); on other hosts it compiles to a no-op that passes.
#ifndef _WIN32
int main() { return 0; }
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <string>

#include "discover.h"
#include "platform/machine.h"
#include "user_paths.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

// Child role: hold the lock, signal readiness with a file, then wait to be killed.
static int hold_lock(const fs::path& dir, const fs::path& ready) {
    InstanceLock lock = acquire_single_instance(dir);
    if (!lock) return 3;
    HANDLE f = CreateFileW(ready.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Sleep(120000);
    return 0;
}

static void test_lock(const fs::path& tmp) {
    const fs::path dir = tmp / "lock";
    {
        InstanceLock a = acquire_single_instance(dir);
        CHECK(static_cast<bool>(a));
        CHECK(fs::exists(dir / "setup.lock"));
        InstanceLock b = acquire_single_instance(dir);  // same process, second handle
        CHECK(!b);
    }
    InstanceLock c = acquire_single_instance(dir);  // released by the destructor
    CHECK(static_cast<bool>(c));
    c = InstanceLock();
    // A folder that cannot be created (a file is in the way) gives an empty lock.
    const fs::path blocker = tmp / "blocker";
    CloseHandle(CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(!acquire_single_instance(blocker / "sub"));
}

// Another process holds the lock; once it is killed (no clean exit) the lock is free again.
static void test_lock_dead_holder(const fs::path& tmp, const std::wstring& self) {
    const fs::path dir = tmp / "lock2", ready = tmp / "ready.flag";
    std::wstring cmd = L"\"" + self + L"\" --hold \"" + dir.wstring() + L"\" \"" + ready.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL cannot start the holder process (error %lu)\n", GetLastError());
        ++g_failed;
        return;
    }
    for (int i = 0; i < 300 && !fs::exists(ready); ++i) Sleep(100);
    CHECK(fs::exists(ready));
    CHECK(!acquire_single_instance(dir));  // held by the other process
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    InstanceLock after = acquire_single_instance(dir);
    CHECK(static_cast<bool>(after));  // a dead holder does not block
}

static void test_network(const fs::path& tmp) {
    CHECK(is_network_filesystem(fs::path(L"\\\\server\\share\\games")));
    CHECK(is_network_filesystem(fs::path(L"\\\\server\\share")));
    CHECK(is_network_filesystem(fs::path(L"\\\\?\\UNC\\server\\share\\games")));
    CHECK(!is_network_filesystem(fs::path(L"\\\\?\\C:\\Windows")));
    CHECK(!is_network_filesystem(fs::path(L"relative\\folder")));  // resolved against the current (local) folder
    CHECK(!is_network_filesystem(tmp));
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    CHECK(!is_network_filesystem(fs::path(windir)));  // the system drive is never remote
    CHECK(!is_network_filesystem(tmp / "does" / "not" / "exist"));
}

static void test_machine(const fs::path& tmp) {
    const MemorySample m = read_memory();
    CHECK(m.available_known);
    CHECK(m.available_kib > 0);
    CHECK(!m.psi_present);
    CHECK(read_memory("ignored-root").available_known);

    const DiskSpace d = free_disk_space(tmp.string());
    CHECK(d.known);
    CHECK(d.free_bytes > 0);
    const DiskSpace n = free_disk_space((tmp / "not" / "yet" / "created").string());  // nearest existing parent
    CHECK(n.known);
    CHECK(fs::equivalent(fs::path(n.checked_path), tmp));
    CHECK(n.free_bytes > 0);

    const PowerState p = read_power();
    CHECK(!p.on_battery || p.battery_present);
    CHECK(p.percent >= -1 && p.percent <= 100);

    const std::string sys = find_in_path("cmd", "");  // the system folder is on PATH; ".exe" is appended
    CHECK(!sys.empty() || std::getenv("PATH") == nullptr);
    const fs::path exe_dir = tmp / "bin";
    fs::create_directories(exe_dir);
    CloseHandle(CreateFileW((exe_dir / "tool.exe").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    const std::string list = "C:\\nowhere;" + exe_dir.string();
    CHECK(fs::path(find_in_path("tool", list)).filename() == "tool.exe");
    CHECK(fs::path(find_in_path("tool.exe", list)).filename() == "tool.exe");
    CHECK(find_in_path("absent", list).empty());
    CHECK(inhibitor_environment().empty());
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wcscmp(argv[1], L"--hold") == 0) return hold_lock(argv[2], argv[3]);
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    const fs::path tmp = fs::path(buf) / (L"don_t733_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    test_lock(tmp);
    test_lock_dead_holder(tmp, argv[0]);
    test_network(tmp);
    test_machine(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (g_failed) {
        std::printf("test_platform_win32: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("test_platform_win32: all passed\n");
    return 0;
}
#endif
