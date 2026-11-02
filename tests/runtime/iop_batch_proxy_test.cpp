// SPDX-License-Identifier: GPL-3.0-or-later
// T-108 patch 0033, independent measurement: how often the EE scheduler makes the IOP emulator run.
//
// This test does not trust the scheduler's own counter: it measures the IOP from outside, through the host
// SPU2 hook, which ps2xIOP calls once per emulator slice it executes (IopEmulator::Impl::runCycles ->
// syncHostSpu2 -> advance). Before the patch the scheduler called runEeCycles once per generated-code
// checkpoint (8 EE cycles), so that hook saw one call per checkpoint; with the patch it sees one per batch
// (a full emulator slice, 2048 EE cycles).
//
// The guest runs enough checkpoints to cross a vertical-blank deadline by execution, waits for that blank
// (a flush point) and ends. Both trees must agree on the totals (the ratio is 1 IOP cycle per 8 EE cycles,
// nothing lost, nothing added); only the number of runs may differ.

#include "host_test_support.h"

#include "ps2x/iop/iop_host_spu2.h"
#include "runtime/ee_scheduler.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
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

    std::string num(uint64_t value)
    {
        return std::to_string(value);
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

    constexpr uint32_t kEntry = 0x00100000u;
    // Enough checkpoints to cross the first vertical-blank deadline (16667 us * 294.912 MHz / 8) by
    // execution: the test measures the batching, not the idle path.
    constexpr int kLoops = 640000;
    // One batch in EE cycles: the emulator's slice (256 IOP instructions) at 8 EE cycles per IOP cycle. A
    // literal on purpose: this test is the specification, it must not follow the code it verifies.
    constexpr uint64_t kBatchEeCycles = 256u * 8u;

    int g_calls = 0;

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_calls;
        if (g_calls == kLoops + 1)
        {
            runtime->eeWaitVSyncTicks(1, kEntry); // does not return; the scheduler resumes at kEntry
        }
        if (g_calls == kLoops + 2)
        {
            ctx->pc = 0u; // the guest's main thread ends
        }
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    FakeSpu2 spu2;
    ps2x::iop::IopHostSpu2 hooks = spu2.hooks;
    ps2x::iop::iopSetHostSpu2(&hooks);

    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("iop-batch-proxy-test"), "initialize() with the host owning the window");
    expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
    rt.cpu().pc = kEntry;
    expect(rt.startGameThread(), "startGameThread()");
    for (int i = 0; i < 1000 && g_calls < kLoops + 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rt.stopGameThread();
    ps2x::iop::iopSetHostSpu2(nullptr);

    const EeKernelSnapshot kernel = rt.eeScheduler().snapshot();
    std::printf("[iop_batch_proxy_test] guest calls=%d eeCycle=%s | iop advances=%s cycles=%s\n", g_calls,
                num(kernel.eeCycle).c_str(), num(spu2.advances).c_str(), num(spu2.cycles).c_str());

    expect(g_calls == kLoops + 2, "the guest ran " + num(kLoops + 2) + " times, got " + num(g_calls));
    if (g_calls < kLoops + 2)
    {
        std::printf("iop_batch_proxy_test: the guest did not finish\n");
        return 1;
    }

    // The ratio is untouched: every EE cycle the guest accounted reaches the IOP, one IOP cycle per eight.
    expect(spu2.cycles == kernel.eeCycle / 8u,
           "the IOP ran the guest's EE cycles / 8: host SPU2 saw " + num(spu2.cycles) + ", expected " +
               num(kernel.eeCycle / 8u));
    // Batches, not checkpoints: the IOP is run about once per full slice, plus the flush points crossed.
    expect(spu2.advances <= kernel.eeCycle / kBatchEeCycles + 8u,
           "the IOP ran " + num(spu2.advances) + " times for " + num(kernel.eeCycle / kBatchEeCycles) +
               " batches (plus at most the flush points crossed)");
    expect(spu2.advances * 100u <= static_cast<uint64_t>(kLoops),
           "the IOP ran " + num(spu2.advances) + " times for " + num(kLoops) + " checkpoints");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "iop_batch_proxy_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("iop_batch_proxy_test: all checks passed\n");
    return 0;
}
