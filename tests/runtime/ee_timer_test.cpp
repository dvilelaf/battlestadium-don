// SPDX-License-Identifier: GPL-3.0-or-later
// Patch 0035: the EE timers are credited per dispatch and advanced in a batch, instead of being advanced
// on every dispatch. This test keeps the guest-visible values exact with an arithmetic the test can do
// itself: the guest programs TIM0 with the 9.216 MHz clock (kEeClockHz / 32, so 32 EE cycles are one timer
// tick, exactly), starts it, spins for a fixed number of dispatch checkpoints and reads the count and the
// mode at fixed checkpoints. At every read the count must be the number the old per-dispatch advance
// would have shown: floor(cycles / 32), with the compare and overflow flags of the mode set from the
// cycle the compare is passed on.
//
// The printed trace (one line per checkpoint plus the host nanoseconds per dispatch) is what the two
// builds are compared on; the tree without 0035 builds the same file and must print the same lines.

#include "host_test_support.h"

#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    using Clock = std::chrono::steady_clock;

    int g_failures = 0;

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        }
        return condition;
    }

    std::string num(uint64_t value) { return std::to_string(value); }

    constexpr uint32_t kEntry = 0x00100000u;
    // TIM0: count at +0x00, mode at +0x10, compare at +0x20 (PS2Memory's kEeTimerBases / offsets).
    constexpr uint32_t kTim0Count = 0x10000000u;
    constexpr uint32_t kTim0Mode = 0x10000010u;
    constexpr uint32_t kTim0Compare = 0x10000020u;
    // CUE | CMPE | OVFE | clock select 1 (9,216,000 Hz = kEeClockHz / 32).
    constexpr uint32_t kMode = (1u << 7) | (1u << 8) | (1u << 9) | 1u;
    constexpr uint32_t kCompare = 64u;      // 64 timer ticks = 2048 EE cycles = 256 dispatches
    constexpr uint32_t kEquf = 1u << 10;
    constexpr uint32_t kOvff = 1u << 11;
    // Every dispatch is EeScheduler::kGuestDispatchCycles EE cycles; 32 EE cycles are one timer tick.
    constexpr uint64_t kCyclesPerDispatch = EeScheduler::kGuestDispatchCycles;
    constexpr uint64_t kCyclesPerTick = 32u;
    // 100,000 dispatches = 800,000 EE cycles = 25,000 timer ticks: past the compare (64 ticks) by far, so
    // the compare flag must appear early and the count must keep matching after it.
    constexpr uint64_t kLoops = 100000u;
    constexpr uint64_t kSampleEvery = 12500u;
    // Phase 2 (below): the timers alone, one advance of kCyclesPerDispatch per iteration. 100,000
    // iterations are 800,000 EE cycles = 25,000 timer ticks, and each read's expectation is exact:
    // count = floor((i + 1) * 8 / 32) = (i + 1) / 4.
    constexpr uint32_t kAdvances = 100000u;
    constexpr uint32_t kAdvanceSampleEvery = 12500u;

    struct State
    {
        uint64_t calls = 0;
        uint64_t samples = 0;
        uint64_t mismatches = 0;
        uint64_t firstEquationCycle = 0;
        uint64_t firstOverflowCycle = 0;
        uint64_t start = 0;
        uint64_t end = 0;
    } g_state;

    std::string hex32(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08x", value);
        return text;
    }

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t call = ++g_state.calls;
        PS2Memory &memory = runtime->memory();
        if (call == 1u)
        {
            memory.write32(kTim0Count, 0u);
            memory.write32(kTim0Compare, kCompare);
            memory.write32(kTim0Mode, kMode);   // starts the timer (CUE)
            g_state.start = Clock::now().time_since_epoch().count();
        }
        else if (call >= kSampleEvery && (call % kSampleEvery) == 0u)
        {
            const uint32_t count = memory.read32(kTim0Count) & 0xFFFFu;
            const uint32_t mode = memory.read32(kTim0Mode);
            const uint64_t cycles = call * kCyclesPerDispatch;
            const uint64_t expected = (cycles / kCyclesPerTick) & 0xFFFFu;
            ++g_state.samples;
            if (count != expected)
            {
                ++g_state.mismatches;
                std::printf("[ee_timer_test] MISMATCH at call %s: count %s expected %s\n", num(call).c_str(),
                            num(count).c_str(), num(expected).c_str());
            }
            if ((mode & kEquf) != 0u && g_state.firstEquationCycle == 0u)
            {
                g_state.firstEquationCycle = cycles;
            }
            if ((mode & kOvff) != 0u && g_state.firstOverflowCycle == 0u)
            {
                g_state.firstOverflowCycle = cycles;
            }
            std::printf("[ee_timer_test] call %s cycles %s count %s mode %s\n", num(call).c_str(),
                        num(cycles).c_str(), num(count).c_str(), hex32(mode).c_str());
        }
        else if (call == kLoops + 1u)
        {
            g_state.end = Clock::now().time_since_epoch().count();
            ctx->pc = 0u; // the guest's main thread ends
        }
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("ee-timer-test"), "initialize()");
    expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
    rt.cpu().pc = kEntry;
    expect(rt.startGameThread(), "startGameThread()");

    for (int i = 0; i < 2000 && g_state.calls < kLoops + 1u; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    rt.stopGameThread();

    const double nsPerDispatch = g_state.end > g_state.start && g_state.samples != 0u
                                     ? static_cast<double>(g_state.end - g_state.start) /
                                           static_cast<double>(kLoops)
                                     : 0.0;
    std::printf("[ee_timer_test] calls=%s samples=%s mismatches=%s nsPerDispatch=%.2f "
                "firstEqufCycle=%s firstOvffCycle=%s\n",
                num(g_state.calls).c_str(), num(g_state.samples).c_str(), num(g_state.mismatches).c_str(),
                nsPerDispatch, num(g_state.firstEquationCycle).c_str(), num(g_state.firstOverflowCycle).c_str());

    expect(g_state.calls == kLoops + 1u, "the guest ran to the end: calls=" + num(g_state.calls));
    expect(g_state.mismatches == 0u,
           "every read of the timer count matched floor(cycles / 32): " + num(g_state.mismatches) +
               " mismatch(es) of " + num(g_state.samples));
    // The compare is passed at 64 ticks = 2048 EE cycles = 256 dispatches, and the flags stay set: the
    // first sample that sees them is the next checkpoint after that, i.e. 25,000 dispatches here.
    expect(g_state.firstEquationCycle == kSampleEvery * kCyclesPerDispatch,
           "the compare flag appeared at the first checkpoint past 2048 cycles: " +
               num(g_state.firstEquationCycle));
    // 65,536 ticks = 2,097,152 cycles = 262,144 dispatches: more than this run, so no overflow flag.
    expect(g_state.firstOverflowCycle == 0u,
           "no overflow flag before 262,144 dispatches, got " + num(g_state.firstOverflowCycle));

    // Phase 2: the timers alone, no guest. Every read must see the count the old per-dispatch advance
    // would have produced, exactly (32 EE cycles are one tick with this clock), and the printed trace is
    // what the tree without this patch is compared on. The host nanoseconds per advanceEeTimers() call is
    // the cost figure (not an assertion).
    PS2Memory &memory = rt.memory();
    memory.resetEeTimers();
    memory.write32(kTim0Count, 0u);
    memory.write32(kTim0Compare, kCompare);
    memory.write32(kTim0Mode, kMode);
    uint64_t directMismatches = 0;
    uint32_t directFirstEquf = 0;
    const Clock::time_point directFrom = Clock::now();
    for (uint32_t i = 0; i < kAdvances; ++i)
    {
        memory.advanceEeTimers(kCyclesPerDispatch);
        if ((i % kAdvanceSampleEvery) == 0u)
        {
            const uint32_t count = memory.read32(kTim0Count) & 0xFFFFu;
            const uint32_t mode = memory.read32(kTim0Mode);
            const uint64_t expected = ((static_cast<uint64_t>(i) + 1u) * kCyclesPerDispatch) / kCyclesPerTick;
            if (count != (expected & 0xFFFFu))
            {
                ++directMismatches;
            }
            if ((mode & kEquf) != 0u && directFirstEquf == 0u)
            {
                directFirstEquf = i;
            }
            std::printf("[ee_timer_test] direct %u count %u expected %llu mode %s\n", i, count,
                        static_cast<unsigned long long>(expected), hex32(mode).c_str());
        }
    }
    const Clock::time_point directTo = Clock::now();
    const double nsPerAdvance =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(directTo - directFrom).count()) /
        static_cast<double>(kAdvances);
    std::printf("[ee_timer_test] direct mismatches=%llu firstEqufAfter=%u advancesNS=%.2f\n",
                static_cast<unsigned long long>(directMismatches), directFirstEquf, nsPerAdvance);
    expect(directMismatches == 0u,
           "phase 2: every read matched floor((i+1)*8/32), got " + num(directMismatches) + " mismatch(es)");
    expect(directFirstEquf != 0u && directFirstEquf <= kAdvanceSampleEvery,
           "phase 2: the compare flag was visible at the first sample past 64 ticks, got after " +
               num(directFirstEquf) + " advances");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "ee_timer_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ee_timer_test: all checks passed\n");
    return 0;
}
