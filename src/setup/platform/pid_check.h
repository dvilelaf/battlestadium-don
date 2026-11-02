// The process checks startup.cpp needs to recognise a compiler the preparation left running (E73), per platform
// (M7, T-737): src/setup/platform/pid_check_posix.cpp (/proc, kill) and pid_check_win32.cpp (OpenProcess...).
// A pid alone is not an identity (pids are reused), so a recorded process is ours only when its image path is the
// compiler and its start time equals the recorded one.
#pragma once

#include <string>

namespace don_setup::platform {

// Full path of the executable image of the process; empty when the pid is gone or cannot be inspected.
// POSIX: the target of /proc/<pid>/exe. Windows: QueryFullProcessImageNameW, converted to UTF-8.
std::string exe_of_pid(int pid);

// An opaque start-time token, equal on two calls for the same process and different for a process that reused the
// pid; empty when the pid is gone. POSIX: field 22 of /proc/<pid>/stat (clock ticks since boot, proc(5)).
// Windows: the creation time of GetProcessTimes (FILETIME, 100 ns units since 1601) in decimal.
std::string start_time_of_pid(int pid);

// Ends the process at once (POSIX: SIGKILL; Windows: TerminateProcess). True when it was delivered.
bool kill_pid(int pid);

}  // namespace don_setup::platform
