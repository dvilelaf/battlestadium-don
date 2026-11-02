// SPDX-License-Identifier: GPL-3.0-or-later
// T-207 patch 0038: the pacing must not change what the guest sees.
//
// The scheduler paces the game on the host clock (a vertical blank is processed when its emulated cycle AND
// its host deadline have passed), and the intent is that the host clock decides only how long the host
// sleeps. This test runs the SAME guest twice in one process: once undisturbed (the host thread is early at
// every blank and sleeps), and once with 20 ms of host time burnt inside the vertical-blank callback (more
// than one 16.7 ms period, so from the second blank on the host deadline of the next blank is already past:
// the thread is always late and never sleeps). At every blank it records what a guest or the IOP could tell
// apart: the EE cycle, the IOP batch statistics (handed-over cycles, pending debt, runs), what the host
// SPU2 saw through advance(), the number of calls of the guest so far, and the EE timer interrupts the
// guest's handler took (how many, and the EE cycle of the last). The two traces must be equal line by line.
//
// The guest is the game's shape: a frame loop that waits for the vertical blank (the idle path), alternated
// with a stretch where it spins for more than a frame without waiting (the busy path), with an EE timer
// (TIM0, compare, zero return) that interrupts every 640,000 EE cycles, i.e. several times per frame, and an
// INTC handler for it. While the guest idles in the wait, the timer's next interrupt falls before the
// blank, and which of the two the scheduler stops at first must not depend on the host clock.

#include "host_test_support.h"

#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_host_gs.h"
#include "ps2x/iop/iop_host_spu2.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

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
    constexpr uint32_t kHandler = 0x00100100u;
    constexpr uint32_t kTim0Count = 0x10000000u;
    constexpr uint32_t kTim0Mode = 0x10000010u;
    constexpr uint32_t kTim0Compare = 0x10000020u;
    // ZRET | CUE | CMPE | clock select 1 (kEeClockHz / 32: 32 EE cycles are one tick).
    constexpr uint32_t kTimerMode = (1u << 6) | (1u << 7) | (1u << 8) | 1u;
    constexpr uint32_t kEquf = 1u << 10;
    constexpr uint32_t kCompare = 20000u; // ticks: 640,000 EE cycles
    constexpr uint32_t kIntcTim0 = 9u;

    constexpr int kBlanks = 36;
    // The guest's frame loop, by blank: wait for the blank in the idle path, except on blanks 10..13 and
    // 24..27 where it spins instead (no wait at all: the blank is crossed by execution, and the guest goes
    // back to waiting mid-frame afterwards).

    struct Line
    {
        uint64_t blank, eeCycle, handedOver, pending, runs, spu2Cycles, spu2Advances, guestCalls, timerIrqs,
            lastTimerIrqCycle;
        bool operator==(const Line &o) const
        {
            return blank == o.blank && eeCycle == o.eeCycle && handedOver == o.handedOver &&
                   pending == o.pending && runs == o.runs && spu2Cycles == o.spu2Cycles &&
                   spu2Advances == o.spu2Advances && guestCalls == o.guestCalls && timerIrqs == o.timerIrqs &&
                   lastTimerIrqCycle == o.lastTimerIrqCycle;
        }
    };

    std::string text(const Line &l)
    {
        return "blank=" + num(l.blank) + " ee=" + num(l.eeCycle) + " iopHanded=" + num(l.handedOver) +
               " iopPending=" + num(l.pending) + " iopRuns=" + num(l.runs) + " spu2=" + num(l.spu2Cycles) + "/" +
               num(l.spu2Advances) + " guestCalls=" + num(l.guestCalls) + " timerIrqs=" + num(l.timerIrqs) +
               " lastIrqEe=" + num(l.lastTimerIrqCycle);
    }

    struct FakeSpu2
    {
        uint64_t cycles = 0;
        uint64_t advances = 0;
        ps2x::iop::IopHostSpu2 hooks;

        FakeSpu2()
        {
            hooks.user = this;
            hooks.write16 = [](void *, uint32_t, uint16_t) {};
            hooks.read16 = [](void *, uint32_t) -> uint16_t { return 0; };
            hooks.dma_write = [](void *, int, const uint8_t *, uint32_t) {};
            hooks.dma_read = [](void *, int, uint8_t *, uint32_t) {};
            hooks.advance = [](void *user, uint64_t iopCycles) {
                auto *self = static_cast<FakeSpu2 *>(user);
                self->cycles += iopCycles;
                ++self->advances;
            };
            hooks.poll_event = [](void *, int *, uint64_t *) -> int { return 0; };
        }
    };

    // The guest and the recorder share these (one game thread runs both).
    struct Run
    {
        PS2Runtime *rt = nullptr;
        FakeSpu2 *spu2 = nullptr;
        int delayMs = 0;
        uint64_t guestCalls = 0;
        uint64_t blanksSeen = 0;
        uint64_t timerIrqs = 0;
        uint64_t lastTimerIrqCycle = 0;
        std::vector<Line> trace;
        std::atomic<bool> done{false};
    } *g_run = nullptr;

    bool inSpinWindow(uint64_t blank)
    {
        return (blank >= 10 && blank <= 13) || (blank >= 24 && blank <= 27);
    }

    void handlerEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_run->timerIrqs;
        g_run->lastTimerIrqCycle = runtime->eeScheduler().eeCycle();
        // Acknowledge the compare flag (write one to clear) so that the timer interrupts again.
        runtime->memory().write32(kTim0Mode, kTimerMode | kEquf);
        ctx->pc = 0u; // return to the interrupt trampoline
    }

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        Run &run = *g_run;
        const uint64_t call = ++run.guestCalls;
        if (call == 1u)
        {
            runtime->eeScheduler().addIrqHandler(false, kIntcTim0, kHandler, true, 0u, 0u, 0u);
            runtime->memory().write32(kTim0Count, 0u);
            runtime->memory().write32(kTim0Compare, kCompare);
            runtime->memory().write32(kTim0Mode, kTimerMode);
            return;
        }
        if (run.blanksSeen >= static_cast<uint64_t>(kBlanks))
        {
            run.done = true;
            ctx->pc = 0u; // the guest's main thread ends
            return;
        }
        if (inSpinWindow(run.blanksSeen))
        {
            // Busy stretch: no wait at all; the blank is crossed by execution. The blank handler counts
            // the blank, so the stretch ends by itself when it is crossed.
            return;
        }
        runtime->eeWaitVSyncTicks(1, kEntry);
    }

    void onVsync(Run &run)
    {
        ++run.blanksSeen;
        const EeIopBatchStats stats = run.rt->eeScheduler().iopBatchStats();
        run.trace.push_back(Line{run.blanksSeen, run.rt->eeScheduler().eeCycle(), stats.eeCycles,
                                 stats.pendingEeCycles, stats.runs, run.spu2->cycles, run.spu2->advances,
                                 run.guestCalls, run.timerIrqs, run.lastTimerIrqCycle});
        if (run.delayMs > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(run.delayMs));
        }
    }

    struct Result
    {
        bool finished = false;
        std::vector<Line> trace;
        double wallSeconds = 0.0;
    };

    Result runOnce(int delayMs)
    {
        FakeSpu2 spu2;
        ps2x::iop::IopHostSpu2 hooks = spu2.hooks;
        ps2x::iop::iopSetHostSpu2(&hooks);

        Run run;
        run.spu2 = &spu2;
        run.delayMs = delayMs;
        g_run = &run;

        Ps2HostGs gs{};
        gs.user = &run;
        gs.thread_begin = [](void *) {};
        gs.thread_end = [](void *) {};
        gs.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
        gs.vsync = [](void *user, const uint64_t *, int) { onVsync(*static_cast<Run *>(user)); };

        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        run.rt = &rt;
        expect(rt.initialize("pacing-neutral-test"), "initialize()");
        rt.setHostGs(&gs);
        expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
        expect(rt.registerFunction(kHandler, handlerEntry), "registerFunction for the timer handler");
        rt.cpu().pc = kEntry;
        const auto from = Clock::now();
        expect(rt.startGameThread(), "startGameThread()");
        for (int i = 0; i < 3000 && !run.done.load(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto to = Clock::now();
        rt.stopGameThread();
        ps2x::iop::iopSetHostSpu2(nullptr);
        g_run = nullptr;

        Result result;
        result.finished = run.done.load();
        result.trace = run.trace;
        result.wallSeconds = std::chrono::duration<double>(to - from).count();
        return result;
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    const Result calm = runOnce(0);
    expect(calm.finished, "the undisturbed run reached the end of the guest");
    expect(calm.trace.size() >= 30, "at least 30 blanks recorded in the undisturbed run");
    expect(!calm.trace.empty() && calm.trace.back().timerIrqs > 10u,
           "the guest took timer interrupts (positive control of the guest)");
    for (size_t i = 0; i < calm.trace.size() && i < 4; ++i)
    {
        std::printf("[pacing_neutral_test] undisturbed %s\n", text(calm.trace[i]).c_str());
    }

    // 10 ms: the host is still early at every blank, but not by the same margin. 20 ms: more than one
    // period, so from the second blank on the host deadline of the next blank is already past (late).
    for (const int delayMs : {10, 20})
    {
        const Result other = runOnce(delayMs);
        std::printf("[pacing_neutral_test] undisturbed: %zu blanks in %.2f s; %d ms of host time per blank: %zu blanks in %.2f s\n",
                    calm.trace.size(), calm.wallSeconds, delayMs, other.trace.size(), other.wallSeconds);
        expect(other.finished, "the run with " + num(delayMs) + " ms per blank reached the end of the guest");
        if (delayMs > 17)
        {
            // It must really have been late (else it proves nothing): it takes at least the delay per blank.
            expect(other.wallSeconds > calm.wallSeconds + 0.1,
                   "the late run took longer than the paced one");
        }
        size_t differing = 0;
        const size_t count = std::min(calm.trace.size(), other.trace.size());
        for (size_t i = 0; i < count; ++i)
        {
            if (!(calm.trace[i] == other.trace[i]))
            {
                if (differing < 3)
                {
                    std::printf("[pacing_neutral_test] DIFF undisturbed: %s\n", text(calm.trace[i]).c_str());
                    std::printf("[pacing_neutral_test] DIFF %2d ms      : %s\n", delayMs, text(other.trace[i]).c_str());
                }
                ++differing;
            }
        }
        expect(calm.trace.size() == other.trace.size(),
               "the traces have the same length: " + num(calm.trace.size()) + " and " + num(other.trace.size()));
        expect(differing == 0u, "with " + num(delayMs) + " ms of host time per blank the guest state at every blank is the "
                                "same as undisturbed: " + num(differing) + " of " + num(count) + " blanks differ");
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "pacing_neutral_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("pacing_neutral_test: all checks passed\n");
    return 0;
}
