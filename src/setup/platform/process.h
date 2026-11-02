// Child processes for the setup (task T-631): the compiler runs as a child we fully control. It lives in its own
// process group, is killed if our program dies (Linux), has a time limit and a bounded stderr (and optionally stdout) capture.
// No OS header here: the implementation is in process_<platform>.cpp.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace don_setup {

struct SpawnOptions {
    std::vector<std::string> argv;  // argv[0] is the program path, executed as given (no PATH search)
    std::vector<std::string> env;   // complete environment, "NAME=value"; the child gets nothing else
    std::string cwd;                // working directory of the child; empty keeps ours
    std::string tmpdir;             // exported to the child as TMPDIR when not empty
    int nice = 0;                   // added to the niceness, best effort
    int oom_score_adj = 0;          // written to /proc/self/oom_score_adj when not 0, best effort
    long timeout_ms = 0;            // 0 means no limit; when it expires the whole group is killed
    size_t stderr_limit = 64 * 1024;  // only the last stderr_limit bytes are kept
    size_t stdout_limit = 0;          // 0 sends stdout to /dev/null; otherwise the last stdout_limit bytes are kept
};

struct ExitStatus {
    bool exited = false;    // normal exit, `code` is valid
    int code = 0;
    int signal = 0;         // terminating signal when !exited
    bool timed_out = false; // the time limit killed it
};

struct Process {
    int pid = -1;       // also the process group id
    int stderr_fd = -1; // read end of the capture pipe
    long long deadline_ms = 0;  // monotonic clock; 0 means none
    size_t stderr_limit = 0;
    int stdout_fd = -1; // read end of the stdout capture pipe (only when stdout_limit was not 0)
    size_t stdout_limit = 0;
    std::string stdout_tail;  // the last bytes of the child's stdout
    std::string stderr_tail;  // the last bytes of the child's stderr
    bool finished = false;
    bool timed_out = false;
    ExitStatus status;
    // Windows only (process_win32.cpp), untouched on POSIX. HANDLEs kept as void* so no OS header is needed here.
    // On Windows `pid` is the process id, the *_fd fields stay -1 and the group is the Job object.
    void* proc_handle = nullptr;    // the child process
    void* job_handle = nullptr;     // Job object holding the child and its descendants
    void* stderr_handle = nullptr;  // read end of the stderr pipe
    void* stdout_handle = nullptr;  // read end of the stdout pipe
    bool killed = false;            // kill_group() terminated the job
};

// Starts the child. On failure returns false and fills `error`.
bool spawn(const SpawnOptions& opts, Process& out, std::string& error);

// Collects stderr, enforces the timeout, and reaps the child without blocking.
// True once it has finished; `status` is then valid (also in p.status).
bool poll(Process& p, ExitStatus& status);

// Blocks until the child has finished (the timeout still applies) and returns its status.
ExitStatus wait(Process& p);

// SIGKILLs every process of the child's group, including grandchildren (Windows: TerminateJobObject; the status then
// reads signal 9). Safe after the child has been reaped.
void kill_group(Process& p);

}  // namespace don_setup
