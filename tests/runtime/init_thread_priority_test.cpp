// SPDX-License-Identifier: GPL-3.0-or-later
// T-016 (journal 004): the priority of the EE main thread after the replaced InitThread.
//
// On the console the game's own InitThread (0x10D078, libkernel's) ends with
// ChangeThreadPriority(GetThreadId(), 1). The runtime replaces that whole function by
// ps2_syscalls::InitThread, which only returned the main thread's id, so the main thread kept the
// priority 0 the scheduler initialises it with. The game's CRI middleware (0x314FF0) reads that
// priority with ReferThreadStatus and copies it into the four threads it creates (0x314BE0,
// 0x314D40, 0x314DC8, 0x314E50); the scheduler rejects priority 0 with KE_ILLEGAL_PRIORITY (-403)
// (EeScheduler.cpp:426), so those threads were never created and the ADXF file read they drive
// never completed (journal 004).
//
// The guest here is a host function that does what the game does, on the runtime's own game thread
// (startGameThread), so the created thread is really scheduled:
//   1. InitThread - this is what replaces 0x10D078;
//   2. GetThreadId, then ReferThreadStatus(id, buf): the priority the game reads (0x3150F0);
//   3. CreateThread with the parameter block the game builds (priority = the value it read) and
//      StartThread on it: the thread must be created and its entry must run;
//   4. the control: the same CreateThread with priority 0 must still be rejected with -403. That is
//      the failure of journal 004: it is the value read in step 2, 1 and not 0, that makes the
//      difference, so a regression in the handler cannot pass this test by accident.
// No game, no generated code, no window; DISPLAY is unset by the test.

#include "host_test_support.h"

#include "Syscalls/Helpers/State.h" // ee_thread_t, ee_thread_status_t
#include "Syscalls/Thread.h"        // ps2_syscalls::InitThread, GetThreadId, ReferThreadStatus, ...
#include "runtime/ee_scheduler.h"   // EeScheduler::kMainThreadId

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

    // Guest addresses. The two entry points are host functions (registerFunction); the rest is
    // guest memory inside RDRAM.
    constexpr uint32_t kMainEntry = 0x00100000u;
    constexpr uint32_t kWorkerEntry = 0x00101000u;
    constexpr uint32_t kParamAddr = 0x00102000u;
    constexpr uint32_t kStatusAddr = 0x00102100u;
    constexpr uint32_t kWorkerStack = 0x00180000u;
    constexpr int kWorkerStackSize = 0x1000;

    // The main thread's priority after InitThread, as the console reports it (journal 004).
    constexpr int kMainPriorityOnConsole = 1;

    struct Observation
    {
        std::atomic<int> initThreadReturn{-1};
        std::atomic<int> mainThreadId{-1};
        std::atomic<int> statusReturn{-1};
        std::atomic<int> priorityRead{-1};
        std::atomic<int> createdId{0};
        std::atomic<int> startReturn{-1};
        std::atomic<int> zeroPriorityId{0};
        std::atomic<int> workerRuns{0};
    };

    Observation g;

    // Calls a handler the way the generated code does: arguments in a0/a1, result in v0.
    int32_t callHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, Handler fn,
                        uint32_t a0 = 0, uint32_t a1 = 0)
    {
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        fn(rdram, ctx, runtime);
        return static_cast<int32_t>(getRegU32(ctx, 2));
    }

    // The guest's main thread: the sequence of the game (0x314FF0 and the CRI thread creators).
    void guestMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // 1. what replaces the game's InitThread (0x10D078)
        g.initThreadReturn.store(callHandler(rdram, ctx, runtime, ps2_syscalls::InitThread));

        // 2. GetThreadId + ReferThreadStatus: the priority the game reads and copies
        const int mainId = callHandler(rdram, ctx, runtime, ps2_syscalls::GetThreadId);
        g.mainThreadId.store(mainId);
        const int statusReturn =
            callHandler(rdram, ctx, runtime, ps2_syscalls::ReferThreadStatus,
                        static_cast<uint32_t>(mainId), kStatusAddr);
        g.statusReturn.store(statusReturn);
        int priority = -1;
        if (statusReturn == 0)
        {
            priority = reinterpret_cast<const ee_thread_status_t *>(rdram + kStatusAddr)->current_priority;
        }
        g.priorityRead.store(priority);

        // 3. the parameter block the game builds for its CRI threads, with the priority it read
        std::memset(rdram + kParamAddr, 0, sizeof(ee_thread_t));
        auto *param = reinterpret_cast<ee_thread_t *>(rdram + kParamAddr);
        param->func = kWorkerEntry;
        param->stack = kWorkerStack;
        param->stack_size = kWorkerStackSize;
        param->gp_reg = getRegU32(ctx, 28);
        param->initial_priority = priority;
        const int createdId =
            callHandler(rdram, ctx, runtime, ps2_syscalls::CreateThread, kParamAddr);
        g.createdId.store(createdId);
        if (createdId > 0)
        {
            g.startReturn.store(callHandler(rdram, ctx, runtime, ps2_syscalls::StartThread,
                                            static_cast<uint32_t>(createdId), 0u));
        }

        // 4. the control: priority 0 is still rejected (the bug journal 004 was about)
        param->initial_priority = 0;
        g.zeroPriorityId.store(callHandler(rdram, ctx, runtime, ps2_syscalls::CreateThread, kParamAddr));

        // The guest's main thread ends here; the scheduler then runs the created thread.
        ctx->pc = 0;
    }

    // The guest's created thread: it only says it ran, then ends.
    void guestWorker(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g.workerRuns.fetch_add(1);
        ctx->pc = 0;
    }

    void scenarioMainThreadPriority()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("init-thread-priority-test"), "initialize() with the host owning the window");
        expect(rt.registerFunction(kMainEntry, guestMain), "registerFunction(main guest)");
        expect(rt.registerFunction(kWorkerEntry, guestWorker), "registerFunction(worker guest)");
        rt.cpu().pc = kMainEntry;

        expect(rt.startGameThread(), "startGameThread() returns true");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline && g.workerRuns.load() == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        rt.stopGameThread();
        expect(!rt.isGameThreadRunning(), "the game thread stopped");
        expect(!rt.gameThreadFailed(), "no exception on the game thread");

        expect(g.initThreadReturn.load() == EeScheduler::kMainThreadId,
               "InitThread still returns the main thread's id, got " +
                   std::to_string(g.initThreadReturn.load()));
        expect(g.mainThreadId.load() == EeScheduler::kMainThreadId,
               "GetThreadId is the main thread, got " + std::to_string(g.mainThreadId.load()));
        expect(g.statusReturn.load() == 0, "ReferThreadStatus succeeds, got " +
                                               std::to_string(g.statusReturn.load()));
        expect(g.priorityRead.load() == kMainPriorityOnConsole,
               "after InitThread the main thread's current_priority is 1 (the console's value), got " +
                   std::to_string(g.priorityRead.load()));
        expect(g.createdId.load() > 0, "CreateThread with the priority the game read succeeds, got " +
                                           std::to_string(g.createdId.load()));
        expect(g.startReturn.load() == 0, "StartThread succeeds, got " +
                                              std::to_string(g.startReturn.load()));
        expect(g.workerRuns.load() == 1, "the created thread's entry ran once, got " +
                                             std::to_string(g.workerRuns.load()));
        expect(g.zeroPriorityId.load() == -403,
               "the control: CreateThread with priority 0 is still rejected with -403, got " +
                   std::to_string(g.zeroPriorityId.load()));
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    scenarioMainThreadPriority();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "init_thread_priority_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("init_thread_priority_test: all checks passed "
                "(priority read %d, thread id %d, entry runs %d)\n",
                g.priorityRead.load(), g.createdId.load(), g.workerRuns.load());
    return 0;
}
