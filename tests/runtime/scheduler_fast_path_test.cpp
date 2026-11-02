// SPDX-License-Identifier: GPL-3.0-or-later
// T-208 patch 0034: the EE scheduler's fast path (a counter and one flag word instead of a clock read, a
// lock and an allocation on every dispatch).
//
// The guest is a host function registered at a guest address (no generated code, no ELF, no game), as in
// iop_batch_test.cpp: every dispatch is one generated-code checkpoint, a checkpoint is
// EeScheduler::kGuestDispatchCycles EE cycles, and the guest's own loop is the work the port's recompiled
// functions would do.
//
// What is checked (the reference is the same test built against the tree without the patch, see
// work/profile/notes/0034-wip.diff's report):
//   * the vertical blanks arrive at the same emulated cycles: the host GS's vsync callback records the
//     cycle of every blank and consecutive blanks are exactly one kVBlankPeriodCycles apart;
//   * the callback is called once per blank (the blank is an event, not a poll);
//   * the I/O processor receives exactly the same cycles as before: what the guest accounted is handed
//     over or still pending, the fake host SPU2 sees EE cycles / 8, and the IOP is run about once per
//     batch (the same check as iop_batch_test.cpp);
//   * a stop request posted from another thread while the guest spins is acted on within a few dispatches,
//     not up to the next vertical blank;
//   * the dispatch counters (guarded by PS2X_EE_SCHEDULER_STATS, so that this file also builds against a
//     tree without the patch): the slow path is entered a small fraction of the dispatches, and the fast
//     path answers the rest;
//   * the host nanosecond per dispatch of the spinning guest (printed, not asserted).
//
// The nanosecond figure and the cycle trace are what the two builds are compared on.

#include "host_test_support.h"

#include "ps2x/iop/iop_host_spu2.h"
#include "runtime/ee_scheduler.h"

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

    // One emulator slice in EE cycles (iop_batch_test.cpp): 256 IOP instructions at the console's 8 EE
    // cycles per IOP instruction. Written out so the test does not follow the code it verifies.
    constexpr uint64_t kBatchEeCycles = 256u * 8u;
    // One vertical blank of the console, in EE cycles: 16667 us at 294.912 MHz, rounded up (the same
    // arithmetic EeScheduler's kVBlankPeriodCycles uses).
    constexpr uint64_t kVBlankPeriodCycles = (16667ull * 294912000ull + 999999ull) / 1000000ull;

    constexpr uint32_t kEntry = 0x00100000u;
    // The rate window: the first dispatches of the run, measured before any vertical blank is due (100,000
    // x 8 = 800,000 EE cycles, far below one 4,915,301-cycle blank).
    constexpr uint64_t kRateWindow = 100000u;
    // The spin that crosses at least two vertical blanks (1,400,000 x 8 = 11.2M EE cycles), then the wait
    // for the next one, and then a slow spin during which another thread asks the runtime to stop.
    constexpr uint64_t kSpinLoops = 1400000u;
    constexpr uint64_t kStopSpinLoops = 4000000u;

    struct State
    {
        std::atomic<uint64_t> calls{0};
        std::atomic<bool> stopRequested{false};
        std::atomic<uint64_t> callsAtStopRequest{0};
        std::atomic<uint64_t> callsAtEnd{0};
        // One line per vertical blank, from the host GS's vsync callback (executor thread only). tick and
        // calls are recorded by both builds; the EE cycle needs the patch's eeCycle() accessor.
        std::vector<uint64_t> blankTicks;
        std::vector<uint64_t> blankCalls;
#if defined(PS2X_EE_SCHEDULER_STATS)
        std::vector<uint64_t> blankCycles;
#endif
        uint64_t rateWindowStart = 0;
        uint64_t rateWindowEnd = 0;
        uint64_t dispatchesAtRateWindowEnd = 0;
    } g_state;

    PS2Runtime *g_runtime = nullptr;

    // The host GS: the vsync callback is the vertical blank, one call per blank, on the game thread.
    Ps2HostGs g_hostGs;

    void onVsync(void *user, const uint64_t *, int)
    {
        auto *state = static_cast<State *>(user);
        state->blankTicks.push_back(g_runtime != nullptr ? g_runtime->eeScheduler().currentVSyncTick() : 0u);
        state->blankCalls.push_back(state->calls.load(std::memory_order_relaxed));
#if defined(PS2X_EE_SCHEDULER_STATS)
        state->blankCycles.push_back(g_runtime != nullptr ? g_runtime->eeScheduler().eeCycle() : 0u);
#endif
    }

    // The "guest": a host function at kEntry. It spins (each call is one dispatch), records the host time
    // of the first dispatches, waits for two vertical blanks by execution, then spins slowly while the main
    // thread asks for a stop.
    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t call = ++g_state.calls;
        if (call == 1)
        {
            g_state.rateWindowStart = Clock::now().time_since_epoch().count();
        }
        else if (call == kRateWindow + 1)
        {
            g_state.rateWindowEnd = Clock::now().time_since_epoch().count();
#if defined(PS2X_EE_SCHEDULER_STATS)
            g_state.dispatchesAtRateWindowEnd = runtime->eeScheduler().dispatchStats().dispatches;
#endif
        }
        else if (call == kSpinLoops + 1)
        {
            // Hand the EE to the next vertical blank; the scheduler resumes the thread at kEntry.
            runtime->eeWaitVSyncTicks(1, kEntry);
        }
        else if (call >= kSpinLoops + 2)
        {
            // The slow phase: a real recompiled function is not this cheap, and the busy loop makes the
            // main thread's own latency (a read and a store) small compared with one dispatch, so the
            // count below is a measurement of the scheduler and not of the test.
            volatile uint64_t sink = 0;
            for (int i = 0; i < 4000; ++i)
            {
                sink = sink + static_cast<uint64_t>(i);
            }
            if (g_state.stopRequested.load(std::memory_order_acquire))
            {
                g_state.callsAtEnd.store(call, std::memory_order_release);
                ctx->pc = 0u; // the guest's main thread ends
            }
            else if (call >= kSpinLoops + 2 + kStopSpinLoops)
            {
                // The control: without the stop request the guest would have ended here on its own.
                g_state.callsAtEnd.store(call, std::memory_order_release);
                ctx->pc = 0u;
            }
        }
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    // The fake host SPU2: it counts what the IOP emulator actually executed (see iop_batch_test.cpp).
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
    } spu2;
    ps2x::iop::IopHostSpu2 hooks = spu2.hooks;
    ps2x::iop::iopSetHostSpu2(&hooks);

    PS2Runtime rt;
    g_runtime = &rt;
    g_hostGs.user = &g_state;
    g_hostGs.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
    g_hostGs.vsync = &onVsync;
    g_hostGs.reset = [](void *) {};
    g_hostGs.read_fifo = [](void *, uint8_t *, uint32_t) -> uint32_t { return 0; };

    rt.setHostOwnsWindow(true);
    expect(rt.initialize("scheduler-fast-path-test"), "initialize()");
    rt.setHostGs(&g_hostGs);
    expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
    rt.cpu().pc = kEntry;

    const Clock::time_point from = Clock::now();
    expect(rt.startGameThread(), "startGameThread()");

    // Let the guest spin, then ask it to stop from this thread while it is in its slow phase.
    uint64_t waits = 0;
    while (g_state.calls.load(std::memory_order_acquire) < kSpinLoops + 2 + 1000 && waits < 20000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++waits;
    }
    const uint64_t callsBeforeRequest = g_state.calls.load(std::memory_order_acquire);
    g_state.callsAtStopRequest.store(callsBeforeRequest, std::memory_order_release);
    g_state.stopRequested.store(true, std::memory_order_release);
    rt.requestStop();

    for (int i = 0; i < 5000 && g_state.callsAtEnd.load(std::memory_order_acquire) == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    rt.stopGameThread();
    const Clock::time_point to = Clock::now();
    ps2x::iop::iopSetHostSpu2(nullptr);

    const uint64_t callsAtEnd = g_state.callsAtEnd.load(std::memory_order_acquire) != 0
                                    ? g_state.callsAtEnd.load(std::memory_order_acquire)
                                    : g_state.calls.load(std::memory_order_acquire);
    const EeIopBatchStats iop = rt.eeScheduler().iopBatchStats();
    const EeKernelSnapshot kernel = rt.eeScheduler().snapshot();
    const uint64_t rateNs = g_state.rateWindowEnd > g_state.rateWindowStart
                                ? g_state.rateWindowEnd - g_state.rateWindowStart
                                : 0u;
    const double nsPerDispatch =
        rateNs != 0u ? static_cast<double>(rateNs) / static_cast<double>(kRateWindow) : 0.0;

    // The trace: one line per vertical blank, and the numbers the two builds are compared on. The tick and
    // the dispatch (guest call) count are recorded by both builds, so the two traces can be compared line
    // by line; the EE cycle of the blank needs the patch's eeCycle() and is printed on a line of its own.
    for (size_t i = 0; i < g_state.blankTicks.size(); ++i)
    {
        std::printf("[scheduler_fast_path_test] blank %zu tick=%s calls=%s\n", i,
                    num(g_state.blankTicks[i]).c_str(), num(g_state.blankCalls[i]).c_str());
#if defined(PS2X_EE_SCHEDULER_STATS)
        std::printf("[scheduler_fast_path_test] blank %zu eeCycle=%s\n", i, num(g_state.blankCycles[i]).c_str());
#endif
    }
    std::printf("[scheduler_fast_path_test] vblanks=%zu calls=%s stop after=%s dispatches "
                "nsPerDispatch=%.2f wallMs=%.1f finalEeCycle=%s\n",
                g_state.blankTicks.size(), num(callsAtEnd).c_str(),
                num(callsAtEnd >= g_state.callsAtStopRequest.load() ? callsAtEnd - g_state.callsAtStopRequest.load() : 0u)
                    .c_str(),
                nsPerDispatch,
                std::chrono::duration<double, std::milli>(to - from).count(),
                num(rt.eeScheduler().snapshot().eeCycle).c_str());
#if defined(PS2X_EE_SCHEDULER_STATS)
    const EeDispatchStats dispatches = rt.eeScheduler().dispatchStats();
    std::printf("[scheduler_fast_path_test] dispatches=%s fastPath=%s slowPath=%s nextCycle=%s "
                "iopBatches=%s iopEeCycles=%s pending=%s\n",
                num(dispatches.dispatches).c_str(), num(dispatches.fastPath).c_str(),
                num(dispatches.slowPath).c_str(), num(dispatches.nextCycle).c_str(), num(iop.runs).c_str(),
                num(iop.eeCycles).c_str(), num(iop.pendingEeCycles).c_str());
#endif

    expect(g_state.blankTicks.size() >= 2u,
           "at least two vertical blanks, got " + num(g_state.blankTicks.size()));
    for (size_t i = 1; i < g_state.blankTicks.size(); ++i)
    {
        expect(g_state.blankTicks[i] == g_state.blankTicks[i - 1] + 1u,
               "blank " + num(i) + " is the next tick: got " + num(g_state.blankTicks[i]) + ", expected " +
                   num(g_state.blankTicks[i - 1] + 1u));
        expect(g_state.blankCalls[i] > g_state.blankCalls[i - 1],
               "the guest ran between two blanks: " + num(g_state.blankCalls[i - 1]) + " -> " +
                   num(g_state.blankCalls[i]));
    }
#if defined(PS2X_EE_SCHEDULER_STATS)
    // The blanks are scheduled one kVBlankPeriodCycles apart; the cycle recorded here is the first
    // checkpoint at or after the blank's cycle deadline, so it can be up to one checkpoint later than the
    // deadline. The trace line above (tick, calls) is the exact cross-build comparison.
    for (size_t i = 1; i < g_state.blankCycles.size(); ++i)
    {
        const uint64_t previous = g_state.blankCycles[i - 1];
        const uint64_t current = g_state.blankCycles[i];
        const uint64_t spacing = current - previous;
        const uint64_t distance =
            spacing > kVBlankPeriodCycles ? spacing - kVBlankPeriodCycles : kVBlankPeriodCycles - spacing;
        expect(distance <= EeScheduler::kGuestDispatchCycles,
               "blank " + num(i) + " is one period after blank " + num(i - 1) + " within one checkpoint: got " +
                   num(spacing) + ", expected " + num(kVBlankPeriodCycles) + " +/- " +
                   num(EeScheduler::kGuestDispatchCycles));
    }
#endif

    // The IOP feed is the one patch 0033 defined and this patch must not change.
    expect(iop.eeCycles + iop.pendingEeCycles == kernel.eeCycle,
           "handed over + pending = the guest's EE cycles: " + num(iop.eeCycles) + " + " +
               num(iop.pendingEeCycles) + " != " + num(kernel.eeCycle));
    expect(iop.pendingEeCycles < iop.batchEeCycles,
           "the debt left over is smaller than one batch, got " + num(iop.pendingEeCycles));
    expect(iop.batchEeCycles == kBatchEeCycles,
           "one batch is " + num(kBatchEeCycles) + " EE cycles, got " + num(iop.batchEeCycles));
    expect(spu2.cycles == iop.eeCycles / 8u,
           "the IOP ran exactly the EE cycles it was handed over / 8: the fake host SPU2 saw " +
               num(spu2.cycles) + ", expected " + num(iop.eeCycles / 8u));

    // The stop request came from this thread while the guest was spinning in its slow phase. The bound is
    // one dispatch (the flag is read on every dispatch); the figure below is a measurement, so it also
    // carries this thread's own latency of a read and a store.
    const uint64_t stopLatency = callsAtEnd >= g_state.callsAtStopRequest.load()
                                     ? callsAtEnd - g_state.callsAtStopRequest.load()
                                     : 0u;
    expect(callsAtEnd > kSpinLoops + 2, "the stop happened in the slow phase, calls=" + num(callsAtEnd));
    expect(callsAtEnd < kSpinLoops + 2 + kStopSpinLoops,
           "the stop cut the control spin short: calls=" + num(callsAtEnd));
    expect(stopLatency <= 8u,
           "a stop request from another thread was seen within a few dispatches, got " + num(stopLatency));

#if defined(PS2X_EE_SCHEDULER_STATS)
    expect(dispatches.dispatches >= kRateWindow, "the run dispatched at least the rate window");
    expect(dispatches.fastPath + dispatches.slowPath == dispatches.dispatches,
           "every dispatch is answered by the fast path or the slow path: " + num(dispatches.fastPath) +
               " + " + num(dispatches.slowPath) + " != " + num(dispatches.dispatches));
    expect(dispatches.slowPath >= 2u, "the slow path ran at least for the vertical blanks");
    expect(dispatches.slowPath * 100u <= dispatches.dispatches,
           "the slow path is a small fraction of the dispatches: " + num(dispatches.slowPath) + " of " +
               num(dispatches.dispatches));
    expect(g_state.dispatchesAtRateWindowEnd >= kRateWindow,
           "the rate window recorded the dispatch counters");
#else
    std::printf("[scheduler_fast_path_test] no PS2X_EE_SCHEDULER_STATS: the counter checks are skipped\n");
#endif

    if (g_failures != 0)
    {
        std::fprintf(stderr, "scheduler_fast_path_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("scheduler_fast_path_test: all checks passed\n");
    return 0;
}
