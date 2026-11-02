// SPDX-License-Identifier: GPL-3.0-or-later
// T-304 patch 0041: an interrupt handler does not start while the guest has interrupts disabled.
//
// The game protects "start a DMA transfer, then mark its queue busy" with DI ... EI, because on the console
// the end-of-transfer interrupt cannot arrive inside that section. The recompiler turns DI and EI into
// clearing and setting bit 16 (EIE) of the status register, and the scheduler used to ignore that bit: the
// handler could start between the start of the transfer and the "busy" mark, find nothing to do, and the
// game then marked the queue busy for an interrupt that never comes again (journals 011 and 026).
//
// The guest here is that sequence, repeated: DI; the DMA channel's end-of-transfer interrupt is raised (the
// call the runtime makes when a write to the channel's control register finishes the transfer, which it does
// at once); a few dozen small calls (each is a scheduler checkpoint); "busy" is set; EI; the handler clears
// "busy" and counts. Checked: the handler never ran while the guest was inside the section, "busy" is clear
// soon after EI, and every interrupt was delivered exactly once.

#include "host_test_support.h"

#include "runtime/ee_scheduler.h"
#include "runtime/gs/ps2_host_gs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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

    constexpr uint32_t kEntry = 0x00100000u;
    constexpr uint32_t kHandler = 0x00100100u;
    constexpr uint32_t kEie = 0x10000u;
    constexpr uint32_t kDmacCause = 1u; // channel 1 (VIF1), the one the game uses
    constexpr int kRounds = 300;
    constexpr int kInsideCalls = 40;
    constexpr int kWaitCalls = 400;

    struct State
    {
        int phase = 0;
        int round = 0;
        int inside = 0;
        int waited = 0;
        bool busy = false;
        bool inSection = false;
        uint64_t handlerCalls = 0;
        uint64_t handlerInsideSection = 0; // the handler started while the guest had interrupts disabled
        uint64_t handlerSawNotBusy = 0;    // ... or before the guest marked the queue busy
        uint64_t neverCleared = 0;         // "busy" still set a few hundred calls after EI
        uint64_t latencySum = 0;
        uint64_t latencyMax = 0;
        std::atomic<bool> done{false};
    } *g_state = nullptr;

    void handlerEntry(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        State &s = *g_state;
        ++s.handlerCalls;
        if (s.inSection)
        {
            ++s.handlerInsideSection;
        }
        if (!s.busy)
        {
            ++s.handlerSawNotBusy; // what the console code does: nothing to send, return
        }
        s.busy = false;
        ctx->pc = 0u; // return to the interrupt trampoline
    }

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &s = *g_state;
        switch (s.phase)
        {
        case 0:
            if (s.round == 0 && s.handlerCalls == 0u && s.waited == 0)
            {
                runtime->eeScheduler().addIrqHandler(true, kDmacCause, kHandler, true, 0u, 0u, 0u);
                s.waited = -1; // registered
                return;
            }
            if (s.round >= kRounds)
            {
                s.done = true;
                ctx->pc = 0u; // the guest's main thread ends
                return;
            }
            ctx->cop0_status &= ~kEie; // DI
            s.inSection = true;
            runtime->eeScheduler().dispatchIrq(true, kDmacCause); // the transfer is carried out and ends
            s.inside = 0;
            s.phase = 1;
            return;
        case 1:
            if (++s.inside < kInsideCalls)
            {
                return; // small calls inside the protected section
            }
            s.busy = true; // "mark the queue busy"
            s.inSection = false;
            ctx->cop0_status |= kEie; // EI
            s.waited = 0;
            s.phase = 2;
            return;
        case 2:
            if (!s.busy)
            {
                s.latencySum += static_cast<uint64_t>(s.waited);
                if (static_cast<uint64_t>(s.waited) > s.latencyMax)
                {
                    s.latencyMax = static_cast<uint64_t>(s.waited);
                }
            }
            else if (++s.waited < kWaitCalls)
            {
                return;
            }
            else
            {
                ++s.neverCleared;
                s.busy = false;
            }
            ++s.round;
            s.phase = 0;
            return;
        default:
            return;
        }
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    State state;
    g_state = &state;
    Ps2HostGs gs{};
    gs.user = &state;
    gs.thread_begin = [](void *) {};
    gs.thread_end = [](void *) {};
    gs.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
    gs.vsync = [](void *, const uint64_t *, int) {};

    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("irq-di-section-test"), "initialize()");
    rt.setHostGs(&gs);
    expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
    expect(rt.registerFunction(kHandler, handlerEntry), "registerFunction for the handler");
    rt.cpu().pc = kEntry;
    expect(rt.startGameThread(), "startGameThread()");
    for (int i = 0; i < 1500 && !state.done.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool finished = state.done.load();
    rt.stopGameThread();

    std::printf("[irq_di_section_test] rounds=%d handlerCalls=%llu insideSection=%llu sawNotBusy=%llu "
                "neverCleared=%llu latency avg=%.1f max=%llu calls after EI\n",
                state.round, static_cast<unsigned long long>(state.handlerCalls),
                static_cast<unsigned long long>(state.handlerInsideSection),
                static_cast<unsigned long long>(state.handlerSawNotBusy),
                static_cast<unsigned long long>(state.neverCleared),
                state.round > 0 ? static_cast<double>(state.latencySum) / state.round : 0.0,
                static_cast<unsigned long long>(state.latencyMax));

    expect(finished, "the guest ran all its rounds");
    expect(state.handlerInsideSection == 0u,
           "the handler never started inside the DI ... EI section: " + std::to_string(state.handlerInsideSection) +
               " of " + std::to_string(kRounds) + " rounds");
    expect(state.handlerSawNotBusy == 0u, "the handler always found the queue marked busy");
    expect(state.neverCleared == 0u, "the handler ran soon after EI in every round");
    expect(state.handlerCalls == static_cast<uint64_t>(kRounds),
           "every interrupt delivered exactly once: " + std::to_string(state.handlerCalls));
    expect(state.latencyMax <= 64u, "the handler started within 64 calls of EI (worst: " +
                                        std::to_string(state.latencyMax) + ")");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "irq_di_section_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("irq_di_section_test: all checks passed\n");
    return 0;
}
