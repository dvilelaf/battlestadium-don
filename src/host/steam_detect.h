// SPDX-License-Identifier: GPL-3.0-or-later
// Whether the Steam client is running on this machine.
//
// Steam injects virtual gamepads into SDL (its own input layer), so the same controller can look
// completely different to SDL depending on whether Steam is running; that is a common reason a wired
// Xbox controller "only works when Steam is open". The input log (input_sdl.cpp) asks this once at
// start so a report shows that context.
//
// Header-only on purpose: the standalone input tests (tests/input) compile src/host/input_sdl.cpp by
// itself, without the src/host source list, so the function cannot live in a separately linked .cpp
// without breaking that link. Both platform halves are inline and guarded; neither needs anything
// beyond the CRT/kernel32 that is already linked.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#else
#include <filesystem>
#include <fstream>
#include <string>
#endif

namespace input
{

// true when the Steam client process is running. Never throws; any error counts as "not running".
inline bool steam_client_running()
{
#ifdef _WIN32
    // A Toolhelp snapshot of the running processes, looked through for "steam.exe" (case-insensitive:
    // the image name is whatever the process was started as). Nothing is launched.
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (::lstrcmpiW(entry.szExeFile, L"steam.exe") == 0)
            {
                found = true;
                break;
            }
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
#else
    // Steam's main process shows up in /proc as a task whose "comm" is exactly "steam" (the web helpers
    // are "steamwebhelper" and the like, but the client itself is always "steam"). Scan /proc/<pid>/comm:
    // no privilege and no external command needed.
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string pid = it->path().filename().string();
        if (pid.empty() || pid[0] < '0' || pid[0] > '9')
            continue; // only /proc/<pid>, not /proc/self, /proc/sys, ...
        std::ifstream comm(it->path() / "comm");
        std::string name;
        std::getline(comm, name);
        if (name == "steam")
            return true;
    }
    return false;
#endif
}

} // namespace input
