// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux sampler of src/host/profile.h: the only file of the profiler with operating-system
// calls (POSIX timers, signals, the map of the process in /proc). Added to the build on every
// platform; on one that is not Linux it compiles to an empty translation unit and profile.cpp's
// empty platform layer reports that profiling is not available.
//
// HOW THE SAMPLING WORKS
//   start_sampler() installs a SIGPROF handler and creates a POSIX timer on the *calling thread's*
//   processor-time clock (CLOCK_THREAD_CPUTIME_ID), one shot per millisecond of that clock, and
//   arms it. The timer is directed at the calling thread by thread id (SIGEV_THREAD_ID, the Linux
//   extension), so no other thread can receive it; where that is not available it falls back to
//   SIGEV_SIGNAL, and then the kernel still delivers a processor-clock timer to the thread the
//   clock belongs to but the interface does not promise it. The handler reads the interrupted
//   instruction pointer from the signal context and calls don_profile::handler_record(), which does
//   nothing but a plain store into the preallocated buffer.
//   stop_sampler() blocks SIGPROF on the thread, deletes the timer and drains a sample that was
//   already queued but not yet delivered, so that after it returns no handler can run again (this
//   is what makes freeing the buffer in profile_finish() safe). SIGPROF stays blocked on that
//   thread afterwards, which is harmless: the thread is the one being profiled and it is ending.
//   The handler only touches memory allocated before the timer was armed: no allocation, no lock,
//   no formatted output, nothing that can call into the C library.
//
// WHY PROCESSOR TIME AND NOT WALL TIME
//   The question is where the thread spends its work, so a sample per millisecond of *thread* time
//   makes the counts directly proportional to time spent, and a thread that sleeps contributes
//   nothing. A thread blocked in a system call, on the other hand, is not interrupted by this clock
//   at all, so its time inside the kernel is invisible (it shows up as a smaller sample count than
//   the wall-clock duration would suggest).
#if defined(__linux__)

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE // sigev_notify_thread_id (SIGEV_THREAD_ID), the REG_* names
#endif

#include "profile.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

namespace don_profile::platform
{
namespace
{
timer_t g_timer{};
bool g_armed = false;

// The interrupted instruction pointer. Both supported architectures, nothing else.
uintptr_t interrupted_pc(void *context)
{
    const auto *const uc = static_cast<const ucontext_t *>(context);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<uintptr_t>(uc->uc_mcontext.pc);
#else
    (void)uc;
    return 0;
#endif
}
} // namespace

// The handler; extern "C" because that is the type sigaction() expects.
extern "C" void don_profile_signal(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)info;
    don_profile::handler_record(interrupted_pc(context));
}

bool start_sampler(std::string &error)
{
#if !defined(__x86_64__) && !defined(__aarch64__)
    error = "profiling is not available on this platform (no instruction pointer for this architecture)";
    return false;
#else
    // The handler stores into lock-free atomics; without them a store could take a lock.
    if (!std::atomic<uintptr_t>::is_always_lock_free || !std::atomic<uint64_t>::is_always_lock_free)
    {
        error = "profiling is not available on this platform (no lock-free atomics)";
        return false;
    }
    if (g_armed)
        return true;

    struct sigaction action{};
    action.sa_sigaction = &don_profile_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPROF, &action, nullptr) != 0)
    {
        error = std::string("cannot install the SIGPROF handler: ") + std::strerror(errno);
        return false;
    }

    struct sigevent event{};
    event.sigev_signo = SIGPROF;
    event.sigev_value.sival_ptr = nullptr;
#if defined(SIGEV_THREAD_ID) && defined(sigev_notify_thread_id)
    // This thread only, by thread id: no other thread of the process can receive the sample.
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_notify_thread_id = static_cast<pid_t>(::syscall(SYS_gettid));
#else
    // A process-directed signal: the kernel still delivers a processor-clock timer to the thread
    // whose clock expired, but the interface does not promise it.
    event.sigev_notify = SIGEV_SIGNAL;
#endif

    timer_t timer{};
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer) != 0)
    {
        error = std::string("cannot create the thread CPU-time timer: ") + std::strerror(errno);
        return false;
    }

    struct itimerspec spec{};
    const long period_ns = 1000000000L / static_cast<long>(kSampleRateHz);
    spec.it_value.tv_nsec = period_ns;
    spec.it_interval.tv_nsec = period_ns;
    if (timer_settime(timer, 0, &spec, nullptr) != 0)
    {
        const int saved = errno;
        timer_delete(timer);
        error = std::string("cannot arm the thread CPU-time timer: ") + std::strerror(saved);
        return false;
    }

    g_timer = timer;
    g_armed = true;
    return true;
#endif
}

void stop_sampler()
{
    if (!g_armed)
        return;
    g_armed = false;

    // Block first, so that a sample generated while the timer is being deleted cannot be delivered
    // after the drain below. On Linux sigprocmask() changes the mask of the calling thread only,
    // which is the thread being profiled. The mask is left as it is: the thread is ending.
    sigset_t set{};
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    sigprocmask(SIG_BLOCK, &set, nullptr);

    timer_delete(g_timer);
    g_timer = timer_t{};

    // Drain a sample that was already queued but not delivered.
    struct timespec zero{};
    while (sigtimedwait(&set, nullptr, &zero) == SIGPROF)
    {
    }
}

uint64_t thread_cpu_ns()
{
    struct timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void collect_modules(std::vector<Module> &out)
{
    std::FILE *const file = std::fopen("/proc/self/maps", "r");
    if (file == nullptr)
        return;

    char line[4096];
    while (std::fgets(line, sizeof line, file) != nullptr)
    {
        // 55c0d0e00000-55c0d0f42000 r-xp 00001000 fd:01 1234567    /path/to/module
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long offset = 0;
        char perms[8] = {};
        int consumed = 0;
        if (std::sscanf(line, "%llx-%llx %7s %llx %*s %*s%n", &start, &end, perms, &offset, &consumed) != 4)
            continue;
        if (consumed <= 0 || perms[0] == '\0' || perms[2] != 'x') // executable mappings only
            continue;
        const char *name = line + consumed;
        while (*name == ' ' || *name == '\t')
            ++name;
        if (*name != '/') // anonymous mappings and [vdso] have no file
            continue;
        std::string path(name);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        Module module;
        module.start = static_cast<uintptr_t>(start);
        module.end = static_cast<uintptr_t>(end);
        module.offset = static_cast<uintptr_t>(offset);
        module.path = std::move(path);
        out.push_back(std::move(module));
    }
    std::fclose(file);
}
} // namespace don_profile::platform

#endif // __linux__
