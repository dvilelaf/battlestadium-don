// Windows readers for machine.h (task T-733): GlobalMemoryStatusEx, GetDiskFreeSpaceExW, GetSystemPowerStatus.
// There is no PSI on Windows: psi_present stays false, which callers already treat as "use the available memory".
// The `root` parameters exist for the tests of the Linux readers; here they are ignored (the real machine is read).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include "platform/machine.h"

namespace fs = std::filesystem;

namespace don_setup {

namespace {

// Paths cross machine.h as UTF-8 std::string; the Win32 calls want UTF-16.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

MemorySample read_memory(const std::string&) {
    MemorySample s;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.available_known = true;
        s.available_kib = ms.ullAvailPhys / 1024;
    }
    return s;
}

DiskSpace free_disk_space(const std::string& path) {
    DiskSpace d;
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(widen(path)), ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    d.checked_path = narrow(p.native());
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) return d;
    d.known = true;
    d.free_bytes = avail.QuadPart;  // available to the calling user (quota-aware)
    return d;
}

PowerState read_power(const std::string&) {
    PowerState st;
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return st;
    constexpr BYTE kNoBattery = 128, kUnknown = 255;  // BatteryFlag / BatteryLifePercent / ACLineStatus sentinels
    st.battery_present = sps.BatteryFlag != kUnknown && (sps.BatteryFlag & kNoBattery) == 0;
    if (st.battery_present && sps.BatteryLifePercent != kUnknown) st.percent = sps.BatteryLifePercent;
    st.on_battery = st.battery_present && sps.ACLineStatus == 0;  // 0 offline, 1 online, 255 unknown
    return st;
}

std::string find_in_path(const std::string& name, const std::string& search_path) {
    std::string list = search_path;
    if (list.empty()) {
        const char* env = std::getenv("PATH");
        list = env ? env : "";
    }
    const fs::path file = fs::path(widen(name));
    const bool has_ext = file.has_extension();
    std::stringstream ss(list);
    std::string dir;
    while (std::getline(ss, dir, ';')) {
        if (dir.empty()) continue;
        for (const bool add_exe : {false, true}) {
            if (add_exe && has_ext) break;
            fs::path candidate = fs::path(widen(dir)) / file;
            if (add_exe) candidate += L".exe";
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec)) return narrow(candidate.native());
        }
    }
    return "";
}

// No logind on Windows: nothing to pass on.
std::vector<std::string> inhibitor_environment() { return {}; }

}  // namespace don_setup
