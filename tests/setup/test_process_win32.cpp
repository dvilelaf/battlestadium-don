// Windows process twin (task T-732): built only for Windows (cross-built, run under Wine). The test program is its own
// helper: `--echo-args ...` prints its argv, `--mid <file>` starts `--leaf <file>` and sleeps, `--leaf <file>` writes
// its process id to <file> and sleeps.
#include "platform/process.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace don_setup;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr DWORD kSleepMs = 120000;

std::string self_path() {
    char buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return std::string(buf, n);
}

std::string system_dir() {
    char buf[MAX_PATH];
    UINT n = GetSystemDirectoryA(buf, sizeof buf);
    return std::string(buf, n);
}

std::vector<std::string> base_env() {
    const char* names[] = {"SystemRoot", "PATH", "WINEPREFIX", "WINEDEBUG"};
    std::vector<std::string> env;
    for (const char* n : names) {
        const char* v = std::getenv(n);
        if (v) env.push_back(std::string(n) + "=" + v);
    }
    return env;
}

SpawnOptions options(std::vector<std::string> argv, size_t stdout_limit = 4096) {
    SpawnOptions o;
    o.argv = std::move(argv);
    o.env = base_env();
    o.stdout_limit = stdout_limit;
    return o;
}

std::string normalize(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

bool process_gone(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;
    bool gone = WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}

DWORD read_pid_file(const std::string& path) {
    for (int i = 0; i < 200; ++i) {
        std::ifstream f(path);
        DWORD pid = 0;
        if (f >> pid && pid != 0) return pid;
        Sleep(50);
    }
    return 0;
}

void test_cmd_echo() {
    Process p;
    std::string err;
    CHECK(spawn(options({system_dir() + "\\cmd.exe", "/c", "echo", "hello"}), p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 0 && !st.signal && !st.timed_out);
    CHECK(normalize(p.stdout_tail) == "hello\n");
}

void test_exit_codes() {
    for (int code : {0, 3, 42}) {
        Process p;
        std::string err;
        CHECK(spawn(options({system_dir() + "\\cmd.exe", "/c", "exit", std::to_string(code)}), p, err));
        ExitStatus st = wait(p);
        CHECK(st.exited && st.code == code);
    }
    Process p;
    std::string err;
    CHECK(!spawn(options({"C:\\no\\such\\program.exe"}), p, err));
    CHECK(!err.empty());
}

void test_stderr_capture() {
    Process p;
    std::string err;
    CHECK(spawn(options({system_dir() + "\\cmd.exe", "/c", "echo oops 1>&2"}, 0), p, err));
    wait(p);
    CHECK(normalize(p.stderr_tail).find("oops") == 0);
    CHECK(p.stdout_tail.empty());
}

void test_arguments_intact() {
    const std::vector<std::string> args = {"with space", "quo\"te", "back\\", "trail \\", "", "a\\\"b", "C:\\Program Files\\x\\",
                                           "tab\there", "plain"};
    std::vector<std::string> argv = {self_path(), "--echo-args"};
    argv.insert(argv.end(), args.begin(), args.end());
    Process p;
    std::string err;
    CHECK(spawn(options(argv), p, err));
    ExitStatus st = wait(p);
    CHECK(st.exited && st.code == 0);
    std::string want;
    for (const auto& a : args) want += "[" + a + "]\n";
    CHECK(normalize(p.stdout_tail) == want);
    if (normalize(p.stdout_tail) != want) std::printf("got:\n%s\nwant:\n%s\n", p.stdout_tail.c_str(), want.c_str());
}

void test_job_stop_no_orphan() {
    std::string pidfile = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\t732_leaf.pid";
    std::remove(pidfile.c_str());
    Process p;
    std::string err;
    CHECK(spawn(options({self_path(), "--mid", pidfile}), p, err));
    DWORD leaf = read_pid_file(pidfile);
    CHECK(leaf != 0);
    ExitStatus st;
    CHECK(!poll(p, st));  // still running
    kill_group(p);
    st = wait(p);
    CHECK(!st.exited && st.signal == 9);
    CHECK(leaf != 0 && process_gone(leaf));  // the grandchild died with the job
    std::remove(pidfile.c_str());
}

void test_timeout_kills_group() {
    std::string pidfile = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\t732_leaf2.pid";
    std::remove(pidfile.c_str());
    SpawnOptions o = options({self_path(), "--mid", pidfile});
    o.timeout_ms = 1500;
    Process p;
    std::string err;
    CHECK(spawn(o, p, err));
    ExitStatus st = wait(p);
    CHECK(st.timed_out && !st.exited && st.signal == 9);
    DWORD leaf = read_pid_file(pidfile);
    CHECK(leaf != 0 && process_gone(leaf));
    std::remove(pidfile.c_str());
}

void test_cwd_and_env() {
    SpawnOptions o = options({system_dir() + "\\cmd.exe", "/c", "echo %DON_T732% & cd"});
    o.env.push_back("DON_T732=value");
    o.cwd = system_dir();
    Process p;
    std::string err;
    CHECK(spawn(o, p, err));
    wait(p);
    CHECK(normalize(p.stdout_tail).find("value") == 0);
    CHECK(normalize(p.stdout_tail).find(system_dir()) != std::string::npos);
}

int helper_main(int argc, char** argv) {
    if (std::strcmp(argv[1], "--echo-args") == 0) {
        for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
        return 0;
    }
    if (std::strcmp(argv[1], "--leaf") == 0 && argc > 2) {
        std::ofstream(argv[2]) << GetCurrentProcessId() << "\n";
        Sleep(kSleepMs);
        return 0;
    }
    if (std::strcmp(argv[1], "--mid") == 0 && argc > 2) {
        // Plain CreateProcess: the leaf joins our job by inheritance, like a compiler's own children.
        std::string cmd = "\"" + self_path() + "\" --leaf \"" + argv[2] + "\"";
        STARTUPINFOA si = {};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 2;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(kSleepMs);
        return 0;
    }
    return 64;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return helper_main(argc, argv);
    test_cmd_echo();
    test_exit_codes();
    test_stderr_capture();
    test_arguments_intact();
    test_cwd_and_env();
    test_job_stop_no_orphan();
    test_timeout_kills_group();
    if (g_failures) {
        std::printf("test_process_win32: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_process_win32: all passed\n");
    return 0;
}
