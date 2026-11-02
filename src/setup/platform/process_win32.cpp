// Windows implementation of platform/process.h (task T-732), twin of process_posix.cpp.
//   * the command line is built from argv with the rules of Microsoft's "Parsing C++ command-line arguments"
//     (https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments): an argument is quoted when it is
//     empty or holds a space, tab or quote; backslashes are doubled only when they precede a quote (an embedded one or
//     the closing one); a quote is escaped with a backslash;
//   * the group is a Job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: kill_group() is TerminateJobObject, and if the
//     setup dies the system kills the whole tree;
//   * stderr/stdout are anonymous pipes read without blocking (PeekNamedPipe before ReadFile).
#include "platform/process.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace don_setup {

namespace {

constexpr UINT kKilledExitCode = 137;  // 128 + SIGKILL, the shell convention; what TerminateJobObject sets
constexpr DWORD kWaitSliceMs = 20;     // longest sleep between checks while waiting

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string last_error(const char* what) {
    return std::string(what) + ": error " + std::to_string(GetLastError());
}

void append_quoted(std::wstring& out, const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += a;
        return;
    }
    out += L'"';
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');  // backslashes before a quote are doubled, the quote escaped
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');  // before the closing quote
    out += L'"';
}

std::wstring command_line(const std::vector<std::string>& argv) {
    std::wstring cl;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cl += L' ';
        append_quoted(cl, widen(argv[i]));
    }
    return cl;
}

// "NAME=value\0NAME=value\0\0", sorted by name without case as CreateProcess documents for Unicode blocks.
std::wstring environment_block(std::vector<std::string> envs) {
    std::vector<std::wstring> w;
    for (const auto& e : envs) w.push_back(widen(e));
    std::sort(w.begin(), w.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& e : w) {
        block += e;
        block += L'\0';
    }
    block += L'\0';
    if (w.empty()) block += L'\0';  // an empty block still needs two terminators
    return block;
}

// Appends what is readable now to `tail` (keeping the last `limit` bytes). Closes the handle at end of stream.
void drain_handle(void*& handle, std::string& tail, size_t limit) {
    if (!handle) return;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &avail, nullptr)) {  // broken pipe: every writer is gone
            CloseHandle(handle);
            handle = nullptr;
            return;
        }
        if (avail == 0) return;
        DWORD got = 0;
        if (!ReadFile(handle, buf, DWORD(std::min<size_t>(avail, sizeof buf)), &got, nullptr) || got == 0) return;
        tail.append(buf, got);
        if (tail.size() > limit) tail.erase(0, tail.size() - limit);
    }
}

void drain(Process& p) {
    drain_handle(p.stderr_handle, p.stderr_tail, p.stderr_limit);
    drain_handle(p.stdout_handle, p.stdout_tail, p.stdout_limit);
}

void close_handle(void*& h) {
    if (h) CloseHandle(h);
    h = nullptr;
}

// Pipe whose read end is private to us and whose write end the child inherits.
bool make_pipe(HANDLE& read_end, HANDLE& write_end) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    return true;
}

}  // namespace

bool spawn(const SpawnOptions& opts, Process& out, std::string& error) {
    if (opts.argv.empty()) {
        error = "spawn: empty argv";
        return false;
    }
    std::vector<std::string> envs = opts.env;
    if (!opts.tmpdir.empty()) {  // TMPDIR is the POSIX name; Windows tools read TMP and TEMP
        envs.push_back("TMPDIR=" + opts.tmpdir);
        envs.push_back("TMP=" + opts.tmpdir);
        envs.push_back("TEMP=" + opts.tmpdir);
    }
    std::wstring app = widen(opts.argv[0]);
    std::wstring cmd = command_line(opts.argv);  // CreateProcessW may modify this buffer
    std::wstring env = environment_block(envs);
    std::wstring cwd = widen(opts.cwd);

    HANDLE err_r = nullptr, err_w = nullptr, out_r = nullptr, out_w = nullptr;
    if (!make_pipe(err_r, err_w)) {
        error = last_error("CreatePipe");
        return false;
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    // stdin is NUL, stdout is NUL unless captured.
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_out = nullptr;
    if (opts.stdout_limit > 0) {
        if (!make_pipe(out_r, out_w)) {
            error = last_error("CreatePipe");
            CloseHandle(err_r);
            CloseHandle(err_w);
            if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
            return false;
        }
    } else {
        nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        out_w = nul_out;
    }

    auto cleanup_child_ends = [&] {
        CloseHandle(err_w);
        if (out_w != INVALID_HANDLE_VALUE && out_w) CloseHandle(out_w);
        if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    };

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        error = last_error("CreateJobObject");
        cleanup_child_ends();
        CloseHandle(err_r);
        if (out_r) CloseHandle(out_r);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    // Only the three standard handles are inherited, not every inheritable handle of the setup.
    HANDLE inherit[3] = {nul_in, out_w, err_w};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul_in;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;

    // Suspended until it is in the job, so no descendant can escape it.
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                  (opts.nice > 0 ? BELOW_NORMAL_PRIORITY_CLASS : 0) | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(app.c_str(), &cmd[0], nullptr, nullptr, TRUE, flags, &env[0],
                             cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    std::string why = ok ? std::string() : last_error("CreateProcess");
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    cleanup_child_ends();
    if (!ok) {
        error = why;
        CloseHandle(job);
        CloseHandle(err_r);
        if (out_r) CloseHandle(out_r);
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        error = last_error("AssignProcessToJobObject");
        TerminateProcess(pi.hProcess, kKilledExitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        CloseHandle(err_r);
        if (out_r) CloseHandle(out_r);
        return false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    out = Process{};
    out.pid = int(pi.dwProcessId);
    out.proc_handle = pi.hProcess;
    out.job_handle = job;
    out.stderr_handle = err_r;
    out.stderr_limit = opts.stderr_limit;
    out.stdout_handle = out_r;
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
    if (WaitForSingleObject(p.proc_handle, 0) != WAIT_OBJECT_0) return false;
    p.finished = true;
    DWORD code = 0;
    if (GetExitCodeProcess(p.proc_handle, &code)) {
        if (p.killed && code == kKilledExitCode) {
            p.status.signal = 9;
        } else {
            p.status.exited = true;
            p.status.code = int(code);
        }
    }
    p.status.timed_out = p.timed_out;
    drain(p);
    // Descendants may still hold the pipes; we have what the child wrote. Closing the job (KILL_ON_JOB_CLOSE) also
    // ends any descendant left behind.
    close_handle(p.stderr_handle);
    close_handle(p.stdout_handle);
    close_handle(p.job_handle);
    close_handle(p.proc_handle);
    status = p.status;
    return true;
}

ExitStatus wait(Process& p) {
    ExitStatus st;
    while (!poll(p, st)) {
        if (p.proc_handle) WaitForSingleObject(p.proc_handle, kWaitSliceMs);
        else Sleep(kWaitSliceMs);
    }
    return st;
}

void kill_group(Process& p) {
    if (!p.job_handle) return;  // already reaped, which also closed the job
    p.killed = true;
    TerminateJobObject(p.job_handle, kKilledExitCode);
}

}  // namespace don_setup
