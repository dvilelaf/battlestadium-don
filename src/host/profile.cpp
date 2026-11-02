// SPDX-License-Identifier: GPL-3.0-or-later
// The portable part of the profiler of src/host/profile.h: reading DON_PROFILE, the buffer of
// sampled addresses, the aggregation and the plain-text file. No operating-system calls here; the
// only file is profile_linux.cpp. On platforms that are not Linux this file also holds the empty
// platform layer (profiling is not available there), so that adding only profile.cpp and
// profile_linux.cpp to a build works everywhere.
#include "profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <tuple>

#if !defined(__linux__) && !defined(_WIN32)
// The empty platform layer of the platforms without a sampler. profile_linux.cpp is expected in the
// build everywhere; it compiles to nothing here, so these definitions are the fallback.
namespace don_profile::platform
{
bool start_sampler(std::string &error)
{
    error = "profiling is not available on this platform";
    return false;
}
void stop_sampler() {}
uint64_t thread_cpu_ns() { return 0; }
void collect_modules(std::vector<Module> &out) { (void)out; }
} // namespace don_profile::platform
#endif

namespace
{
// DON_PROFILE, read once. Empty (or unset) means the whole feature is off.
const std::string &profile_path()
{
    static const std::string path = [] {
        const char *value = std::getenv("DON_PROFILE");
        return std::string(value != nullptr ? value : "");
    }();
    return path;
}

// The buffer of sampled addresses. The sampled thread's handler is the only writer. It is
// allocated by profile_thread_begin() before the sampler is armed and freed by profile_finish()
// after the sampler was stopped and any sample in flight was drained, so the handler either sees
// the buffer or nullptr, never a freed one.
std::atomic<uintptr_t> *g_slots = nullptr;
uint32_t g_capacity = 0;
std::atomic<uint32_t> g_count{0};
// T-765: with DON_PROFILE_CALLERS=1, a second preallocated buffer parallel to g_slots (same index): the return
// address read from the top of the sampled thread's stack. nullptr when off.
std::atomic<uintptr_t> *g_callers = nullptr;
std::atomic<uint64_t> g_lost{0};
// T-203: the number of the current vertical blank (low 16 bits), stored in the top 16 bits of every sample so that
// DON_PROFILE_BLANKS=1 can write which blank each sample was taken in. User-space addresses are below 2^47.
std::atomic<uint32_t> g_blank{0};
constexpr uintptr_t kAddressMask = (uintptr_t(1) << 48) - 1;

std::mutex g_state_mutex;
std::string g_thread_name;
bool g_started = false;   // the sampler was armed (start_sampler returned true)
bool g_writing = false;   // profile_finish() is writing the file / has written it
std::chrono::steady_clock::time_point g_wall_begin;
uint64_t g_wall_us = 0;
uint64_t g_cpu_begin_ns = 0;
uint64_t g_cpu_end_ns = 0;

std::string hex16(uintptr_t value)
{
    char text[32];
    std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

// One distinct address and how many samples it got, most sampled first.
using AddressCount = std::pair<uintptr_t, uint64_t>;

std::vector<AddressCount> histogram()
{
    const uint32_t count = g_count.load(std::memory_order_acquire);
    std::map<uintptr_t, uint64_t> totals;
    for (uint32_t i = 0; i < count; ++i)
        ++totals[g_slots[i].load(std::memory_order_relaxed) & kAddressMask];
    std::vector<AddressCount> out(totals.begin(), totals.end());
    std::sort(out.begin(), out.end(), [](const AddressCount &a, const AddressCount &b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

// T-765: (address, caller) pairs and how many samples each got, most sampled first.
struct CallerCount
{
    uintptr_t address;
    uintptr_t caller;
    uint64_t count;
};

std::vector<CallerCount> caller_histogram()
{
    std::vector<CallerCount> out;
    if (g_callers == nullptr)
        return out;
    const uint32_t count = g_count.load(std::memory_order_acquire);
    std::map<std::pair<uintptr_t, uintptr_t>, uint64_t> totals;
    for (uint32_t i = 0; i < count; ++i)
        ++totals[{g_slots[i].load(std::memory_order_relaxed) & kAddressMask,
                  g_callers[i].load(std::memory_order_relaxed) & kAddressMask}];
    for (const auto &entry : totals)
        out.push_back({entry.first.first, entry.first.second, entry.second});
    std::sort(out.begin(), out.end(), [](const CallerCount &a, const CallerCount &b) {
        if (a.count != b.count)
            return a.count > b.count;
        return std::tie(a.address, a.caller) < std::tie(b.address, b.caller);
    });
    return out;
}

void write_file(const std::vector<AddressCount> &addresses, const std::vector<don_profile::Module> &modules,
                uint64_t lost, const std::vector<CallerCount> &callers)
{
    const std::filesystem::path path(profile_path());
    std::error_code ec;
    if (path.has_parent_path() && !path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "[don] profile: cannot write %s\n", profile_path().c_str());
        return;
    }

    out << "# don profile 1\n";
    out << "rate_hz " << don_profile::kSampleRateHz << "\n";
    out << "thread " << g_thread_name << "\n";
    out << "duration_us " << g_wall_us << "\n";
    out << "cpu_us " << ((g_cpu_end_ns >= g_cpu_begin_ns ? g_cpu_end_ns - g_cpu_begin_ns : 0) / 1000u) << "\n";
    out << "samples_taken " << g_count.load(std::memory_order_relaxed) << "\n";
    out << "samples_lost " << lost << "\n";
    out << "addresses " << addresses.size() << "\n";
    out << "modules " << modules.size() << "\n";
    for (const don_profile::Module &module : modules)
        out << "module " << hex16(module.start) << " " << hex16(module.end) << " " << hex16(module.offset)
            << " " << module.path << "\n";
    for (const AddressCount &entry : addresses)
        out << hex16(entry.first) << " " << entry.second << "\n";
    if (g_callers != nullptr)
    {
        out << "callers " << callers.size() << "\n";
        for (const CallerCount &entry : callers)
            out << hex16(entry.address) << " " << hex16(entry.caller) << " " << entry.count << "\n";
    }
    out.flush();
    if (!out)
    {
        std::fprintf(stderr, "[don] profile: error while writing %s\n", profile_path().c_str());
        return;
    }

    std::fprintf(stderr, "[don] profile: %llu samples (%llu lost, %zu addresses) of thread '%s' written to %s\n",
                 static_cast<unsigned long long>(g_count.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(lost), addresses.size(), g_thread_name.c_str(),
                 profile_path().c_str());
}
} // namespace

bool profile_enabled() { return !profile_path().empty(); }

void profile_set_blank(uint64_t blank) { g_blank.store(static_cast<uint32_t>(blank), std::memory_order_relaxed); }

bool don_profile::callers_enabled() { return g_callers != nullptr; }

void don_profile::handler_record(uintptr_t instruction_address, uintptr_t caller)
{
    // Inside a signal handler: only plain stores to preallocated memory. The atomics must be
    // lock-free or a store could take a lock that the interrupted code already holds.
    static_assert(std::atomic<uintptr_t>::is_always_lock_free,
                  "the profile handler needs a lock-free atomic<uintptr_t>");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "the profile handler needs a lock-free atomic<uint64_t>");

    std::atomic<uintptr_t> *slots = g_slots;
    if (slots == nullptr)
        return;
    const uint32_t index = g_count.load(std::memory_order_relaxed);
    if (index >= g_capacity)
    {
        g_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_callers != nullptr)
        g_callers[index].store(caller, std::memory_order_relaxed); // before the count's release store below
    slots[index].store((instruction_address & kAddressMask) | (uintptr_t(g_blank.load(std::memory_order_relaxed) & 0xFFFFu) << 48),
                       std::memory_order_relaxed);
    g_count.store(index + 1, std::memory_order_release);
}

void profile_thread_begin(const char *name)
{
    if (!profile_enabled())
        return;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_slots != nullptr || g_started)
    {
        std::fprintf(stderr, "[don] profile: already sampling, ignoring thread_begin\n");
        return;
    }

    g_thread_name = (name != nullptr) ? name : "?";
    g_count.store(0, std::memory_order_relaxed);
    g_lost.store(0, std::memory_order_relaxed);
    g_writing = false;
    g_cpu_begin_ns = 0;
    g_cpu_end_ns = 0;
    g_wall_us = 0;

    const uint32_t capacity = don_profile::kSampleCapacity;
    g_slots = new (std::nothrow) std::atomic<uintptr_t>[capacity];
    if (g_slots == nullptr)
    {
        std::fprintf(stderr, "[don] profile: cannot allocate the sample buffer (%u slots)\n", capacity);
        return;
    }
    g_capacity = capacity;
    if (const char *callers = std::getenv("DON_PROFILE_CALLERS"); callers != nullptr && callers[0] == '1')
        g_callers = new (std::nothrow) std::atomic<uintptr_t>[capacity]; // nullptr (no callers) if it fails

    std::string error;
    if (!don_profile::platform::start_sampler(error))
    {
        // Reported once per process: profile_thread_begin() is called once per run.
        std::fprintf(stderr, "[don] profile: %s\n", error.c_str());
        delete[] g_slots;
        g_slots = nullptr;
        delete[] g_callers;
        g_callers = nullptr;
        g_capacity = 0;
        return;
    }

    g_started = true;
    g_wall_begin = std::chrono::steady_clock::now();
    g_cpu_begin_ns = don_profile::platform::thread_cpu_ns();
}

void profile_thread_end()
{
    if (!profile_enabled())
        return;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_started)
        return;
    don_profile::platform::stop_sampler();
    g_started = false;
    g_cpu_end_ns = don_profile::platform::thread_cpu_ns();
    g_wall_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_wall_begin)
            .count());
}

void profile_finish()
{
    if (!profile_enabled())
        return;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_writing)
        return;
    g_writing = true;

    // A caller that never called profile_thread_end() still gets its file: stop first, then read.
    if (g_started)
    {
        don_profile::platform::stop_sampler();
        g_started = false;
        g_cpu_end_ns = don_profile::platform::thread_cpu_ns();
        g_wall_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - g_wall_begin)
                                              .count());
    }
    if (g_slots == nullptr)
    {
        std::fprintf(stderr, "[don] profile: nothing was sampled (see the reason above)\n");
        return;
    }

    const uint64_t lost = g_lost.load(std::memory_order_relaxed);
    std::vector<AddressCount> addresses = histogram();
    std::vector<don_profile::Module> modules;
    don_profile::platform::collect_modules(modules);
    write_file(addresses, modules, lost, caller_histogram());
    if (const char *per_blank = std::getenv("DON_PROFILE_BLANKS"); per_blank != nullptr && per_blank[0] == '1')
    {
        // One line per sample: "<blank mod 65536> <address>", for tools that attribute samples to a blank.
        std::ofstream blanks(profile_path() + ".blanks", std::ios::binary | std::ios::trunc);
        const uint32_t count = g_count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uintptr_t slot = g_slots[i].load(std::memory_order_relaxed);
            blanks << (slot >> 48) << " " << hex16(slot & kAddressMask) << "\n";
        }
    }

    delete[] g_slots;
    g_slots = nullptr;
    delete[] g_callers;
    g_callers = nullptr;
    g_capacity = 0;
}
