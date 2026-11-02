// SPDX-License-Identifier: GPL-3.0-or-later
// T-735: the Windows unhandled-exception filter of crash_trace (src/host/crash_trace_win32.cpp). Cross-built, run
// under Wine (tools/container/run_wine.sh). No game needed.
//
// The program runs itself: "child <n>" installs the crash trace, then the crash report (in that order, as host.cpp
// does with the trace first), sets phase n and dereferences null; the parent runs one child per phase with
// DON_USER_DIR pointing at a fresh folder, its stderr redirected to a file, and then checks that the line
// "[don] fatal: EXCEPTION_ACCESS_VIOLATION during <phase>" is in that file and in don.log once, and that the crash
// report ("[don] crash: exception 0x...") still ran.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "crash_report.h"
#include "crash_trace.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

const char *const kPhases[] = {don_host::kPhaseSetup, don_host::kPhasePrepare, don_host::kPhaseGame};

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t count(const std::string &text, const std::string &part)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size()))
        ++n;
    return n;
}

// The child: the trace is installed, the phase is set, and the program dies of a null dereference.
int run_child(int phase_index)
{
    don_host::crash_trace_install();
    don_crash_report_install();
    don_host::crash_trace_set_phase(kPhases[phase_index]);
    volatile int *null_pointer = reinterpret_cast<volatile int *>(static_cast<std::uintptr_t>(0));
    *null_pointer = 1;
    return 0;
}

std::filesystem::path own_path()
{
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    return std::filesystem::path(std::wstring(buffer, length));
}

// Runs "<this program> child <n>" with stderr in `stderr_file`; returns the exit code (or ~0 when it did not start).
DWORD run_child_process(int phase_index, const std::filesystem::path &stderr_file)
{
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE err = CreateFileW(stderr_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (err == INVALID_HANDLE_VALUE)
        return ~DWORD{0};

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = err;

    const std::wstring program = own_path().wstring();
    std::wstring command = L"\"" + program + L"\" child " + std::to_wstring(phase_index);
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &info);
    CloseHandle(err);
    if (!started)
        return ~DWORD{0};
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = ~DWORD{0};
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return code;
}

void check_phase(int phase_index, const std::filesystem::path &work)
{
    const std::string phase = kPhases[phase_index];
    const std::filesystem::path user = work / ("user-" + std::to_string(phase_index));
    std::filesystem::remove_all(user);
    std::filesystem::create_directories(user);
    SetEnvironmentVariableW(L"DON_USER_DIR", user.wstring().c_str());

    const std::filesystem::path stderr_file = work / ("stderr-" + std::to_string(phase_index) + ".txt");
    const DWORD exit_code = run_child_process(phase_index, stderr_file);
    std::printf("     phase '%s': child exit code 0x%08lX\n", phase.c_str(), static_cast<unsigned long>(exit_code));

    const std::string expected = "[don] fatal: EXCEPTION_ACCESS_VIOLATION during " + phase + "\n";
    const std::string on_stderr = read_file(stderr_file);
    const std::string in_log = read_file(user / "don.log");
    check(count(on_stderr, expected) == 1, "stderr has the fatal line once for phase '" + phase + "'");
    check(count(in_log, expected) == 1, "don.log has the fatal line once for phase '" + phase + "'");
    check(count(on_stderr, "[don] crash: exception 0x") == 1, "the crash report still ran once for phase '" + phase + "'");
    check(exit_code != 0 && exit_code != ~DWORD{0}, "the child still dies (the exception is not swallowed)");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "child")
        return run_child(std::stoi(argv[2]));

    const std::filesystem::path work = std::filesystem::temp_directory_path() / "don-test-crash-trace-win32";
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work);
    for (int i = 0; i < 3; ++i)
        check_phase(i, work);
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
