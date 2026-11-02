// SPDX-License-Identifier: GPL-3.0-or-later
// T-016 (journal 005): the value ChangeThreadPriority answers with.
//
// The EE kernel's ChangeThreadPriority answers the thread's previous priority, or a negative error
// code. The game depends on it: its CRI library raises its own thread to priority 1 on entering a
// critical section (0x314320: the return value is kept in DAT_003d9c10) and restores the saved value
// on leaving it (0x3143a8: ChangeThreadPriority(GetThreadId(), DAT_003d9c10)). The runtime's handler
// always answered KE_OK (0), so the restore passed 0, which the scheduler rejects with
// KE_ILLEGAL_PRIORITY (-403), and the main thread was left at the 1 the raise had set: at priority 1
// it outranks the four CRI threads (8, 16, 18, 25), this scheduler never preempts a running thread
// for a lower-priority one, and the boot stayed in the ADXF poll loop at 0x331C60 (journal 005).
//
// This test runs the three calls of that pair with the values measured in the game's run: the main
// thread is at 24 (0x18, what 0x314FF0 sets) before the critical section, the raise is to 1, and the
// restore uses what the raise answered. It also checks that an illegal priority is still the error
// code and not the previous priority. No game, no generated code, no window; DISPLAY is unset by
// the test.

#include "host_test_support.h"

#include "Syscalls/Helpers/State.h" // ee_thread_status_t
#include "Syscalls/Thread.h"        // ps2_syscalls::ChangeThreadPriority, ...
#include "runtime/ee_scheduler.h"   // EeScheduler::kMainThreadId

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    // Calls a handler the way the generated code does: arguments in a0/a1, result in v0.
    int32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0)
    {
        R5900Context ctx;
        ctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return static_cast<int32_t>(getRegU32(&ctx, 2));
    }

    constexpr uint32_t kStatusAddr = 0x00001000u;

    int32_t currentPriority(PS2Runtime &rt, int id)
    {
        uint8_t *rdram = rt.memory().getRDRAM();
        std::memset(rdram + kStatusAddr, 0, sizeof(ee_thread_status_t));
        const int32_t rc = call(rt, ps2_syscalls::ReferThreadStatus, static_cast<uint32_t>(id), kStatusAddr);
        if (rc != 0)
        {
            return rc;
        }
        return reinterpret_cast<const ee_thread_status_t *>(rdram + kStatusAddr)->current_priority;
    }

    std::string priorityOf(PS2Runtime &rt, int id)
    {
        return std::to_string(currentPriority(rt, id));
    }

    // The game's critical section: 0x314FF0 sets the calling thread to 0x18, 0x314320 raises it to 1
    // and keeps the answer, 0x3143a8 restores the priority with it.
    void scenarioCriticalSection()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("change-priority-test"), "initialize() with the host owning the window");
        const int mainId = EeScheduler::kMainThreadId;

        const int32_t start = call(rt, ps2_syscalls::ChangeThreadPriority, mainId, 24);
        expect(start == 0, "the main thread is created at priority 0, so the call answers 0, got " +
                               std::to_string(start));
        expect(currentPriority(rt, mainId) == 24, "the thread is at 24 after the call, got " +
                                                      priorityOf(rt, mainId));

        // The enter of the critical section: raise to 1, keep the answer as the value to restore.
        const int32_t enter = call(rt, ps2_syscalls::ChangeThreadPriority, mainId, 1);
        expect(enter == 24, "the raise answers the priority the thread had (24), got " +
                                std::to_string(enter));
        expect(currentPriority(rt, mainId) == 1, "the raise took effect, got " + priorityOf(rt, mainId));

        // The exit: restore with the value the enter answered.
        const int32_t leave = call(rt, ps2_syscalls::ChangeThreadPriority, mainId, static_cast<uint32_t>(enter));
        expect(leave == 1, "the restore answers the priority the thread had (1), got " +
                               std::to_string(leave));
        expect(currentPriority(rt, mainId) == 24, "the thread ends at the priority it had (24), got " +
                                                      priorityOf(rt, mainId));

        // An illegal priority is still the error code, not the previous priority.
        const int32_t illegal = call(rt, ps2_syscalls::ChangeThreadPriority, mainId, 0);
        expect(illegal == -403, "priority 0 is rejected with KE_ILLEGAL_PRIORITY (-403), got " +
                                    std::to_string(illegal));
        expect(currentPriority(rt, mainId) == 24, "the rejected call changed nothing, got " +
                                                      priorityOf(rt, mainId));

        // A thread id that does not exist is still the error code.
        const int32_t unknown = call(rt, ps2_syscalls::ChangeThreadPriority, 200, 5);
        expect(unknown == -407, "an unknown thread id is rejected with -407, got " +
                                    std::to_string(unknown));
    }

    // The interrupt-safe variant answers the same way (the game's SDK wrapper uses both).
    void scenarioInterruptSafe()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("change-priority-test"), "scenario 2: initialize()");
        const int mainId = EeScheduler::kMainThreadId;

        const int32_t first = call(rt, ps2_syscalls::iChangeThreadPriority, mainId, 8);
        expect(first == 0, "iChangeThreadPriority answers the initial priority (0), got " +
                               std::to_string(first));
        const int32_t second = call(rt, ps2_syscalls::iChangeThreadPriority, mainId, 16);
        expect(second == 8, "iChangeThreadPriority answers the previous priority (8), got " +
                                std::to_string(second));
        expect(currentPriority(rt, mainId) == 16, "iChangeThreadPriority changed the priority, got " +
                                                      priorityOf(rt, mainId));
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    scenarioCriticalSection();
    scenarioInterruptSafe();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "change_priority_return_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("change_priority_return_test: all checks passed\n");
    return 0;
}
