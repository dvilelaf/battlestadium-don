// SPDX-License-Identifier: GPL-3.0-or-later
// T-110 part 1: a PS2Runtime whose window, audio device and loop belong to the host
// (setHostOwnsWindow, startGameThread, isGameThreadRunning, stopGameThread) and a host GS that counts
// thread_begin / vsync / thread_end. The "guest" is a host function registered at a guest address, no
// generated code, no ELF, no game. Run it with DISPLAY unset (the test unsets it itself).
//
// What it proves:
//   * with the host owning the window the runtime calls no raylib function at all (the raylib functions
//     are replaced by recorders, see host_test_support.h; a runtime that does not own the window is the
//     positive control that shows the recorders work);
//   * thread_begin, then the vsyncs, then thread_end, all on one thread that is not the caller's, which
//     is also the thread the guest function ran on;
//   * the vertical blanks come at about 60 per second;
//   * startGameThread() and stopGameThread() return, stopGameThread() is safe twice, and the process
//     exits normally.

#include "host_test_support.h"

#include "runtime/gs/ps2_host_gs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

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

    constexpr uint32_t kEntry = 0x00100000u;

    struct FakeGs
    {
        std::mutex mutex;
        std::vector<std::string> events; // "begin", "vsync", "end" in arrival order
        std::vector<std::thread::id> threads;
        std::vector<Clock::time_point> vsyncTimes;
        std::vector<int> fields;
        std::atomic<int> vsyncs{0};
        Ps2HostGs iface;

        FakeGs()
        {
            iface.user = this;
            iface.thread_begin = [](void *u) { static_cast<FakeGs *>(u)->note("begin", false); };
            iface.thread_end = [](void *u) { static_cast<FakeGs *>(u)->note("end", false); };
            iface.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
            iface.vsync = [](void *u, const uint64_t *, int field) {
                auto *self = static_cast<FakeGs *>(u);
                self->note("vsync", true);
                std::lock_guard<std::mutex> lock(self->mutex);
                self->fields.push_back(field);
            };
        }

        void note(const char *what, bool isVsync)
        {
            const auto now = Clock::now();
            std::lock_guard<std::mutex> lock(mutex);
            events.emplace_back(what);
            threads.push_back(std::this_thread::get_id());
            if (isVsync)
            {
                vsyncTimes.push_back(now);
                ++vsyncs;
            }
        }
    };

    // The guest: records the thread it runs on, then waits for the next vertical blank and resumes at its
    // own entry (eeWaitVSyncTicks does not return: it unwinds to the scheduler).
    std::atomic<int> g_guestCalls{0};
    std::atomic<int> g_guestFinishAfter{0}; // 0: loop until stopped, n: end the guest thread after n calls
    std::mutex g_guestMutex;
    std::vector<std::thread::id> g_guestThreads;

    void guestEntry(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_guestMutex);
            g_guestThreads.push_back(std::this_thread::get_id());
        }
        const int calls = ++g_guestCalls;
        const int finishAfter = g_guestFinishAfter.load();
        if (finishAfter != 0 && calls >= finishAfter)
        {
            ctx->pc = 0; // the guest's main thread ends
            return;
        }
        runtime->eeWaitVSyncTicks(1, kEntry);
    }

    void resetGuest(int finishAfter)
    {
        g_guestCalls = 0;
        g_guestFinishAfter = finishAfter;
        std::lock_guard<std::mutex> lock(g_guestMutex);
        g_guestThreads.clear();
    }

    double ms(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }

    std::string join(const std::vector<std::string> &v)
    {
        std::string out;
        for (const auto &s : v)
            out += (out.empty() ? "" : ",") + s;
        return out;
    }

    void checkOrderAndThread(const FakeGs &gs, const char *label)
    {
        const std::string tag = std::string(label) + ": ";
        expect(gs.events.size() >= 3, tag + "at least begin, one vsync and end");
        if (gs.events.size() < 3)
            return;
        expect(gs.events.front() == "begin", tag + "first callback is thread_begin, got " + gs.events.front());
        expect(gs.events.back() == "end", tag + "last callback is thread_end, got " + gs.events.back());
        size_t begins = 0, ends = 0, others = 0;
        for (const auto &e : gs.events)
        {
            if (e == "begin")
                ++begins;
            else if (e == "end")
                ++ends;
            else
                ++others;
        }
        expect(begins == 1 && ends == 1, tag + "one thread_begin and one thread_end");
        expect(others > 0, tag + "vsync callbacks arrived");
        bool oneThread = true;
        for (const auto &t : gs.threads)
            oneThread = oneThread && (t == gs.threads.front());
        expect(oneThread, tag + "all callbacks on one thread");
        expect(gs.threads.front() != std::this_thread::get_id(), tag + "callbacks not on the caller's thread");
    }

    std::vector<std::string> g_rlAfterHost;

    // Scenario 1: the host's loop. Guest loops until stopped.
    void scenarioHostLoop()
    {
        resetGuest(0);
        FakeGs gs;
        {
            PS2Runtime rt;
            rt.setHostOwnsWindow(true);
            expect(rt.hostOwnsWindow(), "hostOwnsWindow() reads back");
            expect(rt.initialize("host-loop-test"), "initialize() with the host owning the window");
            rt.setHostGs(&gs.iface);
            expect(rt.registerFunction(kEntry, guestEntry), "registerFunction at the entry address");
            rt.cpu().pc = kEntry;

            expect(!rt.isGameThreadRunning(), "not running before start");
            const auto t0 = Clock::now();
            expect(rt.startGameThread(), "startGameThread() returns true");
            const auto t1 = Clock::now();
            expect(ms(t0, t1) < 2000.0, "startGameThread() returns promptly (" + std::to_string(ms(t0, t1)) + " ms)");
            expect(!rt.startGameThread(), "a second startGameThread() while running returns false");
            expect(rt.isGameThreadRunning(), "running after start");

            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            expect(rt.isGameThreadRunning(), "still running after 2.2 s");
            expect(!rt.gameThreadFailed(), "no exception on the game thread");

            const auto t2 = Clock::now();
            rt.stopGameThread();
            const auto t3 = Clock::now();
            expect(ms(t2, t3) < 2000.0, "stopGameThread() returns promptly (" + std::to_string(ms(t2, t3)) + " ms)");
            expect(!rt.isGameThreadRunning(), "not running after stop");
            rt.stopGameThread(); // second call is harmless

            std::lock_guard<std::mutex> lock(gs.mutex);
            checkOrderAndThread(gs, "host loop");
            const size_t n = gs.vsyncTimes.size();
            expect(n >= 100, "about 130 vsyncs in 2.2 s, got " + std::to_string(n));
            if (n >= 20)
            {
                // Skip the first ones (start-up) and measure the steady rate.
                const size_t first = 5;
                const double seconds = ms(gs.vsyncTimes[first], gs.vsyncTimes[n - 1]) / 1000.0;
                const double rate = static_cast<double>(n - 1 - first) / seconds;
                std::printf("[host_loop_test] %zu vsyncs, steady rate %.2f Hz\n", n, rate);
                expect(rate > 57.0 && rate < 63.0, "vsync rate about 60 Hz, got " + std::to_string(rate));
                bool alternates = true;
                for (size_t i = 1; i < gs.fields.size(); ++i)
                    alternates = alternates && (gs.fields[i] != gs.fields[i - 1]);
                expect(alternates, "CSR.FIELD alternates at each vsync");
            }
            {
                std::lock_guard<std::mutex> g(g_guestMutex);
                bool sameThread = !g_guestThreads.empty();
                for (const auto &t : g_guestThreads)
                    sameThread = sameThread && (t == gs.threads.front());
                expect(sameThread, "the guest function ran on the thread of the GS callbacks");
                expect(g_guestCalls.load() > 100, "the guest function was resumed at each vsync, calls=" +
                                                     std::to_string(g_guestCalls.load()));
            }
        } // ~PS2Runtime: must not hang or touch raylib
        g_rlAfterHost = rt_test::raylibLog().take();
    }

    // Scenario 2: the guest's main thread ends by itself. The scheduler goes on (vblank keeps being
    // delivered), stop still works.
    void scenarioGuestEnds()
    {
        resetGuest(3);
        FakeGs gs;
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-loop-test-2"), "scenario 2: initialize()");
        rt.setHostGs(&gs.iface);
        rt.registerFunction(kEntry, guestEntry);
        rt.cpu().pc = kEntry;
        expect(rt.startGameThread(), "scenario 2: start");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        expect(g_guestCalls.load() == 3, "scenario 2: the guest ran exactly 3 times, got " + std::to_string(g_guestCalls.load()));
        expect(rt.isGameThreadRunning(), "scenario 2: the game thread (scheduler) outlives the guest's main thread");
        const int before = gs.vsyncs.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expect(gs.vsyncs.load() > before, "scenario 2: vsync keeps arriving after the guest ended");
        rt.stopGameThread();
        expect(!rt.isGameThreadRunning(), "scenario 2: stopped");
        std::lock_guard<std::mutex> lock(gs.mutex);
        checkOrderAndThread(gs, "guest ends");
    }

    // Scenario 3: stop asked immediately after start (the stop must not be lost).
    void scenarioImmediateStop()
    {
        resetGuest(0);
        FakeGs gs;
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-loop-test-3"), "scenario 3: initialize()");
        rt.setHostGs(&gs.iface);
        rt.registerFunction(kEntry, guestEntry);
        for (int i = 0; i < 20; ++i)
        {
            rt.cpu().pc = kEntry;
            expect(rt.startGameThread(), "scenario 3: start #" + std::to_string(i));
            rt.stopGameThread();
            expect(!rt.isGameThreadRunning(), "scenario 3: stopped #" + std::to_string(i));
        }
        std::lock_guard<std::mutex> lock(gs.mutex);
        size_t begins = 0, ends = 0;
        for (const auto &e : gs.events)
        {
            begins += (e == "begin");
            ends += (e == "end");
        }
        expect(begins == 20 && ends == 20, "scenario 3: 20 begin/end pairs, got " + std::to_string(begins) + "/" + std::to_string(ends));
    }

    // Scenario 4: the convenience run() of a host-owned runtime waits without drawing and returns on stop.
    void scenarioRun()
    {
        resetGuest(0);
        FakeGs gs;
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-loop-test-4"), "scenario 4: initialize()");
        rt.setHostGs(&gs.iface);
        rt.registerFunction(kEntry, guestEntry);
        rt.cpu().pc = kEntry;
        std::thread stopper([&rt]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            rt.requestStop();
        });
        rt.run();
        stopper.join();
        expect(!rt.isGameThreadRunning(), "scenario 4: run() returned with the game thread joined");
        std::lock_guard<std::mutex> lock(gs.mutex);
        checkOrderAndThread(gs, "run()");
    }
}

int main()
{
    // A window must never open, whatever the environment of the caller.
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    expect(std::getenv("DISPLAY") == nullptr, "DISPLAY is unset");

    rt_test::raylibLog().take();
    scenarioHostLoop();
    scenarioGuestEnds();
    scenarioImmediateStop();
    scenarioRun();
    const auto allHostCalls = rt_test::raylibLog().take();

#ifdef PS2X_NO_RAYLIB
    // A build without raylib has no window of its own: the runtime says so instead of failing silently.
    {
        PS2Runtime rt;
        expect(!rt.initialize("no-raylib"), "no raylib: initialize() without setHostOwnsWindow(true) fails");
    }
#endif
#ifdef DON_RT_RAYLIB_INTERPOSED
    expect(g_rlAfterHost.empty(), "host-owned runtime, scenario 1: no raylib call (" + join(g_rlAfterHost) + ")");
    expect(allHostCalls.empty(), "host-owned runtime, scenarios 2-4: no raylib call (" + join(allHostCalls) + ")");

    std::string controlCalls;
    // Positive control: the same runtime without the host flag does call them (the recorders do record).
    {
        PS2Runtime rt;
        expect(!rt.hostOwnsWindow(), "default: the runtime owns its window");
        expect(rt.initialize("control"), "control: initialize()");
        const auto calls = rt_test::raylibLog().take();
        controlCalls = join(calls);
        auto has = [&](const char *name) {
            for (const auto &c : calls)
                if (c == name)
                    return true;
            return false;
        };
        expect(has("SetConfigFlags") && has("InitWindow") && has("InitAudioDevice") && has("SetTargetFPS"),
               "control: a runtime that owns its window calls SetConfigFlags, InitWindow, InitAudioDevice, SetTargetFPS (got " +
                   join(calls) + ")");
    }
    std::printf("[host_loop_test] raylib calls by the host-owned runtime: %zu (control recorded %s)\n",
                g_rlAfterHost.size() + allHostCalls.size(), controlCalls.c_str());
#else
    std::printf("[host_loop_test] built without raylib interposers (raylib absent or not Linux): window checks skipped\n");
    (void)g_rlAfterHost;
    (void)allHostCalls;
#endif

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("host_loop_test: all checks passed\n");
    return 0;
}
