// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows sampler of src/host/profile.h (T-759): the twin of profile_linux.cpp, the only other file
// of the profiler with operating-system calls. Added to the build on every platform; where _WIN32 is not
// defined it compiles to an empty translation unit.
//
// HOW THE SAMPLING WORKS
//   Windows has no per-thread CPU-time timer that interrupts a thread, so the sampler is a second
//   thread. start_sampler() (called on the thread to sample) duplicates a real handle to the calling
//   thread (DuplicateHandle on GetCurrentThread(): the latter is only a pseudo handle that means "the
//   caller" and would mean the sampler itself in the other thread) and starts the sampler thread. About
//   every millisecond that thread
//     1. reads the target's cycle counter (QueryThreadCycleTime). If it did not advance since the last
//        sample the target was blocked (a wait, a Sleep), and no sample is stored: like the Linux timer
//        on the thread's processor-time clock, a sleeping thread contributes nothing, so counts are
//        proportional to work and not to wall time;
//     2. SuspendThread(target), GetThreadContext(target, CONTEXT_CONTROL), ResumeThread(target);
//     3. stores context.Rip with don_profile::handler_record().
//   With DON_PROFILE_CALLERS=1 (T-765) step 2 also reads the 8 bytes at context.Rsp into a local and step 3 stores
//   them as the sample's caller (see "CALLERS" below).
//   Between suspend and resume the sampler calls nothing but GetThreadContext (and, with callers, one read): the target may hold the
//   heap lock or the loader lock, and anything that needs one would deadlock. The store is done after the
//   resume and is a plain write to the preallocated buffer.
//   stop_sampler() raises a flag, wakes the sampler and joins it, so after it returns no sample can reach
//   the buffer (that is what makes freeing it in profile_finish() safe).
//
// CALLERS (T-765)
//   Where the sampled instruction is in a leaf helper that is reached by a jump (VCRUNTIME140's memmove jumps to an
//   unexported copy helper for big copies), the first stack slot at RSP holds the return address into the caller
//   of memmove, so [RSP] names who asked for the copy. The read is ReadProcessMemory on our own process: it is a
//   plain system call that reports failure instead of faulting, takes no user-mode lock (no heap, no loader lock) and
//   so is safe while the target is stopped; a plain load would also work in practice (the stack page at RSP of a
//   stopped thread is committed, the thread was using it), but a failed read must not crash the profiler. The
//   value is only meaningful where the sampled code has not moved RSP (leaf helpers): elsewhere it is whatever lies
//   on top of the stack, and the summary prints it as a candidate, not a proof.
//
// TIMING
//   The wait between samples is a high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
//   Windows 10 1803 and later); without it, Sleep(1), whose accuracy is the system timer resolution
//   (the host asks for 1 ms with timeBeginPeriod, src/host/sysinfo_win32.cpp). The real rate is therefore
//   at most kSampleRateHz and can be lower; the file's rate_hz is the nominal one, and cpu_us/duration_us
//   are what to compare the sample count with.
//
// THREAD CPU TIME
//   thread_cpu_ns() uses GetThreadTimes (kernel + user time), not QueryThreadCycleTime: cycles are not
//   time (the clock of a core changes with frequency scaling), and the file asks for microseconds. The
//   price is accuracy: the kernel charges it in scheduler quanta (about 15.6 ms), fine for runs of
//   seconds, wrong for tiny ones.
//
// MODULES
//   collect_modules() writes one line per loaded module with the WHOLE IMAGE as the range: start = image
//   base, end = base + SizeOfImage, file offset 0. So address - start is the RVA, which is what
//   tools/verify/profile_summary.py groups by (module + offset) for a DLL without symbols. Paths are
//   UTF-8 (Windows paths, with backslashes and possibly spaces).
//
// Microsoft Learn pages (learn.microsoft.com/en-us/windows/win32/api/...):
//   processthreadsapi/nf-processthreadsapi-suspendthread, -resumethread, -getthreadcontext,
//   -getthreadtimes, -getcurrentthread, -createthread (via std::thread), handleapi/nf-handleapi-duplicatehandle,
//   realtimeapiset/nf-realtimeapiset-querythreadcycletime, winnt/ns-winnt-context (Rip, CONTEXT_CONTROL),
//   memoryapi/nf-memoryapi-readprocessmemory,
//   synchapi/nf-synchapi-createwaitabletimerexw, -setwaitabletimer, -waitforsingleobject, -sleep,
//   psapi/nf-psapi-enumprocessmodules, -getmoduleinformation, libloaderapi/nf-libloaderapi-getmodulefilenamew,
//   stringapiset/nf-stringapiset-widechartomultibyte.
#if defined(_WIN32)

#include "profile.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi.h after windows.h. With PSAPI_VERSION=2 (Windows 8 and later) the functions live in kernel32
// (K32EnumProcessModules...) and psapi.h maps the names to them; the import library psapi covers both.
#include <psapi.h>

namespace don_profile::platform
{
namespace
{
HANDLE g_target = nullptr;           // a real handle to the sampled thread (owned here)
std::thread g_sampler;               // the sampler thread
std::atomic<bool> g_stop{false};
HANDLE g_wake_timer = nullptr;       // the waitable timer the sampler sleeps on (null: Sleep(1))

#if !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // older SDK headers lack it
#endif

// Waits one sampling period.
void wait_one_period()
{
    if (g_wake_timer != nullptr)
    {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(10000000ll / kSampleRateHz); // relative, 100 ns units
        if (SetWaitableTimer(g_wake_timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(g_wake_timer, INFINITE);
            return;
        }
    }
    Sleep(1);
}

void sampler_main()
{
    ULONG64 last_cycles = 0;
    const bool want_callers = don_profile::callers_enabled();
    // A cycle counter that reads 0 or fails (Wine 9 returns 0: QueryThreadCycleTime is a stub there) cannot
    // tell a blocked thread from a running one: the filter is off and every tick is a sample.
    const bool idle_filter = QueryThreadCycleTime(g_target, &last_cycles) && last_cycles != 0;
    while (!g_stop.load(std::memory_order_acquire))
    {
        wait_one_period();
        if (g_stop.load(std::memory_order_acquire))
            break;

        ULONG64 cycles = 0;
        if (idle_filter)
        {
            if (!QueryThreadCycleTime(g_target, &cycles) || cycles == last_cycles)
                continue; // the target did not run since the last sample: blocked, nothing to attribute
            last_cycles = cycles;
        }

        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        uintptr_t pc = 0;
        uintptr_t caller = 0;
        if (SuspendThread(g_target) != static_cast<DWORD>(-1))
        {
            // Nothing but this call while the target is stopped (it may hold the heap or loader lock).
            if (GetThreadContext(g_target, &context))
            {
#if defined(_M_X64) || defined(__x86_64__)
                pc = static_cast<uintptr_t>(context.Rip);
                if (want_callers)
                {
                    // Own process, no lock, no fault: see CALLERS above.
                    SIZE_T got = 0;
                    uintptr_t top = 0;
                    if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(context.Rsp), &top,
                                          sizeof top, &got) && got == sizeof top)
                        caller = top;
                }
#elif defined(_M_ARM64) || defined(__aarch64__)
                pc = static_cast<uintptr_t>(context.Pc);
#endif
            }
            ResumeThread(g_target);
        }
        if (pc != 0)
            don_profile::handler_record(pc, caller);
    }
}

std::string utf8(const wchar_t *wide)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size) - 1); // drop the terminator the call wrote
    return out;
}
} // namespace

bool start_sampler(std::string &error)
{
#if !defined(_M_X64) && !defined(__x86_64__) && !defined(_M_ARM64) && !defined(__aarch64__)
    error = "profiling is not available on this platform (no instruction pointer for this architecture)";
    return false;
#else
    if (!std::atomic<uintptr_t>::is_always_lock_free || !std::atomic<uint64_t>::is_always_lock_free)
    {
        error = "profiling is not available on this platform (no lock-free atomics)";
        return false;
    }
    if (g_target != nullptr)
        return true;

    HANDLE target = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        error = "cannot duplicate the handle of the sampled thread (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    g_target = target;
    g_stop.store(false, std::memory_order_release);
    g_wake_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    try
    {
        g_sampler = std::thread(sampler_main);
    }
    catch (...)
    {
        error = "cannot start the sampler thread";
        if (g_wake_timer != nullptr)
            CloseHandle(g_wake_timer);
        g_wake_timer = nullptr;
        CloseHandle(g_target);
        g_target = nullptr;
        return false;
    }
    return true;
#endif
}

void stop_sampler()
{
    if (g_target == nullptr)
        return;
    g_stop.store(true, std::memory_order_release);
    if (g_sampler.joinable())
        g_sampler.join(); // after this no sample can reach the buffer
    if (g_wake_timer != nullptr)
        CloseHandle(g_wake_timer);
    g_wake_timer = nullptr;
    CloseHandle(g_target);
    g_target = nullptr;
}

uint64_t thread_cpu_ns()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    const auto ticks = [](const FILETIME &time) {
        return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; // 100 ns units
    };
    return (ticks(kernel) + ticks(user)) * 100ull;
}

void collect_modules(std::vector<Module> &out)
{
    const HANDLE process = GetCurrentProcess();
    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed))
        return;
    if (needed > handles.size() * sizeof(HMODULE))
    {
        handles.resize(needed / sizeof(HMODULE));
        if (!EnumProcessModules(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                                &needed))
            return;
    }
    const size_t count = needed / sizeof(HMODULE);
    for (size_t i = 0; i < count && i < handles.size(); ++i)
    {
        MODULEINFO info{};
        if (!GetModuleInformation(process, handles[i], &info, sizeof info))
            continue;
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(handles[i], path, static_cast<DWORD>(sizeof path / sizeof path[0]));
        if (length == 0 || length >= sizeof path / sizeof path[0])
            continue;
        Module module;
        module.start = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        module.end = module.start + info.SizeOfImage;
        module.offset = 0;
        module.path = utf8(path);
        out.push_back(std::move(module));
    }
}
} // namespace don_profile::platform

#endif // _WIN32
