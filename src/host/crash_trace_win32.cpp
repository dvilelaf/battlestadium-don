// SPDX-License-Identifier: GPL-3.0-or-later
// T-735: the Windows half of crash_trace.h. An unhandled-exception filter writes the same line as the POSIX
// handlers, "[don] fatal: <exception name> during <phase>", to the stderr the program started with and to
// <user>/don.log. Then it hands the exception on: to the top-level filter that was installed before it (the
// one crash_report_win32.cpp installs, when that came first), or to the system (EXCEPTION_CONTINUE_SEARCH).
// Crashed-process rules: only WriteFile on handles opened here at install, a static buffer, snprintf into it.
#ifdef _WIN32

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "user_dir.h"

namespace don_host
{
    namespace
    {
        const char *const volatile *g_phase = nullptr;
        HANDLE g_stderr = INVALID_HANDLE_VALUE;   // a duplicate of the stderr the program started with
        HANDLE g_log = INVALID_HANDLE_VALUE;      // <user>/don.log, opened for append
        LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
        volatile LONG g_reported = 0;             // the line is written once, as SA_RESETHAND does on POSIX
        char g_line[512];

        // The names of the exceptions crash_report_win32.cpp calls fatal, plus the others that can escape.
        const char *exception_name(DWORD code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
            case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
            case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
            case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
            case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
            case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
            default: return nullptr;
            }
        }

        void write_all(HANDLE handle, const char *text, DWORD n)
        {
            if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
                return;
            DWORD written = 0;
            ::WriteFile(handle, text, n, &written, nullptr);
        }

        void emit(DWORD code)
        {
            char hex_name[32];
            const char *what = exception_name(code);
            if (what == nullptr)
            {
                std::snprintf(hex_name, sizeof hex_name, "EXCEPTION_0x%08lX", static_cast<unsigned long>(code));
                what = hex_name;
            }
            const int n = std::snprintf(g_line, sizeof g_line, "[don] fatal: %s during %s\n", what, *g_phase);
            if (n <= 0)
                return;
            const DWORD len = static_cast<DWORD>(n < static_cast<int>(sizeof g_line) ? n : sizeof g_line - 1);
            write_all(g_stderr, g_line, len);
            write_all(g_log, g_line, len);
        }

        LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *info)
        {
            if (info != nullptr && info->ExceptionRecord != nullptr && ::InterlockedExchange(&g_reported, 1) == 0)
                emit(info->ExceptionRecord->ExceptionCode);
            // Chain: the filter installed before this one decides, as it would have without us. With none, the
            // system runs its own handling.
            if (g_previous != nullptr)
                return g_previous(info);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        HANDLE open_log_for_append(const std::filesystem::path &path)
        {
            // FILE_APPEND_DATA: every WriteFile goes to the end. The other openers (the redirect of
            // log_redirect.cpp) may share the file; the share flags keep them from failing.
            return ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    } // namespace

    // Called by crash_trace_install() (crash_trace.cpp) with the phase variable it reads when it writes the line.
    void crash_trace_win32_install(const char *const volatile *phase)
    {
        g_phase = phase;
        const HANDLE process = ::GetCurrentProcess();
        const HANDLE original_stderr = ::GetStdHandle(STD_ERROR_HANDLE);
        if (original_stderr != nullptr && original_stderr != INVALID_HANDLE_VALUE)
            ::DuplicateHandle(process, original_stderr, process, &g_stderr, 0, FALSE, DUPLICATE_SAME_ACCESS);

        std::error_code ec;
        const std::filesystem::path log = don_user_dir() / "don.log";
        std::filesystem::create_directories(log.parent_path(), ec);
        g_log = open_log_for_append(log);

        g_previous = ::SetUnhandledExceptionFilter(&unhandled_filter);
    }
} // namespace don_host

#endif // _WIN32
