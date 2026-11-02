// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX half of the crash report (see crash_report.h): sigaction on an alternate stack for the signals
// that mean the program is dying, so that a run that crashes leaves a readable block on stderr and straight
// in <user>/don.log (write(2) to the log fd and to stderr, the pipe tee may not run again). The writing is
// async-signal-safe (write(2) into fixed stack buffers, no allocation); the frame addresses come from
// backtrace() and their module names from dladdr(), which do not allocate either. The handler then re-raises
// the signal with the default disposition so the process dies with the signal it got.
#ifndef _WIN32

#include "crash_report.h"
#include "log_redirect.h"

#include <cstdint>

#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

namespace
{
    // Writes the text straight to the log file (the tee thread may never run again after a crash) and to
    // stderr, so a terminal, a test's pipe or a script still sees the block. Both writes are async-signal-
    // safe; the log fd is a plain int read from a static.
    void raw_write(const char *text)
    {
        std::size_t n = 0;
        while (text[n] != '\0')
            ++n;
        const int log_fd = don_log_fd();
        if (log_fd >= 0)
        {
            const ssize_t ignored = ::write(log_fd, text, n);
            (void)ignored;
        }
        // The original stderr, not fd 2: fd 2 goes through the tee and would put the block in the log twice.
        // Without a log (it could not be opened) fd 2 is still the original stream.
        const int err_fd = log_fd >= 0 ? don_log_original_stderr_fd() : 2;
        if (err_fd >= 0)
        {
            const ssize_t ignored = ::write(err_fd, text, n);
            (void)ignored;
        }
    }

    // Writes `value` as unsigned decimal into `out` (room for at least 21 chars + NUL) and returns the
    // number of digits (not counting the NUL).
    unsigned write_decimal(char *out, unsigned long long value)
    {
        char tmp[24];
        unsigned i = 0;
        do
        {
            tmp[i++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        } while (value != 0);
        const unsigned n = i;
        while (i > 0)
            *out++ = tmp[--i];
        *out = '\0';
        return n;
    }

    void write_hex(char *out, unsigned long long value)
    {
        static constexpr char digits[] = "0123456789abcdef";
        char tmp[16];
        unsigned i = 0;
        do
        {
            tmp[i++] = digits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        *out++ = '0';
        *out++ = 'x';
        while (i > 0)
            *out++ = tmp[--i];
        *out = '\0';
    }

    const char *signal_name(int sig)
    {
        switch (sig)
        {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "SIG?";
        }
    }

    void write_frame(unsigned index, const void *addr)
    {
        char line[800];
        char *w = line;
        const char *prefix = "[don] crash: #";
        while (*prefix != '\0')
            *w++ = *prefix++;
        w += write_decimal(w, static_cast<unsigned long long>(index));
        *w++ = ' ';
        Dl_info info;
        if (::dladdr(addr, &info) != 0 && info.dli_fname != nullptr && info.dli_fbase != nullptr)
        {
            // The module's file name (everything after the last '/').
            const char *base = info.dli_fname;
            const char *slash = base;
            for (const char *p = base; *p != '\0'; ++p)
                if (*p == '/')
                    slash = p + 1;
            while (*slash != '\0' && static_cast<std::size_t>(w - line) < sizeof(line) - 32)
                *w++ = *slash++;
            *w++ = '+';
            char hexbuf[32];
            write_hex(hexbuf, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr) -
                                                               reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
            for (const char *p = hexbuf; *p != '\0'; ++p)
                *w++ = *p;
        }
        else
        {
            char hexbuf[32];
            write_hex(hexbuf, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(addr)));
            for (const char *p = hexbuf; *p != '\0'; ++p)
                *w++ = *p;
        }
        *w++ = '\n';
        *w = '\0';
        raw_write(line);
    }

    void crash_handler(int sig, siginfo_t *info, void * /*context*/)
    {
        char line[512];
        char *w = line;
        const char *p = "[don] crash: signal ";
        while (*p != '\0')
            *w++ = *p++;
        w += write_decimal(w, static_cast<unsigned long long>(sig));
        const char *name = signal_name(sig);
        *w++ = ' ';
        *w++ = '(';
        while (*name != '\0')
            *w++ = *name++;
        *w++ = ')';
        p = ", fault address ";
        while (*p != '\0')
            *w++ = *p++;
        char hexbuf[32];
        write_hex(hexbuf, info != nullptr ? reinterpret_cast<unsigned long long>(info->si_addr)
                                          : static_cast<unsigned long long>(0));
        for (const char *q = hexbuf; *q != '\0'; ++q)
            *w++ = *q;
        *w++ = '\n';
        *w = '\0';
        raw_write(line);

        void *frames[32];
        const int count = ::backtrace(frames, 32);
        for (int i = 0; i < count; ++i)
            write_frame(static_cast<unsigned>(i), frames[i]);

        // Re-raise with the default disposition so the process dies with this signal (and the exit code /
        // core dump the shell and the tests expect).
        ::signal(sig, SIG_DFL);
        ::raise(sig);
    }

    // 64 KiB is plenty for the biggest XSAVE area; a fixed buffer sidesteps SIGSTKSZ being a sysconf()
    // call (not a compile-time constant) since glibc 2.34.
    alignas(16) unsigned char g_altstack[65536];
} // namespace

void don_crash_report_install()
{
    stack_t ss{};
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof(g_altstack);
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = &crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    ::sigaction(SIGILL, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
}

#endif // !_WIN32
