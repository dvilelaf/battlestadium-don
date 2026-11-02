// POSIX implementation of platform/process.h (task T-631).
#include "platform/process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <poll.h>

namespace don_setup {

namespace {

// Descriptors above stderr that the child must not inherit; closing stops at this bound when the system
// reports no (or a huge) limit.
constexpr int kMaxFdToClose = 4096;
constexpr int kExecFailedCode = 127;  // the shell convention for "command not found / not executable"
constexpr int kWaitSliceMs = 20;      // longest sleep between checks while waiting

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void close_inherited_fds() {
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > kMaxFdToClose) limit = kMaxFdToClose;
    for (int fd = STDERR_FILENO + 1; fd < limit; ++fd) close(fd);
}

void write_oom_score_adj(int value) {
    FILE* f = std::fopen("/proc/self/oom_score_adj", "w");
    if (!f) return;
    std::fprintf(f, "%d", value);
    std::fclose(f);
}

[[noreturn]] void child_main(const SpawnOptions& o, int err_write, int out_write, const std::vector<char*>& argv,
                             const std::vector<char*>& envp, pid_t parent) {
    setpgid(0, 0);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    // The parent may have died between fork and prctl; the signal would then never come.
    if (getppid() != parent) _exit(kExecFailedCode);
#else
    (void)parent;
#endif
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        if (out_write < 0) dup2(devnull, STDOUT_FILENO);
    }
    if (out_write >= 0) dup2(out_write, STDOUT_FILENO);
    dup2(err_write, STDERR_FILENO);
    close_inherited_fds();
    if (o.nice != 0) {
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, 0);
        if (!(cur == -1 && errno != 0)) setpriority(PRIO_PROCESS, 0, cur + o.nice);
    }
    if (o.oom_score_adj != 0) write_oom_score_adj(o.oom_score_adj);
    if (!o.cwd.empty() && chdir(o.cwd.c_str()) != 0) _exit(kExecFailedCode);
    execve(argv[0], argv.data(), envp.data());
    _exit(kExecFailedCode);
}

void drain_fd(int& fd, std::string& tail, size_t limit) {
    if (fd < 0) return;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            tail.append(buf, size_t(n));
            if (tail.size() > limit) tail.erase(0, tail.size() - limit);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) {  // every writer is gone
            close(fd);
            fd = -1;
        }
        return;  // EOF or EAGAIN
    }
}

void drain(Process& p) {
    drain_fd(p.stderr_fd, p.stderr_tail, p.stderr_limit);
    drain_fd(p.stdout_fd, p.stdout_tail, p.stdout_limit);
}

}  // namespace

bool spawn(const SpawnOptions& opts, Process& out, std::string& error) {
    if (opts.argv.empty()) {
        error = "spawn: empty argv";
        return false;
    }
    // Everything the child needs is built before fork: only async-signal-safe calls run after it.
    std::vector<std::string> envs = opts.env;
    if (!opts.tmpdir.empty()) envs.push_back("TMPDIR=" + opts.tmpdir);
    std::vector<char*> argv, envp;
    for (const auto& a : opts.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (const auto& e : envs) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    for (int fd : fds) fcntl(fd, F_SETFD, FD_CLOEXEC);
    int ofds[2] = {-1, -1};
    if (opts.stdout_limit > 0) {
        if (pipe(ofds) != 0) {
            error = std::string("pipe: ") + std::strerror(errno);
            close(fds[0]);
            close(fds[1]);
            return false;
        }
        for (int fd : ofds) fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        close(fds[0]);
        close(fds[1]);
        for (int fd : ofds) if (fd >= 0) close(fd);
        return false;
    }
    if (pid == 0) {
        // dup2 clears FD_CLOEXEC on the new descriptor, so stderr survives the exec.
        child_main(opts, fds[1], ofds[1], argv, envp, parent);
    }
    setpgid(pid, pid);  // also done in the child; whichever runs first wins, no race
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    if (ofds[0] >= 0) {
        close(ofds[1]);
        fcntl(ofds[0], F_SETFL, fcntl(ofds[0], F_GETFL) | O_NONBLOCK);
    }
    out = Process{};
    out.pid = pid;
    out.stderr_fd = fds[0];
    out.stderr_limit = opts.stderr_limit;
    out.stdout_fd = ofds[0];
    out.stdout_limit = opts.stdout_limit;
    out.deadline_ms = opts.timeout_ms > 0 ? now_ms() + opts.timeout_ms : 0;
    return true;
}

bool poll(Process& p, ExitStatus& status) {
    if (p.finished) {
        status = p.status;
        return true;
    }
    drain(p);
    if (p.deadline_ms != 0 && !p.timed_out && now_ms() >= p.deadline_ms) {
        p.timed_out = true;
        kill_group(p);
    }
    int ws = 0;
    pid_t r;
    do r = waitpid(p.pid, &ws, WNOHANG); while (r < 0 && errno == EINTR);
    if (r == 0) return false;
    p.finished = true;
    if (r > 0) {
        if (WIFEXITED(ws)) {
            p.status.exited = true;
            p.status.code = WEXITSTATUS(ws);
        } else if (WIFSIGNALED(ws)) {
            p.status.signal = WTERMSIG(ws);
        }
    }
    p.status.timed_out = p.timed_out;
    drain(p);
    // Grandchildren may still hold the pipe open; we have what the child wrote, so stop reading.
    if (p.stderr_fd >= 0) {
        close(p.stderr_fd);
        p.stderr_fd = -1;
    }
    if (p.stdout_fd >= 0) {
        close(p.stdout_fd);
        p.stdout_fd = -1;
    }
    status = p.status;
    return true;
}

ExitStatus wait(Process& p) {
    ExitStatus st;
    while (!poll(p, st)) {
        struct pollfd pfd = {p.stderr_fd, POLLIN, 0};
        // A closed capture pipe is ignored by poll(2) when fd is negative, so this is a plain sleep then.
        ::poll(&pfd, 1, kWaitSliceMs);
        if (p.stderr_fd < 0) usleep(kWaitSliceMs * 1000);
    }
    return st;
}

void kill_group(Process& p) {
    if (p.pid > 0) kill(-p.pid, SIGKILL);
}

}  // namespace don_setup
