// SPDX-License-Identifier: GPL-3.0-or-later
// Per-subsystem performance measurement of the game thread (T-203): where the thread that runs the
// recompiled game spends its processor time, measured by sampling its instruction pointer, not by
// instrumenting anything.
//
// HOW IT WORKS
//   profile_thread_begin("GameThread") is called on the thread to sample (the game thread's
//   thread_begin callback). It allocates one fixed array of addresses and arms a POSIX timer on
//   that thread's processor-time clock (CLOCK_THREAD_CPUTIME_ID) that delivers SIGPROF to that
//   same thread about every millisecond of *thread* time. The signal handler does only what is
//   safe inside a handler: it reads the interrupted instruction's address from the signal context
//   and stores it in the next free slot. It never allocates, never locks and never formats. A full
//   buffer is not overwritten: the samples that do not fit are counted (samples_lost) and dropped,
//   so the first part of a long run is what survives (raise kSampleCapacity or lower kSampleRateHz
//   if that ever happens).
//   profile_thread_end() stops the timer. profile_finish() (any thread, after the sampled thread
//   has stopped or ended) reads the module map of the process once, aggregates the addresses into
//   a plain-text file and frees the buffer.
//   Addresses are raw host instruction pointers. The program does no symbol lookup: the addresses
//   only mean something together with the module map written to the same file (the program is
//   loaded at a different address in every run), and tools/verify/profile_summary.py turns them
//   into symbols and subsystems.
//
// COST WHEN OFF
//   With DON_PROFILE unset (or empty) every function of this interface returns at once: nothing is
//   allocated, no timer is created, no signal handler is installed and no clock is read.
//   DON_PROFILE is the path of the file to write, so a run with it set writes exactly one file.
//
// PLATFORMS
//   profile.cpp holds the portable part and, on platforms that are not Linux, an empty platform
//   layer that reports "profiling is not available on this platform" once. profile_linux.cpp holds
//   the only file with operating-system calls (timer, signal, /proc/self/maps); elsewhere it
//   compiles to an empty translation unit. Adding a platform means writing one small file like it.
//
// FILE FORMAT (plain text, one line per item; see tools/verify/profile_summary.py for the reader)
//   # don profile 1
//   rate_hz <samples per second of thread time>
//   thread <name given to profile_thread_begin>
//   duration_us <wall time between begin and end>
//   cpu_us <processor time of the thread between begin and end>
//   samples_taken <addresses stored>
//   samples_lost <samples dropped because the buffer was full>
//   addresses <distinct addresses below>
//   modules <module lines below>
//   module <start> <end> <file offset> <path>              (executable mappings, hexadecimal)
//   <address> <count>                                      (one line per distinct address)
//   callers <n>                                            (optional, T-765, only with DON_PROFILE_CALLERS=1)
//   <address> <caller> <count>                             (n lines: how often <address> was sampled with the
//                                                           return address <caller> on top of the stack)
// The `callers` section follows the address lines. Readers that do not know it (an old profile_summary.py) fail on
// its lines, so they work only with files where the section is absent: it is written only with
// DON_PROFILE_CALLERS=1, and the default file is unchanged. Only the Windows sampler fills it (it reads the 8 bytes at RSP of the stopped
// thread; profile_linux.cpp has no use for it: a SIGPROF handler could read the ucontext stack the same way, but
// nobody needs it there).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// true when DON_PROFILE is set to a non-empty path. Read once, on the first call.
bool profile_enabled();

// The thread to sample: call first thing on it. `name` (copied) is written to the file as the
// thread's name. Does nothing when profiling is off.
void profile_thread_begin(const char *name);

// T-203: tells the profiler which vertical blank the game thread is in (called at each blank, on that thread).
void profile_set_blank(uint64_t blank);

// Stops the sampler. Does not write the file (profile_finish() does).
void profile_thread_end();

// Writes the file and frees the buffer. Idempotent; safe to call when profiling never started.
void profile_finish();

// ---------------------------------------------------------------------------------------------
// Internal: shared between profile.cpp and the per-platform sampler. Not used by the host.
// ---------------------------------------------------------------------------------------------
namespace don_profile
{
// Sampling rate, in samples per second of processor time of the sampled thread.
constexpr uint32_t kSampleRateHz = 1000;
// Capacity of the buffer of sampled addresses (2^20 samples is about 17 minutes at 1000 Hz).
constexpr uint32_t kSampleCapacity = 1u << 20;

// One executable mapping of a loaded file, from the system's map of the process.
struct Module
{
    uintptr_t start = 0;  // where the mapping starts in this process
    uintptr_t end = 0;    // one past it
    uintptr_t offset = 0; // offset of the mapping inside the file
    std::string path;
};

// Called from the signal handler only: stores `instruction_address` in the next free slot, or
// counts it as lost when the buffer is full. Runs on the sampled thread.
// T-765: `caller` (the return address at the top of the stopped thread's stack, 0 when unknown) is kept only
// when callers_enabled().
void handler_record(uintptr_t instruction_address, uintptr_t caller = 0);

// true when DON_PROFILE_CALLERS=1 and the caller buffer was allocated.
bool callers_enabled();

// The operating-system part. Defined by profile_linux.cpp on Linux and by profile.cpp elsewhere.
namespace platform
{
// Arms the per-thread sampler. Returns false and sets `error` when it cannot (the caller prints
// the reason once). Called on the sampled thread, with the buffer already allocated.
bool start_sampler(std::string &error);

// Disarms it (idempotent) and makes sure no sample still in flight can reach the buffer.
void stop_sampler();

// Processor time of the calling thread in nanoseconds, or 0 when it is not available.
uint64_t thread_cpu_ns();

// The executable mappings of the process, appended to `out`.
void collect_modules(std::vector<Module> &out);
} // namespace platform
} // namespace don_profile
