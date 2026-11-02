// Process checks on Linux, through /proc (proc(5)). See pid_check.h.
#include "pid_check.h"
#include "path_text.h"

#include <signal.h>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace don_setup::platform {

std::string exe_of_pid(int pid) {
    std::error_code ec;
    fs::path l = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    return ec ? std::string() : path_text(l);
}

// Field 22 of /proc/<pid>/stat (proc(5)): the start time in clock ticks since boot. Field 2 is "(comm)" and comm may hold
// spaces and ')', so the fields are counted from the last ')': the state (field 3) comes right after it.
std::string start_time_of_pid(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat", std::ios::binary);
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t close = stat.rfind(')');
    if (close == std::string::npos) return {};
    size_t at = close + 1;
    for (int field = 3; field <= 22; ++field) {
        at = stat.find_first_not_of(' ', at);
        if (at == std::string::npos) return {};
        size_t end = stat.find_first_of(" \n", at);
        if (end == std::string::npos) end = stat.size();
        if (field == 22) return stat.substr(at, end - at);
        at = end;
    }
    return {};
}

bool kill_pid(int pid) { return kill(pid, SIGKILL) == 0; }

}  // namespace don_setup::platform
