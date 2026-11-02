// Tests for platform/process.h (task T-631), with /bin/sh children.
#include "platform/process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace don_setup;

namespace {

int failures = 0;
constexpr int kGoneWaitMs = 3000;  // longest wait for a killed process to disappear

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

SpawnOptions sh(const std::string& script) {
    SpawnOptions o;
    o.argv = {"/bin/sh", "-c", script};
    o.env = {"PATH=/usr/bin:/bin"};
    return o;
}

// A killed process that nobody reaped (reparented to init or a subreaper) can stay a zombie for a moment.
bool is_gone(long pid) {
    for (int waited = 0; waited <= kGoneWaitMs; waited += 20) {
        if (kill(pid_t(pid), 0) != 0 && errno == ESRCH) return true;
        std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        std::getline(st, line);
        size_t close_paren = line.rfind(')');
        if (close_paren != std::string::npos && close_paren + 2 < line.size() && line[close_paren + 2] == 'Z')
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

long pid_from(const std::string& text) { return std::strtol(text.c_str(), nullptr, 10); }

void test_exit_code_and_signal() {
    Process p;
    std::string err;
    CHECK(spawn(sh("exit 7"), p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 7 && !st.timed_out);

    CHECK(spawn(sh("kill -TERM $$"), p, err));
    st = wait(p);
    CHECK(!st.exited && st.signal == SIGTERM);

    SpawnOptions bad;
    bad.argv = {"/nonexistent/program"};
    CHECK(spawn(bad, p, err));
    st = wait(p);
    CHECK(st.exited && st.code == 127);
}

void test_env_cwd_tmpdir() {
    SpawnOptions o = sh("echo \"$TMPDIR $ONLY $(pwd)\" >&2; [ -z \"$HOME\" ]");
    o.env = {"PATH=/usr/bin:/bin", "ONLY=yes"};
    o.tmpdir = "/tmp/don-t631";
    o.cwd = "/";
    Process p;
    std::string err;
    CHECK(spawn(o, p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 0);
    CHECK(p.stderr_tail == "/tmp/don-t631 yes /\n");
}

void test_stdout_capture() {
    SpawnOptions o = sh("echo '[progress] 1/3'; echo err >&2; echo '[progress] 3/3'");
    o.stdout_limit = 1024;
    Process p;
    std::string err;
    CHECK(spawn(o, p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 0);
    CHECK(p.stdout_tail == "[progress] 1/3\n[progress] 3/3\n");
    CHECK(p.stderr_tail == "err\n");
    // Bounded: only the last bytes are kept.
    o = sh("i=0; while [ $i -lt 2000 ]; do echo 0123456789abcdefghij line-$i; i=$((i+1)); done");
    o.stdout_limit = 100;
    CHECK(spawn(o, p, err));
    wait(p);
    CHECK(p.stdout_tail.size() == 100 && p.stdout_tail.find("line-1999\n") != std::string::npos);
    // Off by default: nothing is captured.
    CHECK(spawn(sh("echo hidden"), p, err));
    wait(p);
    CHECK(p.stdout_tail.empty() && p.stdout_fd < 0);
}

void test_stderr_bound() {
    SpawnOptions o = sh("i=0; while [ $i -lt 2000 ]; do echo 0123456789abcdefghij line-$i >&2; i=$((i+1)); done");
    o.stderr_limit = 100;
    Process p;
    std::string err;
    CHECK(spawn(o, p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 0);
    CHECK(p.stderr_tail.size() == 100);
    CHECK(p.stderr_tail.find("line-1999\n") == p.stderr_tail.size() - 10);  // the last line is what survives
}

void test_timeout_kills_group() {
    SpawnOptions o = sh("sleep 30 & echo $! >&2; wait");
    o.timeout_ms = 300;
    Process p;
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(spawn(o, p, err));
    ExitStatus st = wait(p);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(st.timed_out && !st.exited && st.signal == SIGKILL);
    CHECK(ms >= 300 && ms < 10000);
    long grandchild = pid_from(p.stderr_tail);
    CHECK(grandchild > 1);
    CHECK(is_gone(grandchild));
}

void test_kill_group() {
    Process p;
    std::string err;
    CHECK(spawn(sh("sleep 30 & echo $! >&2; wait"), p, err));
    ExitStatus st;
    while (p.stderr_tail.find('\n') == std::string::npos && !poll(p, st))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    long grandchild = pid_from(p.stderr_tail);
    CHECK(grandchild > 1);
    CHECK(kill(pid_t(grandchild), 0) == 0);
    kill_group(p);
    st = wait(p);
    CHECK(!st.exited && st.signal == SIGKILL && !st.timed_out);
    CHECK(is_gone(grandchild));
    kill_group(p);  // safe after the child was reaped
}

// A helper process spawns a child and dies; the death signal must take the child with it.
void test_parent_death() {
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t helper = fork();
    if (helper == 0) {
        close(fds[0]);
        Process p;
        std::string err;
        if (!spawn(sh("echo $$ >&2; exec sleep 30"), p, err)) _exit(2);
        ExitStatus st;
        while (p.stderr_tail.find('\n') == std::string::npos && !poll(p, st))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        long pid = pid_from(p.stderr_tail);
        if (write(fds[1], &pid, sizeof pid) != ssize_t(sizeof pid)) _exit(3);
        _exit(0);  // without waiting for or killing the child
    }
    close(fds[1]);
    long child = 0;
    CHECK(read(fds[0], &child, sizeof child) == ssize_t(sizeof child));
    close(fds[0]);
    int ws = 0;
    CHECK(waitpid(helper, &ws, 0) == helper && WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
    CHECK(child > 1);
#ifdef __linux__
    CHECK(is_gone(child));
#else
    if (child > 1) kill(pid_t(child), SIGKILL);
#endif
}

}  // namespace

int main() {
    test_exit_code_and_signal();
    test_env_cwd_tmpdir();
    test_stdout_capture();
    test_stderr_bound();
    test_timeout_kills_group();
    test_kill_group();
    test_parent_death();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("ok");
    return 0;
}
