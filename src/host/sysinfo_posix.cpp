// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX (Linux and the BSDs) half of don_sysinfo (see sysinfo.h). Nothing here calls a GUI or a
// speaker; it only reads the kernel's own counters and /proc.
#include "sysinfo.h"

#ifndef _WIN32

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace don_sysinfo
{
    void init()
    {
        // The default timer is already fine on Linux; nothing to do.
    }

    void shutdown()
    {
    }

    namespace
    {
        // The CPU model name from /proc/cpuinfo: "model name" on x86, "Hardware" on ARM, "Model" on RISC-V.
        std::string cpu_model()
        {
            std::ifstream file("/proc/cpuinfo");
            std::string line;
            while (std::getline(file, line))
            {
                for (const char *key : {"model name", "Hardware", "Model"})
                {
                    if (line.compare(0, std::strlen(key), key) == 0)
                    {
                        const size_t colon = line.find(':');
                        if (colon != std::string::npos)
                        {
                            std::string value = line.substr(colon + 1);
                            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                                value.erase(value.begin());
                            if (!value.empty())
                                return value;
                        }
                    }
                }
            }
            return {};
        }
    } // namespace

    Info gather()
    {
        Info info;
        utsname un{};
        if (::uname(&un) == 0)
        {
            info.os = std::string(un.sysname) + " " + un.release + " (" + un.machine + ")";
        }
        info.cpu = cpu_model();
        const long cores = ::sysconf(_SC_NPROCESSORS_ONLN);
        if (cores > 0)
            info.logical_cores = static_cast<int>(cores);
        const long pages = ::sysconf(_SC_PHYS_PAGES);
        const long page_size = ::sysconf(_SC_PAGESIZE);
        if (pages > 0 && page_size > 0)
        {
            const uint64_t bytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
            info.ram_total_mib = bytes / (1024u * 1024u);
        }
        // timer_resolution, power_plan and power_source are Windows-only: empty here.
        return info;
    }

    void process_usage(uint64_t &cpu_us, uint64_t &working_set_bytes)
    {
        cpu_us = 0;
        working_set_bytes = 0;
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0)
            return;
        const uint64_t user_us =
            static_cast<uint64_t>(usage.ru_utime.tv_sec) * 1000000u + static_cast<uint64_t>(usage.ru_utime.tv_usec);
        const uint64_t sys_us =
            static_cast<uint64_t>(usage.ru_stime.tv_sec) * 1000000u + static_cast<uint64_t>(usage.ru_stime.tv_usec);
        cpu_us = user_us + sys_us;
        // On Linux ru_maxrss is kilobytes; on the BSDs it is bytes. Guard the Linux case by the sysname.
        utsname un{};
        const bool is_linux = ::uname(&un) == 0 && std::strcmp(un.sysname, "Linux") == 0;
        const uint64_t maxrss = static_cast<uint64_t>(usage.ru_maxrss);
        working_set_bytes = is_linux ? maxrss * 1024u : maxrss;
    }
} // namespace don_sysinfo

#endif // !_WIN32
