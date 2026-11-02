// SPDX-License-Identifier: GPL-3.0-or-later
// Windows half of don_sysinfo (see sysinfo.h). Compiled only on Windows.
//
// init() raises the multimedia timer to 1 ms with timeBeginPeriod(1): the audio-pacing hook in
// host.cpp sleeps in 1 ms steps at every vblank, and without this call a process gets the default
// ~15.6 ms timer, so each "1 ms" step can last ~15.6 ms and the game crawls (and since Windows 10
// 2004 whether it is finer depends on what other programs asked for, which explains one slow run
// and then fine runs). timeEndPeriod(1) in shutdown() undoes it.
#ifdef _WIN32

#include "sysinfo.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace don_sysinfo
{
    namespace
    {
        bool g_period_begun = false;

        // ntdll.dll is always loaded; RtlGetVersion and NtQueryTimerResolution are not in the SDK's
        // import libraries, so they are looked up at run time (never an extra link dependency).
        typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        typedef LONG(WINAPI *NtQueryTimerResolutionFn)(ULONG *, ULONG *, ULONG *);

        // powrprof.dll: the power plan name. Loaded lazily so a system without it still runs.
        typedef DWORD(WINAPI *PowerGetActiveSchemeFn)(HKEY, GUID **);
        typedef DWORD(WINAPI *PowerReadFriendlyNameFn)(HKEY, const GUID *, const GUID *, const GUID *, UCHAR *, DWORD *);

        std::string read_reg_string(HKEY root, const wchar_t *sub, const wchar_t *name)
        {
            wchar_t buffer[512] = {};
            DWORD size = static_cast<DWORD>(sizeof(buffer));
            DWORD type = 0;
            if (::RegGetValueW(root, sub, name, RRF_RT_REG_SZ, &type, buffer, &size) != ERROR_SUCCESS)
                return {};
            return std::string(buffer, buffer + (size / sizeof(wchar_t) - 1));
        }
    } // namespace

    void init()
    {
        if (!g_period_begun)
        {
            ::timeBeginPeriod(1); // 1 ms granularity for the audio-pacing sleep loop; see the header above
            g_period_begun = true;
        }
    }

    void shutdown()
    {
        if (g_period_begun)
        {
            ::timeEndPeriod(1);
            g_period_begun = false;
        }
    }

    Info gather()
    {
        Info info;

        // OS version through RtlGetVersion (GetVersionEx lies on newer Windows without a manifest).
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
        {
            const auto rtl_get_version =
                reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            if (rtl_get_version && rtl_get_version(&version) == 0)
            {
                // Build 22000 and later are Windows 11; anything lower that still reports major 10 is 10.
                const char *family = (version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000) ? "Windows 11"
                                                                                                     : "Windows";
                char text[128];
                std::snprintf(text, sizeof(text), "%s %lu.%lu (build %lu)", family, version.dwMajorVersion,
                              version.dwMinorVersion, version.dwBuildNumber);
                info.os = text;
            }
        }

        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        info.logical_cores = static_cast<int>(si.dwNumberOfProcessors);

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (::GlobalMemoryStatusEx(&mem))
            info.ram_total_mib = mem.ullTotalPhys / (1024u * 1024u);

        info.cpu = read_reg_string(HKEY_LOCAL_MACHINE,
                                   L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                   L"ProcessorNameString");
        if (info.cpu.empty())
            info.cpu = read_reg_string(HKEY_LOCAL_MACHINE,
                                       L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                       L"ProcessorName");

        // Timer resolution actually in effect (100 ns units; shows the effect of timeBeginPeriod(1)).
        if (ntdll)
        {
            const auto nt_query =
                reinterpret_cast<NtQueryTimerResolutionFn>(::GetProcAddress(ntdll, "NtQueryTimerResolution"));
            ULONG minimum = 0, maximum = 0, current = 0;
            if (nt_query && nt_query(&minimum, &maximum, &current) == 0)
            {
                char text[64];
                std::snprintf(text, sizeof(text), "%.3f ms", static_cast<double>(current) / 10000.0);
                info.timer_resolution = text;
            }
        }

        // Power plan friendly name and AC/battery, both best-effort.
        const HMODULE powrprof = ::LoadLibraryW(L"powrprof.dll");
        if (powrprof)
        {
            const auto get_scheme =
                reinterpret_cast<PowerGetActiveSchemeFn>(::GetProcAddress(powrprof, "PowerGetActiveScheme"));
            const auto read_name =
                reinterpret_cast<PowerReadFriendlyNameFn>(::GetProcAddress(powrprof, "PowerReadFriendlyName"));
            if (get_scheme && read_name)
            {
                GUID *scheme = nullptr;
                if (get_scheme(nullptr, &scheme) == ERROR_SUCCESS && scheme)
                {
                    UCHAR buffer[512] = {};
                    DWORD size = static_cast<DWORD>(sizeof(buffer));
                    if (read_name(nullptr, scheme, nullptr, nullptr, buffer, &size) == ERROR_SUCCESS)
                    {
                        // The name comes as UTF-16; the log is UTF-8.
                        const wchar_t *wide = reinterpret_cast<const wchar_t *>(buffer);
                        char utf8[512] = {};
                        if (::WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, static_cast<int>(sizeof(utf8)),
                                                  nullptr, nullptr) > 0)
                            info.power_plan = utf8;
                    }
                    ::LocalFree(scheme);
                }
            }
        }
        SYSTEM_POWER_STATUS power{};
        if (::GetSystemPowerStatus(&power))
            info.power_source = power.ACLineStatus == 1 ? "AC" : (power.ACLineStatus == 0 ? "battery" : "");

        return info;
    }

    void process_usage(uint64_t &cpu_us, uint64_t &working_set_bytes)
    {
        cpu_us = 0;
        working_set_bytes = 0;
        FILETIME creation, exit_time, kernel, user;
        if (::GetProcessTimes(::GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
        {
            const auto to_us = [](const FILETIME &ft) {
                ULARGE_INTEGER u;
                u.LowPart = ft.dwLowDateTime;
                u.HighPart = ft.dwHighDateTime;
                return u.QuadPart / 10ull; // 100 ns -> us
            };
            cpu_us = to_us(kernel) + to_us(user);
        }
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
            working_set_bytes = counters.WorkingSetSize;
    }
} // namespace don_sysinfo

#endif // _WIN32
