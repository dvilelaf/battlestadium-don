// Linux readers for machine.h: /proc, statvfs and /sys/class/power_supply (task T-632).
#include "platform/machine.h"
#include "path_text.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace don_setup {

namespace {

bool read_first_line(const fs::path& p, std::string& out) {
    std::ifstream in(p);
    return static_cast<bool>(std::getline(in, out));
}

// "avg10=1.23" -> 1.23 inside a PSI line such as "some avg10=1.23 avg60=0.5 avg300=0.1 total=99".
bool psi_field(const std::string& line, const std::string& key, double& value) {
    const size_t at = line.find(key + "=");
    if (at == std::string::npos) return false;
    value = std::strtod(line.c_str() + at + key.size() + 1, nullptr);
    return true;
}

}  // namespace

MemorySample read_memory(const std::string& root) {
    MemorySample s;
    const fs::path base = root.empty() ? fs::path("/") : fs::path(root);
    {
        std::ifstream in(base / "proc/meminfo");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("MemAvailable:", 0) != 0) continue;
            s.available_kib = std::strtoull(line.c_str() + 13, nullptr, 10);
            s.available_known = true;
            break;
        }
    }
    {
        std::ifstream in(base / "proc/pressure/memory");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("some ", 0) == 0 && psi_field(line, "avg10", s.psi_some_avg10)) s.psi_present = true;
            if (line.rfind("full ", 0) == 0) psi_field(line, "avg10", s.psi_full_avg10);
        }
    }
    return s;
}

DiskSpace free_disk_space(const std::string& path) {
    DiskSpace d;
    std::error_code ec;
    fs::path p = fs::absolute(path, ec);
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    d.checked_path = path_text(p);
    struct statvfs sv {};
    if (statvfs(d.checked_path.c_str(), &sv) != 0) return d;
    d.known = true;
    d.free_bytes = static_cast<uint64_t>(sv.f_bavail) * static_cast<uint64_t>(sv.f_frsize);
    return d;
}

PowerState read_power(const std::string& root) {
    PowerState st;
    const fs::path dir = (root.empty() ? fs::path("/") : fs::path(root)) / "sys/class/power_supply";
    std::error_code ec;
    bool mains_online = false;
    bool discharging = false;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::string type;
        if (!read_first_line(entry.path() / "type", type)) continue;
        if (type == "Battery") {
            // Scope=Device batteries (a wireless mouse) are not the machine's.
            std::string scope;
            if (read_first_line(entry.path() / "scope", scope) && scope == "Device") continue;
            std::string status, cap;
            if (!st.battery_present && read_first_line(entry.path() / "capacity", cap)) st.percent = std::atoi(cap.c_str());
            st.battery_present = true;
            if (read_first_line(entry.path() / "status", status) && status == "Discharging") discharging = true;
        } else if (type == "Mains" || type == "USB") {
            std::string online;
            if (read_first_line(entry.path() / "online", online) && online == "1") mains_online = true;
        }
    }
    st.on_battery = st.battery_present && discharging && !mains_online;
    return st;
}

std::string find_in_path(const std::string& name, const std::string& search_path) {
    std::string list = search_path;
    if (list.empty()) {
        const char* env = std::getenv("PATH");
        list = env ? env : "";
    }
    std::stringstream ss(list);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        const std::string candidate = path_text(fs::path(dir) / name);
        if (access(candidate.c_str(), X_OK) == 0 && !fs::is_directory(candidate)) return candidate;
    }
    return "";
}

std::vector<std::string> inhibitor_environment() {
    std::vector<std::string> env;
    for (const char* name : {"PATH", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS"}) {
        if (const char* v = std::getenv(name)) env.push_back(std::string(name) + "=" + v);
    }
    return env;
}

}  // namespace don_setup
