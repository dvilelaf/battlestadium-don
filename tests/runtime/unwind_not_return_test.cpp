// SPDX-License-Identifier: GPL-3.0-or-later
// T-304 patch 0054: an unwinding is never taken for a return.
//
// When the scheduler's checkpoint answers "due" at a dispatch, PS2Runtime::dispatchGuestBranch returns false
// with the guest's resume address in ctx->pc and every generated caller above returns in turn, until the
// scheduler runs the resume address. One rule in dispatchGuestBranch ("a callee that comes back with pc equal
// to its own entry has returned without setting pc: continue after the call", meant for native stubs) also fired
// on such an unwinding when the resume address was the entry of a callee that was active further up the host
// stack (guest recursion): that level then went on with its own code on another frame's registers and stack,
// and its epilogue returned to a callee-saved register's contents (journal 029).
//
// The property checked: "a checkpoint due at ANY dispatch does not change what the guest computes". The guest
// is written the way generated code is (a switch on ctx->pc at entry, a frame in guest memory, ra set before
// each call, `if (!dispatchGuestBranch(...)) return;`), with the shapes of the game's frame-end path: mutual
// recursion behind a guard (A calls B, B calls A again, C calls B, A calls C), a callee-saved register that
// holds a code address and is spilled by every frame, a loop whose back edge asks eeCheckpointDue(), an
// indirect call, and a native function that returns without setting pc. It runs once with no forced
// checkpoint (the reference), then once for every k from 1 to the number of dispatches/back edges of that run,
// with a checkpoint forced due at exactly the k-th one (an ExternalWake event posted just before it). Every
// run must end in the reference's state.

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
#include <vector>

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

    constexpr uint32_t kMain = 0x00100000u;
    constexpr uint32_t kA = 0x00100100u;
    constexpr uint32_t kB = 0x00100200u;
    constexpr uint32_t kC = 0x00100300u;
    constexpr uint32_t kD = 0x00100500u;
    constexpr uint32_t kNative = 0x00100800u;
    constexpr uint32_t kWrap = 0x00100900u;
    constexpr uint32_t kWork = 0x00E00200u;
    constexpr uint32_t kGuard = 0x00E00000u;
    constexpr uint32_t kResults = 0x00E00100u;
    constexpr uint32_t kStackTop = 0x00F00000u;
    constexpr uint64_t kBudget = 4000; // dispatches; a run that needs more is a runaway

    enum Reg
    {
        V0 = 2,
        A0 = 4,
        A1 = 5,
        S0 = 16,
        S2 = 18,
        SP = 29,
        RA = 31
    };

    // The state of one run. Reset before each.
    struct Run
    {
        uint64_t sites = 0;     // dispatches + back-edge checks seen
        uint64_t forceAt = 0;   // 0: none
        std::vector<uint32_t> log;
        uint32_t counts[8] = {};
        uint32_t nativeCalls = 0;
        std::vector<uint32_t> results;
        bool overrun = false;
        std::atomic<bool> done{false};
    } *g_run = nullptr;

    uint32_t R(const R5900Context *ctx, int reg) { return getRegU32(ctx, reg); }
    void S(R5900Context *ctx, int reg, uint32_t value)
    {
        ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }
    uint32_t &W(uint8_t *rd, uint32_t addr) { return *reinterpret_cast<uint32_t *>(rd + addr); }

    void note(uint32_t event) { g_run->log.push_back(event); }

    // One checkpoint site: the k-th makes the next checkpoint answer "due".
    void site(PS2Runtime *rt)
    {
        // The k-th site and the one after it: a checkpoint due at the dispatch of the host replacement makes
        // the run loop call it, and the second one makes the generated-shaped code inside it unwind with pc
        // equal to the replacement's entry (patch 0055).
        const uint64_t site = ++g_run->sites;
        if (g_run->forceAt != 0 && (site == g_run->forceAt || site == g_run->forceAt + 1u))
        {
            rt->eeScheduler().postEvent(EeEvent{EeEventType::ExternalWake, 0u, 0u});
        }
    }

    // `jal`/`jalr` as the generated code does it. Returns false when the caller must return (an unwinding).
    bool call(PS2Runtime *rt, uint8_t *rd, R5900Context *ctx, uint32_t target, uint32_t src, uint32_t ret,
              PS2Runtime::GuestBranchKind kind)
    {
        S(ctx, RA, ret);
        ctx->pc = target;
        site(rt);
        if (!rt->dispatchGuestBranch(rd, ctx, target, src, ret, kind, "test"))
        {
            return false;
        }
        ctx->pc = ret;
        return true;
    }
#define JAL(target, src, ret) \
    if (!call(rt, rd, ctx, (target), (src), (ret), PS2Runtime::GuestBranchKind::DirectCall)) { return; }
#define JALR(target, src, ret) \
    if (!call(rt, rd, ctx, (target), (src), (ret), PS2Runtime::GuestBranchKind::IndirectCall)) { return; }

    // The prologue every guest function has: a 0x30-byte frame holding ra, the depth, s0 and s2.
    void prologue(uint8_t *rd, R5900Context *ctx, uint32_t depth)
    {
        const uint32_t sp = R(ctx, SP) - 0x30u;
        S(ctx, SP, sp);
        W(rd, sp + 0x00) = R(ctx, RA);
        W(rd, sp + 0x08) = depth;
        W(rd, sp + 0x10) = R(ctx, S0);
        W(rd, sp + 0x20) = R(ctx, S2);
        S(ctx, S0, R(ctx, A0));
    }

    // The epilogue: reload from the frame, pop, `jr ra`.
    void epilogue(uint8_t *rd, R5900Context *ctx)
    {
        const uint32_t sp = R(ctx, SP);
        const uint32_t ra = W(rd, sp + 0x00);
        S(ctx, S0, W(rd, sp + 0x10));
        S(ctx, S2, W(rd, sp + 0x20));
        S(ctx, SP, sp + 0x30u);
        S(ctx, RA, ra);
        ctx->pc = ra;
    }

    bool runaway(PS2Runtime *, R5900Context *ctx)
    {
        if (g_run->sites > kBudget)
        {
            g_run->overrun = true;
            ctx->pc = 0u;
            return true;
        }
        return false;
    }

    // A(n, d): behind a guard, call B; for odd n call C. Resume addresses: kA+0x10 (after B), kA+0x20 (after C).
    void funA(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        if (runaway(rt, ctx)) { return; }
        const uint32_t pc = ctx->pc;
        if (pc == kA)
        {
            prologue(rd, ctx, R(ctx, A1));
            note(0xA00000u + R(ctx, S0));
            ++g_run->counts[0];
            if (R(ctx, A1) >= 3u) { goto done; }
            if (W(rd, kGuard) == 0u)
            {
                W(rd, kGuard) = 1u;
                S(ctx, A0, R(ctx, S0));
                S(ctx, A1, R(ctx, A1) + 1u);
                JAL(kB, kA + 0x0c, kA + 0x10);
                goto after_b;
            }
            goto after_b_skipped;
        }
        if (pc == kA + 0x10) { goto after_b; }
        if (pc == kA + 0x20) { goto after_c; }
        return;
    after_b:
        W(rd, kGuard) = 0u;
    after_b_skipped:
        if ((R(ctx, S0) & 1u) != 0u)
        {
            S(ctx, A0, R(ctx, S0));
            S(ctx, A1, W(rd, R(ctx, SP) + 0x08) + 1u);
            JAL(kC, kA + 0x1c, kA + 0x20);
        }
    after_c:
    done:
        S(ctx, V0, R(ctx, S0) * 3u + 1u);
        epilogue(rd, ctx);
    }

    // B(n, d): call A(n + 100, d + 1). Resume address kB+0x10.
    void funB(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        if (runaway(rt, ctx)) { return; }
        if (ctx->pc == kB)
        {
            prologue(rd, ctx, R(ctx, A1));
            note(0xB00000u + R(ctx, S0));
            ++g_run->counts[1];
            S(ctx, A0, R(ctx, S0) + 100u);
            S(ctx, A1, R(ctx, A1) + 1u);
            JAL(kA, kB + 0x0c, kB + 0x10);
        }
        else if (ctx->pc != kB + 0x10)
        {
            return;
        }
        S(ctx, V0, R(ctx, V0) + R(ctx, S0));
        epilogue(rd, ctx);
    }

    // C(n, d): call B(n + 1, d + 1), then the code address in s2 (indirect), then the native function.
    // Resume addresses kC+0x10, kC+0x20, kC+0x30.
    void funC(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        if (runaway(rt, ctx)) { return; }
        const uint32_t pc = ctx->pc;
        if (pc == kC)
        {
            prologue(rd, ctx, R(ctx, A1));
            note(0xC00000u + R(ctx, S0));
            ++g_run->counts[2];
            S(ctx, A0, R(ctx, S0) + 1u);
            S(ctx, A1, R(ctx, A1) + 1u);
            JAL(kB, kC + 0x0c, kC + 0x10);
            goto after_b;
        }
        if (pc == kC + 0x10) { goto after_b; }
        if (pc == kC + 0x20) { goto after_d; }
        if (pc == kC + 0x30) { goto after_native; }
        return;
    after_b:
        S(ctx, A0, R(ctx, S0));
        JALR(R(ctx, S2), kC + 0x1c, kC + 0x20);
    after_d:
        S(ctx, A0, R(ctx, S0));
        JAL(kNative, kC + 0x2c, kC + 0x30);
    after_native:
        S(ctx, V0, R(ctx, V0) + 5u);
        epilogue(rd, ctx);
    }

    // D(n): a leaf with a frame; it clobbers s2 and restores it from the frame.
    void funD(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        if (runaway(rt, ctx)) { return; }
        prologue(rd, ctx, 0u);
        note(0xD00000u + R(ctx, S0));
        ++g_run->counts[3];
        S(ctx, S2, 0u);
        S(ctx, V0, R(ctx, S0) * 2u);
        epilogue(rd, ctx);
    }

    // The native function: returns without setting pc.
    void funNative(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_run->nativeCalls;
        if (g_run->nativeCalls > 1000u)
        {
            // Called again and again by the run loop: a checkpoint was due exactly at its dispatch and
            // nothing continued after it (patch 0055).
            g_run->overrun = true;
            ctx->pc = 0u;
            return;
        }
        note(0xE00000u + R(ctx, A0));
        S(ctx, V0, R(ctx, A0) + 7u);
    }

    // A host replacement (replaceFunction) that wraps generated-shaped code: the code is a loop whose back edge
    // goes to the replacement's own entry, and it asks eeCheckpointDue() there. When the checkpoint is due the
    // unwinding reaches the run loop with pc equal to the replacement's entry, which is NOT a return: the
    // replacement must be called again and finish the loop (patch 0055, the run loop's rule).
    void funWrap(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        for (;;)
        {
            uint32_t &n = W(rd, kWork);
            ++g_run->counts[4];
            note(0xF00000u + n);
            ++n;
            S(ctx, V0, n);
            if (n < 3u)
            {
                ctx->pc = kWrap;
                site(rt);
                if (rt->eeCheckpointDue())
                {
                    return;
                }
                continue;
            }
            n = 0u;
            ctx->pc = R(ctx, RA);
            return;
        }
    }

    // main: for i in 0..2 { r[i] = A(i, 0); C(i + 10, 1); native(i); }. Loop head kMain+0x10 (back edge asks
    // eeCheckpointDue). Resume addresses kMain+0x20, +0x30, +0x40.
    void funMain(uint8_t *rd, R5900Context *ctx, PS2Runtime *rt)
    {
        if (runaway(rt, ctx)) { return; }
        const uint32_t pc = ctx->pc;
        if (pc == kMain)
        {
            prologue(rd, ctx, 0u);
            S(ctx, S0, 0u); // i
            S(ctx, S2, kD);
            goto loop;
        }
        if (pc == kMain + 0x10) { goto loop; }
        if (pc == kMain + 0x20) { goto after_a; }
        if (pc == kMain + 0x30) { goto after_c; }
        if (pc == kMain + 0x40) { goto after_native; }
        if (pc == kMain + 0x50) { goto after_wrap; }
        return;
    loop:
        if (R(ctx, S0) >= 3u)
        {
            epilogue(rd, ctx);
            g_run->done = true;
            return;
        }
        S(ctx, A0, R(ctx, S0));
        S(ctx, A1, 0u);
        JAL(kA, kMain + 0x1c, kMain + 0x20);
    after_a:
        W(rd, kResults + 4u * R(ctx, S0)) = R(ctx, V0);
        S(ctx, A0, R(ctx, S0) + 10u);
        S(ctx, A1, 1u);
        JAL(kC, kMain + 0x2c, kMain + 0x30);
    after_c:
        W(rd, kResults + 0x20u + 4u * R(ctx, S0)) = R(ctx, V0);
        S(ctx, A0, R(ctx, S0));
        JAL(kNative, kMain + 0x3c, kMain + 0x40);
    after_native:
        W(rd, kResults + 0x40u + 4u * R(ctx, S0)) = R(ctx, V0);
        S(ctx, A0, R(ctx, S0));
        JAL(kWrap, kMain + 0x4c, kMain + 0x50);
    after_wrap:
        W(rd, kResults + 0x60u + 4u * R(ctx, S0)) = R(ctx, V0);
        S(ctx, S0, R(ctx, S0) + 1u);
        site(rt);
        ctx->pc = kMain + 0x10;
        if (rt->eeCheckpointDue())
        {
            return; // unwinding with the loop head as the resume address
        }
        goto loop;
    }

    // Each resume address is registered to the function that has a label for it, as the generated code does.
    struct Reg2
    {
        uint32_t addr;
        PS2Runtime::RecompiledFunction fn;
    };

    void readResults(uint8_t *rd)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            g_run->results.push_back(W(rd, kResults + 4u * i));
        }
    }

    struct Outcome
    {
        bool finished = false;
        bool overrun = false;
        std::vector<uint32_t> log;
        uint32_t counts[8] = {};
        uint32_t nativeCalls = 0;
        std::vector<uint32_t> results;
        uint64_t sites = 0;
    };

    // The results are read from the guest's memory by a function the main thread's end hands over; here, from
    // the runtime's RDRAM after the game thread stopped.
    Outcome runOnce(uint64_t forceAt)
    {
        Run run;
        run.forceAt = forceAt;
        g_run = &run;

        Ps2HostGs gs{};
        gs.user = &run;
        gs.thread_begin = [](void *) {};
        gs.thread_end = [](void *) {};
        gs.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
        gs.vsync = [](void *, const uint64_t *, int) {};

        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("unwind-not-return-test"), "initialize()");
        rt.setHostGs(&gs);
        const Reg2 table[] = {
            {kMain, funMain}, {kMain + 0x10, funMain}, {kMain + 0x20, funMain}, {kMain + 0x30, funMain},
            {kMain + 0x40, funMain}, {kMain + 0x50, funMain}, {kA, funA}, {kA + 0x10, funA}, {kA + 0x20, funA}, {kB, funB},
            {kB + 0x10, funB}, {kC, funC}, {kC + 0x10, funC}, {kC + 0x20, funC}, {kC + 0x30, funC},
            {kD, funD}, {kNative, funNative}, {kWrap, funWrap}};
        for (const Reg2 &r : table)
        {
            // The native function is a host replacement (replaceFunction, as src/game/don_overrides.cpp uses);
            // the others are shaped like generated code.
            if (r.addr == kNative || r.addr == kWrap)
            {
                rt.replaceFunction(r.addr, r.fn);
            }
            else
            {
                rt.registerFunction(r.addr, r.fn);
            }
        }
        R5900Context &cpu = rt.cpu();
        cpu.pc = kMain;
        S(&cpu, SP, kStackTop);
        S(&cpu, RA, 0u);
        expect(rt.startGameThread(), "startGameThread()");
        for (int i = 0; i < 400 && !run.done.load() && !run.overrun; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const bool finished = run.done.load();
        rt.stopGameThread();
        if (finished)
        {
            readResults(rt.memory().getRDRAM());
        }
        Outcome out;
        out.finished = finished;
        out.overrun = run.overrun;
        out.log = run.log;
        for (int i = 0; i < 8; ++i) { out.counts[i] = run.counts[i]; }
        out.nativeCalls = run.nativeCalls;
        out.results = run.results;
        out.sites = run.sites;
        g_run = nullptr;
        return out;
    }

    bool same(const Outcome &a, const Outcome &b)
    {
        if (a.finished != b.finished || a.log != b.log || a.nativeCalls != b.nativeCalls || a.results != b.results)
        {
            return false;
        }
        for (int i = 0; i < 8; ++i)
        {
            if (a.counts[i] != b.counts[i]) { return false; }
        }
        return true;
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    const Outcome reference = runOnce(0);
    expect(reference.finished && !reference.overrun, "the reference run finishes");
    std::printf("[unwind_not_return_test] reference: %llu checkpoint sites, A=%u B=%u C=%u D=%u native=%u, log %zu events\n",
                static_cast<unsigned long long>(reference.sites), reference.counts[0], reference.counts[1],
                reference.counts[2], reference.counts[3], reference.nativeCalls, reference.log.size());
    expect(reference.nativeCalls == reference.counts[2] + 3u, "the native function ran once per C and once per main iteration: " +
                                                                  std::to_string(reference.nativeCalls));

    unsigned differing = 0;
    std::string first;
    std::string ks;
    for (uint64_t k = 1; k <= reference.sites; ++k)
    {
        const Outcome out = runOnce(k);
        if (!same(out, reference))
        {
            ++differing;
            ks += " " + std::to_string(k);
            if (first.empty())
            {
                first = "k=" + std::to_string(k) + ": finished=" + std::to_string(out.finished) + " overrun=" +
                        std::to_string(out.overrun) + " A=" + std::to_string(out.counts[0]) + " B=" +
                        std::to_string(out.counts[1]) + " C=" + std::to_string(out.counts[2]) + " D=" +
                        std::to_string(out.counts[3]) + " native=" + std::to_string(out.nativeCalls) + " log " +
                        std::to_string(out.log.size()) + " events";
            }
        }
    }
    if (differing != 0u) { std::printf("[unwind_not_return_test] differing k:%s\n", ks.c_str()); }
    std::printf("[unwind_not_return_test] %llu forced checkpoints, %u runs differ from the reference%s%s\n",
                static_cast<unsigned long long>(reference.sites), differing, first.empty() ? "" : "; first ",
                first.c_str());
    expect(differing == 0u, "a checkpoint due at any dispatch leaves the guest's result unchanged (" +
                                std::to_string(differing) + " of " + std::to_string(reference.sites) + " differ; " +
                                first + ")");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "unwind_not_return_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("unwind_not_return_test: all checks passed\n");
    return 0;
}
