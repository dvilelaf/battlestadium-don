// SPDX-License-Identifier: GPL-3.0-or-later
// T-108 patch 0033: the EE scheduler hands the IOP emulator its cycles in batches.
//
// The guest is a host function registered at a guest address (no generated code, no ELF, no game), as in
// host_loop_test.cpp: every dispatch is one generated-code checkpoint, and a checkpoint is
// EeScheduler::kGuestDispatchCycles EE cycles. The guest runs enough of them to cross a vertical blank by
// execution, waits for that blank (the flush point) and then ends.
//
// What is checked:
//   * one batch is a full emulator slice of IOP cycles, in EE cycles (IopSubsystem::sliceEeCycles():
//     kDefaultSlice * 8 = 2048), and nothing is copied from the emulator's constant here;
//   * the IOP receives exactly the same cycles as before the patch: everything the guest accounted is
//     either handed over or still pending, and the cycles the host SPU2 sees are EE cycles / 8;
//   * the number of times the IOP was run stays at about one per batch (plus the flush points crossed),
//     far below the number of checkpoints - before the patch it was one per checkpoint;
//   * a vertical blank flushes the debt: right after the blank the IOP is owed at most the checkpoint that
//     dispatched the guest again.
//
// The fake host SPU2 is the second, independent measurement: IopEmulator::Impl::runCycles calls its
// advance() once per emulator slice it executes, so before the patch it is called once per checkpoint.

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

    // The host SPU2 hooks are global (ps2x/iop/iop_host_spu2.h), so this one is installed for the run.
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
    // execution, so that the test measures batches and not the idle path.
    constexpr int kLoops = 640000;
    // One emulator slice, in EE cycles: 256 IOP instructions at the console's 8 EE cycles per IOP cycle.
    // kDefaultSlice and the ratio live in the emulator (IopSubsystem::sliceEeCycles); this is the value the
    // scheduler must use, written out so that the test does not follow the code it verifies.
    constexpr uint64_t kBatchEeCycles = 256u * 8u;

    struct GuestState
    {
        int calls = 0;
        uint64_t pendingAfterVblank = 0;
    } g_guest;

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_guest.calls;
        if (g_guest.calls == kLoops + 1)
        {
            // Hand the EE over to the vertical blank: the scheduler must bring the IOP up to date before
            // it processes the blank. This does not return; the scheduler resumes the thread at kEntry.
            runtime->eeWaitVSyncTicks(1, kEntry);
        }
        if (g_guest.calls == kLoops + 2)
        {
            g_guest.pendingAfterVblank = runtime->eeScheduler().iopBatchStats().pendingEeCycles;
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
    expect(rt.initialize("iop-batch-test"), "initialize() with the host owning the window");
    expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
    rt.cpu().pc = kEntry;
    expect(rt.startGameThread(), "startGameThread()");

    // Wait for the guest to end (it ends right after the vertical blank). The blank arrives on the host
    // clock, so give it a generous margin.
    for (int i = 0; i < 1000 && g_guest.calls < kLoops + 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rt.stopGameThread();
    ps2x::iop::iopSetHostSpu2(nullptr);

    const EeIopBatchStats stats = rt.eeScheduler().iopBatchStats();
    const EeKernelSnapshot kernel = rt.eeScheduler().snapshot();
    std::printf("[iop_batch_test] guest calls=%d eeCycle=%s batch=%s handover=%s pending=%s runs=%s | "
                "spu2 advances=%s cycles=%s | pending after the blank=%s\n",
                g_guest.calls, num(kernel.eeCycle).c_str(), num(stats.batchEeCycles).c_str(),
                num(stats.eeCycles).c_str(), num(stats.pendingEeCycles).c_str(), num(stats.runs).c_str(),
                num(spu2.advances).c_str(), num(spu2.cycles).c_str(), num(g_guest.pendingAfterVblank).c_str());

    expect(g_guest.calls == kLoops + 2,
           "the guest ran " + num(kLoops + 2) + " times, got " + num(g_guest.calls));
    if (g_guest.calls < kLoops + 2)
    {
        std::printf("iop_batch_test: the guest did not finish; not checking the scheduler statistics\n");
        return 1;
    }

    // One batch is one emulator slice, and the scheduler took it from the emulator.
    expect(stats.batchEeCycles == kBatchEeCycles,
           "one batch is " + num(kBatchEeCycles) + " EE cycles (one emulator slice of 256 IOP cycles), got " +
               num(stats.batchEeCycles));

    // Nothing added, nothing lost: everything the guest accounted has been handed over or is still pending,
    // and what the IOP received through its own counter (the host SPU2) is exactly EE cycles / 8.
    expect(stats.eeCycles + stats.pendingEeCycles == kernel.eeCycle,
           "handed over + pending = the guest's EE cycles: " + num(stats.eeCycles) + " + " +
               num(stats.pendingEeCycles) + " != " + num(kernel.eeCycle));
    expect(stats.pendingEeCycles < stats.batchEeCycles,
           "the debt left over is smaller than one batch, got " + num(stats.pendingEeCycles));
    expect(spu2.cycles == stats.eeCycles / 8u,
           "the IOP ran exactly the EE cycles it was handed over / 8: host SPU2 saw " + num(spu2.cycles) +
               ", expected " + num(stats.eeCycles / 8u));

    // Batches, not checkpoints: at most one run per batch plus the flush points the run crossed (the
    // vertical blank and its idle accounting), and far below the number of checkpoints.
    const uint64_t batches = stats.eeCycles / stats.batchEeCycles;
    expect(stats.runs <= batches + 4u,
           "the IOP was run " + num(stats.runs) + " times for " + num(batches) + " batches (plus at most the "
           "flush points crossed)");
    expect(stats.runs * 100u <= static_cast<uint64_t>(kLoops),
           "the IOP was run " + num(stats.runs) + " times for " + num(kLoops) + " checkpoints");
    expect(spu2.advances >= stats.runs, "every run of the IOP reached the host SPU2 (" + num(spu2.advances) +
                                            " advances for " + num(stats.runs) + " runs)");

    // The vertical blank is a flush point: when the guest is resumed after it, the IOP is owed at most the
    // checkpoint that dispatched it again.
    expect(g_guest.pendingAfterVblank <= EeScheduler::kGuestDispatchCycles,
           "after the vertical blank the debt was at most one checkpoint (" +
               num(EeScheduler::kGuestDispatchCycles) + " EE cycles), got " + num(g_guest.pendingAfterVblank));

    if (g_failures != 0)
    {
        std::fprintf(stderr, "iop_batch_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("iop_batch_test: all checks passed\n");
    return 0;
}
