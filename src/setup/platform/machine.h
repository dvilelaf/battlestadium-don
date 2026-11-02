// Machine facts the setup needs while it prepares the game (task T-632): available memory, memory pressure, free
// disk space and the battery. No OS header here: the implementation is in machine_<platform>.cpp.
// Every reader takes the root of /proc and /sys as a parameter so the tests can point it at a fake tree; an empty
// root means the real "/".
// Windows (machine_win32.cpp, T-733): the root parameters are ignored and the real machine is read; psi_present is
// always false (no PSI); find_in_path splits on ';' and also tries "<name>.exe"; inhibitor_environment is empty.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace don_setup {

struct MemorySample {
    bool available_known = false;   // /proc/meminfo had MemAvailable
    uint64_t available_kib = 0;
    bool psi_present = false;       // /proc/pressure/memory exists (kernel 4.20+ with PSI enabled)
    double psi_some_avg10 = 0.0;    // % of the last 10 s in which at least one task stalled on memory
    double psi_full_avg10 = 0.0;    // % of the last 10 s in which all tasks stalled on memory
};

// Reads <root>/proc/meminfo and <root>/proc/pressure/memory. Never throws; a missing file leaves its fields unset
// (callers fall back to MemAvailable when psi_present is false).
MemorySample read_memory(const std::string& root = "");

struct DiskSpace {
    bool known = false;
    uint64_t free_bytes = 0;        // available to an unprivileged process
    std::string checked_path;       // the nearest existing parent folder that was asked
};

// Free space of the folder, or of its nearest existing parent when it does not exist yet.
DiskSpace free_disk_space(const std::string& path);

struct PowerState {
    bool on_battery = false;        // discharging with no mains supply online; no battery at all means on mains
    bool battery_present = false;
    int percent = -1;               // first battery's capacity, -1 when unknown
};

// Reads <root>/sys/class/power_supply.
PowerState read_power(const std::string& root = "");

// First executable (not a folder) called `name` in the PATH-style list, or "". An empty list means $PATH.
std::string find_in_path(const std::string& name, const std::string& search_path);

// "NAME=value" entries of the variables a systemd-inhibit child needs to reach logind (PATH and the bus
// addresses); everything else of the environment stays out.
std::vector<std::string> inhibitor_environment();

}  // namespace don_setup
