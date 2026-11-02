// SPDX-License-Identifier: GPL-3.0-or-later
// System information for the [don] start block and the perf line. The per-platform work lives in
// sysinfo_posix.cpp and sysinfo_win32.cpp (AGENTS.md: per-platform code in separate files).
//
// init()/shutdown() bracket the run: on Windows init() raises the multimedia timer to 1 ms
// (timeBeginPeriod(1)) so the audio-pacing hook's 1 ms sleeps really last ~1 ms (without it the
// default ~15.6 ms timer makes every step up to ~15.6 ms, and the game crawls; see host.cpp's
// audio pacing). On POSIX they do nothing. Call init() once at the start of run() and shutdown()
// once before returning; both are idempotent and never throw.
#pragma once

#include <cstdint>
#include <string>

namespace don_sysinfo
{
    void init();
    void shutdown();

    struct Info
    {
        std::string os;             // "Windows 11 (build 22631)" / "Linux 6.8.0 (x86_64)"
        std::string cpu;            // marketing name, e.g. "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz"
        int logical_cores = 0;
        uint64_t ram_total_mib = 0; // physical RAM, MiB
        std::string timer_resolution; // Windows: the timer resolution actually in effect ("1.0 ms"); empty on POSIX
        std::string power_plan;       // Windows: the active power plan's friendly name; empty on POSIX
        std::string power_source;     // Windows: "AC" or "battery"; empty on POSIX
    };

    // Fills the block above. Never throws; missing fields stay empty.
    Info gather();

    // Process resource usage since the process started: CPU time in microseconds and working set in bytes.
    void process_usage(uint64_t &cpu_us, uint64_t &working_set_bytes);
} // namespace don_sysinfo
