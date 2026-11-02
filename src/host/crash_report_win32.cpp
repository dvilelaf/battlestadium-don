// SPDX-License-Identifier: GPL-3.0-or-later
// Windows half of the crash report (see crash_report.h). Installs three reporters, all writing to stderr
// (which don_log_redirect has sent to don.log when the program was double-clicked):
//   * a first-chance vectored exception handler, which reports the fatal hardware exceptions (access
//     violation, illegal instruction, division by zero, stack overflow, ...) and lets the search continue;
//   * SetUnhandledExceptionFilter, which reports whatever escaped unhandled (a C++ exception that got past
//     the runtime, or a fatal code on a path the vectored handler missed) and then terminates without the
//     Windows Error Reporting dialog;
//   * std::set_terminate, which reports an uncaught C++ exception before the previous terminate runs.
// One block per death: "[don] crash: exception 0x<code> at <module>+0x<offset> thread <id>" and a short
// stack of <module>+0x<offset> frames (CaptureStackBackTrace, no symbols). No allocation, no file or
// minidump writing: everything uses fixed buffers, written straight to don_log_fd() and to stderr with
// _write (safe enough even for a stack overflow, and the pipe thread may not run again after a crash).
#ifdef _WIN32

#include "crash_report.h"
#include "log_redirect.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>

namespace
{
    std::atomic<bool> g_crash_reported{false};
    std::atomic<bool> g_terminate_reported{false};
    std::terminate_handler g_previous_terminate = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr; // installed before this one (T-735: crash_trace)

    bool is_fatal(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
            return true;
        default:
            return false;
        }
    }

    // "<module>+0x<offset>", or the raw address when the module cannot be resolved. Fixed buffers only.
    void format_module_offset(std::uintptr_t addr, char *out, std::size_t cap)
    {
        HMODULE mod = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(addr), &mod) &&
            mod != nullptr)
        {
            wchar_t wpath[MAX_PATH];
            const DWORD n = ::GetModuleFileNameW(mod, wpath, MAX_PATH);
            if (n != 0 && n < MAX_PATH)
            {
                const wchar_t *base = wpath + n;
                while (base > wpath && base[-1] != L'\\' && base[-1] != L'/')
                    --base;
                char narrow[MAX_PATH];
                const int m = ::WideCharToMultiByte(CP_UTF8, 0, base, -1, narrow, MAX_PATH, nullptr, nullptr);
                std::snprintf(out, cap, "%s+0x%llx", m > 0 ? narrow : "?",
                              static_cast<unsigned long long>(addr - reinterpret_cast<std::uintptr_t>(mod)));
                return;
            }
        }
        std::snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(addr));
    }

    // Straight to the log file and to the original stderr (not stderr's descriptor: don_log_redirect has
    // pointed it at the tee, which would put the block in the log twice).
    void write_line(const char *text)
    {
        const std::size_t n = std::strlen(text);
        const int log_fd = don_log_fd();
        if (log_fd >= 0)
            ::_write(log_fd, text, static_cast<unsigned>(n));
        const int err_fd = log_fd >= 0 ? don_log_original_stderr_fd() : 2;
        if (err_fd >= 0)
            ::_write(err_fd, text, static_cast<unsigned>(n));
    }

    void write_frames()
    {
        void *frames[32];
        const USHORT count = ::CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            char where[512];
            format_module_offset(reinterpret_cast<std::uintptr_t>(frames[i]), where, sizeof(where));
            char line[576];
            std::snprintf(line, sizeof(line), "[don] crash: #%u %s\n", static_cast<unsigned>(i), where);
            write_line(line);
        }
    }

    void report_seh(DWORD code, std::uintptr_t fault_addr)
    {
        bool expected = false;
        if (!g_crash_reported.compare_exchange_strong(expected, true))
            return;
        char where[512];
        format_module_offset(fault_addr, where, sizeof(where));
        char line[576];
        std::snprintf(line, sizeof(line), "[don] crash: exception 0x%08lX at %s thread %lu\n",
                      static_cast<unsigned long>(code), where, static_cast<unsigned long>(::GetCurrentThreadId()));
        write_line(line);
        write_frames();
    }

    LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *info)
    {
        if (info != nullptr && info->ExceptionRecord != nullptr &&
            is_fatal(info->ExceptionRecord->ExceptionCode))
        {
            report_seh(info->ExceptionRecord->ExceptionCode,
                       reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *info)
    {
        if (info != nullptr && info->ExceptionRecord != nullptr)
        {
            report_seh(info->ExceptionRecord->ExceptionCode,
                       reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress));
        }
        // T-735: the filter installed before this one still runs (crash_trace_win32.cpp writes its fatal line
        // there). Its verdict is not used: this filter decides the end of the process.
        if (g_previous_filter != nullptr)
            g_previous_filter(info);
        // Handled here: terminate without the Windows Error Reporting dialog.
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // "terminate_handler" is also a CRT typedef, so the name is qualified.
    void crash_terminate_handler()
    {
        bool expected = false;
        if (g_terminate_reported.compare_exchange_strong(expected, true))
        {
            char line[96];
            std::snprintf(line, sizeof(line), "[don] crash: std::terminate called, thread %lu\n",
                          static_cast<unsigned long>(::GetCurrentThreadId()));
            write_line(line);
            write_frames();
        }
        if (g_previous_terminate != nullptr)
            g_previous_terminate();
        std::abort();
    }
} // namespace

void don_crash_report_install()
{
    g_previous_filter = ::SetUnhandledExceptionFilter(&unhandled_filter);
    ::AddVectoredExceptionHandler(1 /* first */, &vectored_handler);
    g_previous_terminate = std::set_terminate(&crash_terminate_handler);
}

#endif // _WIN32
