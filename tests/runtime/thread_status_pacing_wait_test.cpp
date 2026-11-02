// SPDX-License-Identifier: GPL-3.0-or-later
// T-305 (journal 034, follow-up 13), patch 0068: what ReferThreadStatus says about a thread in a wait the
// runtime makes up.
//
// The runtime paces sceMpegGetPicture and the CD stream with a vertical-sync wait inside the calling thread
// (reason VSync, Mpeg or External). On the console that time is code running, not a kernel wait. The game's
// CRI library reads the status of its server thread and calls WakeupThread when it sees a wait (0x314AB0:
// status 4 or 0xC); with the pacing wait reported as a wait the wakeups were only counted (157 by the Start in
// the opening movie, the console's count is always 0) and each later SleepThread returned at once.
//
// The guest is host functions on the runtime's own game thread, so the threads really block:
//   * the main thread (priority 1) starts a worker (priority 100), then waits one vertical blank, so the worker
//     runs; the worker sets a flag and waits many blanks through eeWaitVSyncTicks (reason VSync);
//   * ReferThreadStatus of the worker answers READY (2), not WAIT (4);
//   * after SuspendThread the worker answers SUSPEND (8);
//   * the control: a second worker that sleeps (SleepThread, a kernel wait) still reads WAIT (4), so the patch
//     does not hide real waits.
// Without patch 0068 the first answer is 4 and the second 12 (WAIT+SUSPEND). No game, no generated code, no
// window; DISPLAY is unset by the test.

#include "host_test_support.h"

#include "Syscalls/Helpers/State.h" // ee_thread_t, ee_thread_status_t
#include "Syscalls/Thread.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    using Clock = std::chrono::steady_clock;
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    constexpr uint32_t kMainEntry = 0x00100000u;
    constexpr uint32_t kMainAfter = 0x00100100u;
    constexpr uint32_t kPacerEntry = 0x00101000u;
    constexpr uint32_t kSleeperEntry = 0x00101100u;
    constexpr uint32_t kParamAddr = 0x00102000u;
    constexpr uint32_t kStatusAddr = 0x00102100u;
    constexpr uint32_t kPacerStack = 0x00180000u;
    constexpr uint32_t kSleeperStack = 0x00190000u;

    struct Observation
    {
        std::atomic<int> pacerId{0};
        std::atomic<int> sleeperId{0};
        std::atomic<bool> pacerWaiting{false};
        std::atomic<bool> sleeperSleeping{false};
        std::atomic<int> pacingStatus{-1};
        std::atomic<int> suspendedStatus{-1};
        std::atomic<int> sleepingStatus{-1};
        std::atomic<bool> done{false};
    };

    Observation g;

    int32_t callHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Handler fn, uint32_t a0 = 0,
                        uint32_t a1 = 0)
    {
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        fn(rdram, ctx, runtime);
        return static_cast<int32_t>(getRegU32(ctx, 2));
    }

    int statusOf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int id)
    {
        std::memset(rdram + kStatusAddr, 0, sizeof(ee_thread_status_t));
        callHandler(rdram, ctx, runtime, ps2_syscalls::ReferThreadStatus, static_cast<uint32_t>(id), kStatusAddr);
        return reinterpret_cast<const ee_thread_status_t *>(rdram + kStatusAddr)->status;
    }

    int makeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t entry, uint32_t stack)
    {
        std::memset(rdram + kParamAddr, 0, sizeof(ee_thread_t));
        auto *param = reinterpret_cast<ee_thread_t *>(rdram + kParamAddr);
        param->func = entry;
        param->stack = stack;
        param->stack_size = 0x1000;
        param->initial_priority = 100;
        const int id = callHandler(rdram, ctx, runtime, ps2_syscalls::CreateThread, kParamAddr);
        if (id > 0)
        {
            callHandler(rdram, ctx, runtime, ps2_syscalls::StartThread, static_cast<uint32_t>(id), 0u);
        }
        return id;
    }

    // The main thread: two workers, then one vertical blank to let them run, then the questions.
    void guestMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        callHandler(rdram, ctx, runtime, ps2_syscalls::ChangeThreadPriority, EeScheduler::kMainThreadId, 1);
        g.pacerId.store(makeThread(rdram, ctx, runtime, kPacerEntry, kPacerStack));
        g.sleeperId.store(makeThread(rdram, ctx, runtime, kSleeperEntry, kSleeperStack));
        runtime->eeWaitVSyncTicks(1, kMainAfter);
    }

    void guestMainAfter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g.done.load())
        {
            runtime->eeWaitVSyncTicks(1, kMainAfter); // idle until the test stops the runtime
        }
        if (!g.pacerWaiting.load() || !g.sleeperSleeping.load())
        {
            runtime->eeWaitVSyncTicks(1, kMainAfter); // the workers have not run yet
        }
        g.pacingStatus.store(statusOf(rdram, ctx, runtime, g.pacerId.load()));
        callHandler(rdram, ctx, runtime, ps2_syscalls::SuspendThread, static_cast<uint32_t>(g.pacerId.load()));
        g.suspendedStatus.store(statusOf(rdram, ctx, runtime, g.pacerId.load()));
        g.sleepingStatus.store(statusOf(rdram, ctx, runtime, g.sleeperId.load()));
        g.done.store(true);
        runtime->eeWaitVSyncTicks(1, kMainAfter);
    }

    // Waits 100000 vertical blanks: far longer than the test.
    void guestPacer(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        g.pacerWaiting.store(true);
        runtime->eeWaitVSyncTicks(100000, kPacerEntry + 4); // never reached
    }

    void guestSleeper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g.sleeperSleeping.store(true);
        callHandler(rdram, ctx, runtime, ps2_syscalls::SleepThread); // a kernel wait
    }

    void scenario()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("thread-status-pacing-wait-test"), "initialize() with the host owning the window");
        expect(rt.registerFunction(kMainEntry, guestMain), "registerFunction(main)");
        expect(rt.registerFunction(kMainAfter, guestMainAfter), "registerFunction(main, after the blank)");
        expect(rt.registerFunction(kPacerEntry, guestPacer), "registerFunction(pacer)");
        expect(rt.registerFunction(kSleeperEntry, guestSleeper), "registerFunction(sleeper)");
        rt.cpu().pc = kMainEntry;

        expect(rt.startGameThread(), "startGameThread() returns true");
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < deadline && !g.done.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        rt.stopGameThread();
        expect(g.done.load(), "the main thread reached its questions");
        expect(g.pacerId.load() > 1 && g.sleeperId.load() > 1, "both workers were created");

        expect(g.pacingStatus.load() == 2,
               "a thread in the runtime's vertical-sync pacing wait reads READY (2), got " +
                   std::to_string(g.pacingStatus.load()));
        expect(g.suspendedStatus.load() == 8,
               "the same thread, suspended, reads SUSPEND (8), got " + std::to_string(g.suspendedStatus.load()));
        expect(g.sleepingStatus.load() == 4,
               "the control: a thread in SleepThread still reads WAIT (4), got " +
                   std::to_string(g.sleepingStatus.load()));
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    scenario();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "thread_status_pacing_wait_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("thread_status_pacing_wait_test: all checks passed\n");
    return 0;
}
