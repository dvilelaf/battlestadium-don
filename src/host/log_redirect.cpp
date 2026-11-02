// SPDX-License-Identifier: GPL-3.0-or-later
// The one log implementation, both platforms (see log_redirect.h). The tee is identical everywhere;
// only the handful of file/pipe/time primitives differ, guarded by _WIN32. Replaces the old
// log_redirect_win32.cpp / log_redirect_posix.cpp twins (Windows-only redirect, POSIX no-op).
#include "log_redirect.h"

#include "build_id.h"
#include "user_dir.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <BaseTsd.h>
using ssize_t = SSIZE_T; // POSIX's signed size, which the CRT does not name
#else
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#endif

namespace fs = std::filesystem;

namespace
{
    int g_log_fd = -1;
    int g_orig_err_fd = -1;
    fs::path g_log_path;
    // The tee: stdout / stderr's descriptors, the originals they were pointed away from, and the two threads.
    int g_out_fd = -1;
    int g_err_fd = -1;
    int g_orig_out_fd = -1;
    std::thread g_out_thread;
    std::thread g_err_thread;
    std::atomic<int> g_threads_done{0};
    bool g_tee_active = false;
    std::atomic<bool> g_exit_written{false};

#ifdef _WIN32
    int raw_open_append(const fs::path &path)
    {
        return ::_wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, 0600);
    }
    ssize_t raw_write(int fd, const void *buf, size_t n) { return ::_write(fd, buf, static_cast<unsigned>(n)); }
    ssize_t raw_read(int fd, void *buf, size_t n) { return ::_read(fd, buf, static_cast<unsigned>(n)); }
    int raw_pipe(int fds[2]) { return ::_pipe(fds, 65536, _O_BINARY); }
    int raw_dup(int fd) { return ::_dup(fd); }
    int raw_dup2(int src, int dst) { return ::_dup2(src, dst); }
    int raw_close(int fd) { return ::_close(fd); }
    int raw_open_null() { return ::_open("NUL", _O_WRONLY | _O_BINARY); }
    int process_id() { return static_cast<int>(::GetCurrentProcessId()); }
    void local_time_string(char *out, size_t cap)
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u", static_cast<unsigned>(st.wYear),
                      static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
                      static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                      static_cast<unsigned>(st.wSecond));
    }
#else
    int raw_open_append(const fs::path &path)
    {
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    }
    ssize_t raw_write(int fd, const void *buf, size_t n) { return ::write(fd, buf, n); }
    ssize_t raw_read(int fd, void *buf, size_t n) { return ::read(fd, buf, n); }
    int raw_pipe(int fds[2]) { return ::pipe(fds); }
    int raw_dup(int fd) { return ::dup(fd); }
    int raw_dup2(int src, int dst) { return ::dup2(src, dst); }
    int raw_close(int fd) { return ::close(fd); }
    int raw_open_null() { return ::open("/dev/null", O_WRONLY); }
    int process_id() { return static_cast<int>(::getpid()); }
    void local_time_string(char *out, size_t cap)
    {
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        std::snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
    }
#endif

    // T-520: reads the last 256 KB of the log and returns the date of the previous start when that start
    // left no "[don] exit:" line (a crash, a forced close or the PC stopped). Empty otherwise.
    std::string previous_start_note(const fs::path &path)
    {
        constexpr std::uintmax_t kTailBytes = 256u * 1024u;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return {};
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size <= 0)
            return {};
        const std::streamoff start =
            size > static_cast<std::streamoff>(kTailBytes) ? size - static_cast<std::streamoff>(kTailBytes) : 0;
        std::string tail(static_cast<size_t>(size - start), '\0');
        in.seekg(start);
        in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
        in.close();

        const std::string marker = "===== Battle Stadium D.O.N started ";
        const std::size_t header = tail.rfind(marker);
        if (header == std::string::npos)
            return {};
        if (tail.find("[don] exit:", header) != std::string::npos)
            return {};
        std::string date;
        for (std::size_t i = header + marker.size(); i < tail.size() && date.size() < 19; ++i)
        {
            const char c = tail[i];
            if (c == '\r' || c == '\n' || c == '=' || c == '(')
                break;
            if (c == ' ' || c == '-' || c == ':' || (c >= '0' && c <= '9'))
                date += c;
        }
        while (!date.empty() && date.back() == ' ')
            date.pop_back();
        return date;
    }

    // Reads from one pipe end and writes each chunk to the log and to the original stream. Runs until EOF.
    void tee_loop(int read_fd, int orig_fd)
    {
        char buf[8192];
        for (;;)
        {
            const ssize_t n = raw_read(read_fd, buf, sizeof(buf));
            if (n <= 0)
            {
                g_threads_done.fetch_add(1);
                return;
            }
            if (g_log_fd >= 0)
                raw_write(g_log_fd, buf, static_cast<size_t>(n));
            if (orig_fd >= 0)
                raw_write(orig_fd, buf, static_cast<size_t>(n));
        }
    }
} // namespace

void don_log_redirect()
{
    if (g_log_fd >= 0)
        return; // already redirected

    const fs::path user = don_host::don_user_dir();
    std::error_code ec;
    fs::create_directories(user, ec);
    g_log_path = user / "don.log";

    // The "previous start did not end normally" note, read before the file is moved or appended.
    const std::string note = previous_start_note(g_log_path);

    // Past 10 MB the file becomes don.old.log first.
    constexpr std::uintmax_t kMaxLogBytes = 10u * 1024u * 1024u;
    const std::uintmax_t size = fs::file_size(g_log_path, ec);
    if (!ec && size > kMaxLogBytes)
    {
        fs::path old = g_log_path;
        old.replace_filename("don.old.log");
        std::error_code mv;
        fs::remove(old, mv); // drop the previous .old so the rename can replace it
        fs::rename(g_log_path, old, mv);
    }

    g_log_fd = raw_open_append(g_log_path);
    if (g_log_fd < 0)
        return; // cannot log: leave the streams alone

    char when[64];
    local_time_string(when, sizeof(when));
    char header[512];
    std::snprintf(header, sizeof(header), "\n===== Battle Stadium D.O.N started %s (pid %d, build %s) =====\n",
                  when, process_id(), don_build_id());
    raw_write(g_log_fd, header, std::strlen(header));
    if (!note.empty())
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[don] the previous start (%s) did not end normally: a crash, a forced close or the PC stopped\n",
                      note.c_str());
        raw_write(g_log_fd, line, std::strlen(line));
    }

#ifdef _WIN32
    // A window program started with no console has no stdout / stderr: their descriptor is -2 and the CRT
    // drops what is printed. Give them one (NUL) first, so the dup2 below redirects the streams the CRT
    // actually writes to. freopen resets the buffering: unbuffered again, as main.cpp asks on Windows.
    if (::_fileno(stdout) < 0 && std::freopen("NUL", "w", stdout) != nullptr)
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (::_fileno(stderr) < 0 && std::freopen("NUL", "w", stderr) != nullptr)
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    const int out_fd = ::_fileno(stdout);
    const int err_fd = ::_fileno(stderr);
#else
    const int out_fd = ::fileno(stdout);
    const int err_fd = ::fileno(stderr);
#endif
    if (out_fd < 0 || err_fd < 0)
        return;

    // Save the original streams, then point stdout and stderr at the two pipe write ends.
    const int orig_out = raw_dup(out_fd);
    const int orig_err = raw_dup(err_fd);
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (raw_pipe(out_pipe) != 0 || raw_pipe(err_pipe) != 0)
        return;
    raw_dup2(out_pipe[1], out_fd);
    raw_dup2(err_pipe[1], err_fd);
    // Only stdout / stderr keep the write ends: when don_log_finish points them back, the pipes end and the
    // threads see it once they have written everything.
    raw_close(out_pipe[1]);
    raw_close(err_pipe[1]);
    g_out_fd = out_fd;
    g_err_fd = err_fd;
    g_orig_out_fd = orig_out;
    g_orig_err_fd = orig_err;
    g_out_thread = std::thread([out_read = out_pipe[0], orig_out]() { tee_loop(out_read, orig_out); });
    g_err_thread = std::thread([err_read = err_pipe[0], orig_err]() { tee_loop(err_read, orig_err); });
    g_tee_active = true;
}

std::filesystem::path don_log_path() { return g_log_path; }

int don_log_fd() { return g_log_fd; }

int don_log_original_stderr_fd() { return g_orig_err_fd; }

void don_log_note_exit_written() { g_exit_written.store(true); }

bool don_log_exit_written() { return g_exit_written.load(); }

void don_log_flush()
{
    std::fflush(stdout);
    std::fflush(stderr);
}

void don_log_finish()
{
    if (!g_tee_active)
        return;
    g_tee_active = false;
    std::fflush(stdout);
    std::fflush(stderr);
    // Point stdout and stderr back at the original streams (or at the null device when there was none):
    // that closes the pipes' last write ends, so each thread writes what is left and then sees the end.
    // Waiting for that is the drain: a run that ends early (the GS does not open, an exception) kept
    // nothing of what it printed when the process ended with the threads still holding it.
    for (const auto &[orig, fd] : {std::pair{g_orig_out_fd, g_out_fd}, std::pair{g_orig_err_fd, g_err_fd}})
    {
        const int target = orig >= 0 ? orig : raw_open_null();
        if (target >= 0)
            raw_dup2(target, fd);
    }
    // A library that kept its own copy of stdout or stderr would hold a pipe open: the threads have written
    // everything that was in the pipes well within this second, and are left behind rather than waited for.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (g_threads_done.load() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (std::thread *t : {&g_out_thread, &g_err_thread})
    {
        if (g_threads_done.load() >= 2)
            t->join();
        else
            t->detach();
    }
}
