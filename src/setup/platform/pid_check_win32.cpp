// Process checks on Windows. See pid_check.h.
#include "pid_check.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace don_setup::platform {
namespace {

struct Handle {
    HANDLE h;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (h) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string utf8_of(const wchar_t* w, int n) {
    if (n <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

}  // namespace

std::string exe_of_pid(int pid) {
    // PROCESS_QUERY_LIMITED_INFORMATION is enough for the image path and the times, and is granted for processes of the
    // same user even when PROCESS_QUERY_INFORMATION is not.
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    if (!p.h) return {};
    std::vector<wchar_t> buf(32768);  // the longest path Windows allows (\\?\ form)
    DWORD n = static_cast<DWORD>(buf.size());
    if (!QueryFullProcessImageNameW(p.h, 0, buf.data(), &n)) return {};
    return utf8_of(buf.data(), static_cast<int>(n));
}

std::string start_time_of_pid(int pid) {
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
    if (!p.h) return {};
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(p.h, &created, &exited, &kernel, &user)) return {};
    ULARGE_INTEGER t;
    t.LowPart = created.dwLowDateTime;
    t.HighPart = created.dwHighDateTime;
    return std::to_string(t.QuadPart);
}

bool kill_pid(int pid) {
    Handle p(OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid)));
    if (!p.h) return false;
    return TerminateProcess(p.h, 1) != 0;
}

}  // namespace don_setup::platform
