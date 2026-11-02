// SPDX-License-Identifier: GPL-3.0-or-later
// T-305 (journal 034), patch 0063: what ResumeThread and WakeupThread answer with.
//
// The EE kernel's ResumeThread and WakeupThread (and their iXxx variants) answer the thread's id on
// success, or a negative error code. The game depends on it: its CRI library wakes its server thread
// from the vertical-blank service with `if (ResumeThread(t) == t) WakeupThread(t)` (0x315940) and
// `if (WakeupThread(t) == t) flag = 0` (0x3159C8). The runtime answered KE_OK (0) on success, so the
// comparison never held, the server thread was never woken and the video player, which only runs from
// that thread, never decoded a picture. The console's memory during the opening movie has the thread's
// loop counter in the thousands (work/ref dump 07); the port had 0.
//
// This test creates a thread that never runs (priority below the caller's), then checks each answer:
// the id on success, and the error codes unchanged for an id of 0, an unknown id and a thread that is
// not suspended. No game, no generated code, no window; DISPLAY is unset by the test.

#include "host_test_support.h"

#include "Syscalls/Helpers/State.h" // ee_thread_t
#include "Syscalls/Thread.h"
#include "runtime/ee_scheduler.h"

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

    int32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0)
    {
        R5900Context ctx;
        ctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return static_cast<int32_t>(getRegU32(&ctx, 2));
    }

    constexpr uint32_t kParamAddr = 0x00102000u;
    constexpr uint32_t kStack = 0x00180000u;

    std::string str(int32_t v) { return std::to_string(v); }

    void neverRuns(uint8_t *, R5900Context *, PS2Runtime *) {}

    void scenario()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("thread-wake-test"), "initialize() with the host owning the window");

        // The calling thread is the main one at priority 1; the new thread is at 100, so it never runs.
        call(rt, ps2_syscalls::ChangeThreadPriority, EeScheduler::kMainThreadId, 1);
        uint8_t *rdram = rt.memory().getRDRAM();
        std::memset(rdram + kParamAddr, 0, sizeof(ee_thread_t));
        auto *param = reinterpret_cast<ee_thread_t *>(rdram + kParamAddr);
        param->func = 0x00100000u;
        param->stack = kStack;
        param->stack_size = 0x1000;
        param->initial_priority = 100;
        expect(rt.registerFunction(0x00100000u, neverRuns), "registerFunction(entry)");
        const int32_t id = call(rt, ps2_syscalls::CreateThread, kParamAddr);
        if (!expect(id > 1, "CreateThread answers the new id, got " + str(id)))
        {
            return;
        }
        const int32_t started = call(rt, ps2_syscalls::StartThread, static_cast<uint32_t>(id), 0);
        expect(started == 0, "StartThread succeeds, got " + str(started));

        // Wakeup of a thread that is not sleeping: counted, answers the id (0x3159C8 compares it).
        const int32_t w = call(rt, ps2_syscalls::WakeupThread, static_cast<uint32_t>(id));
        expect(w == id, "WakeupThread answers the thread id (" + str(id) + "), got " + str(w));
        const int32_t iw = call(rt, ps2_syscalls::iWakeupThread, static_cast<uint32_t>(id));
        expect(iw == id, "iWakeupThread answers the thread id (" + str(id) + "), got " + str(iw));

        // Resume of a thread that is not suspended is an error, not the id (0x315940 relies on that).
        const int32_t notSuspended = call(rt, ps2_syscalls::ResumeThread, static_cast<uint32_t>(id));
        expect(notSuspended == -415, "ResumeThread of a running thread is KE_NOT_SUSPEND (-415), got " +
                                         str(notSuspended));

        // Suspends do not nest: two suspends, one resume. The thread is ready again (a second resume says it is
        // not suspended) and the resume answers the id.
        call(rt, ps2_syscalls::SuspendThread, static_cast<uint32_t>(id));
        const int32_t s2 = call(rt, ps2_syscalls::SuspendThread, static_cast<uint32_t>(id));
        expect(s2 == 0, "a second SuspendThread of a suspended thread answers 0, got " + str(s2));
        const int32_t r1 = call(rt, ps2_syscalls::ResumeThread, static_cast<uint32_t>(id));
        expect(r1 == id, "ResumeThread after two suspends answers the id, got " + str(r1));
        const int32_t r2 = call(rt, ps2_syscalls::ResumeThread, static_cast<uint32_t>(id));
        expect(r2 == -415, "the thread is already resumed: the second ResumeThread is KE_NOT_SUSPEND (-415), got " +
                               str(r2));
        const int32_t st = [&] {
            const uint32_t addr = 0x00102400u;
            call(rt, ps2_syscalls::ReferThreadStatus, static_cast<uint32_t>(id), addr);
            return static_cast<int32_t>(*reinterpret_cast<uint32_t *>(rt.memory().getRDRAM() + addr));
        }();
        expect(st == 2, "after the resume the thread is READY (status 2), got " + str(st));

        // Errors are unchanged.
        expect(call(rt, ps2_syscalls::ResumeThread, 0) == -406, "ResumeThread(0) is KE_ILLEGAL_THID (-406)");
        expect(call(rt, ps2_syscalls::ResumeThread, 200) == -407, "ResumeThread(unknown) is -407");
        expect(call(rt, ps2_syscalls::WakeupThread, 0) == -406, "WakeupThread(0) is KE_ILLEGAL_THID (-406)");
        expect(call(rt, ps2_syscalls::WakeupThread, 200) == -407, "WakeupThread(unknown) is -407");
        expect(call(rt, ps2_syscalls::iWakeupThread, 200) == -407, "iWakeupThread(unknown) is -407");
        expect(call(rt, ps2_syscalls::WakeupThread, EeScheduler::kMainThreadId) == -406,
               "WakeupThread(the calling thread) is KE_ILLEGAL_THID (-406)");
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    scenario();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "thread_wake_return_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("thread_wake_return_test: all checks passed\n");
    return 0;
}
