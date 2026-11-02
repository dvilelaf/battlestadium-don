// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_trace.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include "user_dir.h"

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace don_host
{
#ifdef _WIN32
    // crash_trace_win32.cpp
    void crash_trace_win32_install(const char *const volatile *phase);
#endif

    namespace
    {
        const char *volatile g_phase = kPhaseSetup;
        char g_line[512];

        // Appends `s` to g_line at `*n` (truncating); no allocation.
        void put(std::size_t *n, const char *s)
        {
            while (*s != '\0' && *n + 2 < sizeof g_line)
                g_line[(*n)++] = *s++;
        }

        void build_line(std::size_t *n, const char *what)
        {
            *n = 0;
            put(n, "[don] fatal: ");
            put(n, what);
            put(n, " during ");
            put(n, g_phase);
            g_line[(*n)++] = '\n';
        }

#ifndef _WIN32
        int g_stderr_fd = -1; // the stderr the program started with (fd 2 later goes through the log tee)
        int g_log_fd = -1;    // <user>/don.log opened for append

        const char *signal_name(int sig)
        {
            switch (sig)
            {
            case SIGSEGV: return "SIGSEGV";
            case SIGABRT: return "SIGABRT";
            case SIGBUS: return "SIGBUS";
            case SIGFPE: return "SIGFPE";
            case SIGILL: return "SIGILL";
            default: return "SIG?";
            }
        }

        void emit(const char *what)
        {
            std::size_t n = 0;
            build_line(&n, what);
            if (g_stderr_fd >= 0)
                (void)!::write(g_stderr_fd, g_line, n);
            if (g_log_fd >= 0)
                (void)!::write(g_log_fd, g_line, n);
        }

        void on_signal(int sig)
        {
            emit(signal_name(sig));
            ::signal(sig, SIG_DFL);
            ::raise(sig);
        }

        alignas(16) char g_alt_stack[64 * 1024];

        void install_signals()
        {
            g_stderr_fd = ::dup(2);
            std::error_code ec;
            const std::filesystem::path log = don_user_dir() / "don.log";
            std::filesystem::create_directories(log.parent_path(), ec);
            g_log_fd = ::open(log.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);

            stack_t ss{};
            ss.ss_sp = g_alt_stack;
            ss.ss_size = sizeof g_alt_stack;
            ::sigaltstack(&ss, nullptr);
            struct sigaction sa{};
            sa.sa_handler = &on_signal;
            sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
            sigemptyset(&sa.sa_mask);
            for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
                ::sigaction(sig, &sa, nullptr);
        }

        void emit_terminate(const char *what)
        {
            emit(what);
            // The default action follows, without a second line from the SIGABRT handler.
            ::signal(SIGABRT, SIG_DFL);
        }
#else
        // Windows: the unhandled-exception filter of crash_trace_win32.cpp, which reads g_phase when it writes.
        void install_signals()
        {
            crash_trace_win32_install(&g_phase);
        }
        void emit_terminate(const char *what)
        {
            std::size_t n = 0;
            build_line(&n, what);
            (void)std::fwrite(g_line, 1, n, stderr);
        }
#endif

        void on_terminate()
        {
            const std::exception_ptr ep = std::current_exception();
            std::string what = "uncaught exception";
            if (ep)
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception &e)
                {
                    what = e.what();
                }
                catch (...)
                {
                    what = "unknown exception";
                }
            }
            emit_terminate(what.c_str());
            std::abort();
        }
    }

    void crash_trace_install()
    {
        try
        {
            install_signals();
            std::set_terminate(&on_terminate);
        }
        catch (...)
        {
        }
    }

    void crash_trace_set_phase(const char *phase)
    {
        g_phase = phase;
    }

    void crash_trace_diag(const char *which)
    {
        const char *want = std::getenv("DON_DIAG_CRASH");
        if (want == nullptr || std::strcmp(want, which) != 0)
            return;
        std::fprintf(stderr, "[don] DON_DIAG_CRASH=%s: raising SIGSEGV\n", which);
        std::fflush(stderr);
        ::raise(SIGSEGV);
    }
}
