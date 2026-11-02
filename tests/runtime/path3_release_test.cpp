// SPDX-License-Identifier: GPL-3.0-or-later
// T-320: the third path held by MSKPATH3 leaves one end-of-packet-terminated GIF packet per lifting of the
// mask, in order, between the VIF1 transfers that follow (what the console does with the game's stream).
// A host GS records the transfers with their path. No window, no game.

#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/ps2_host_gs.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        }
    }

    struct Host
    {
        std::vector<std::string> seen; // "p3:<id>" or "p2:<id>", id = the A+D value of the last tag
        Ps2HostGs iface;

        Host()
        {
            iface.user = this;
            iface.gif_transfer = [](void *u, int path, const uint8_t *d, uint32_t n) {
                uint64_t v = 0;
                std::memcpy(&v, d + n - 16u, 8); // the last quadword: A+D data
                static_cast<Host *>(u)->seen.push_back("p" + std::to_string(path) + ":" + std::to_string(v));
            };
            iface.reset = [](void *) {};
            iface.vsync = [](void *, const uint64_t *, int) {};
            iface.read_fifo = [](void *, uint8_t *, uint32_t) -> uint32_t { return 0; };
        }
    };

    // One A+D tag (NLOOP=1, PACKED, NREG=1, REGS=0xE) and its register write 0x4C = id.
    void adTag(std::vector<uint8_t> &out, bool eop, uint64_t id)
    {
        const uint64_t lo = 1ull | (eop ? 0x8000ull : 0ull) | (0ull << 58) | (1ull << 60);
        const uint64_t hi = 0xEull;
        const uint64_t w[4] = {lo, hi, id, 0x4Cull};
        const auto *b = reinterpret_cast<const uint8_t *>(w);
        out.insert(out.end(), b, b + 16);
        out.insert(out.end(), b + 16, b + 32);
    }

    std::vector<uint8_t> packet(uint64_t id) { std::vector<uint8_t> p; adTag(p, true, id); return p; }

    // Packet 2 of the test: two tags, the EOP only on the second.
    std::vector<uint8_t> twoTagPacket(uint64_t id)
    {
        std::vector<uint8_t> p;
        adTag(p, false, id - 1000);
        adTag(p, true, id);
        return p;
    }

    constexpr uint32_t kNop = 0u;
    uint32_t mskpath3(bool masked) { return (0x06u << 24) | (masked ? 0x8000u : 0u); }
    constexpr uint32_t kFlusha = 0x13u << 24;
    uint32_t direct(uint32_t qw) { return (0x50u << 24) | qw; }

    struct Rig
    {
        Host host;
        PS2Memory mem;
        GifArbiter arb;
        bool ok = false;

        Rig()
        {
            ok = mem.initialize();
            arb.setProcessPacketFn([](const uint8_t *, uint32_t) {});
            arb.setHostGs(&host.iface, &mem.gs());
            mem.setGifArbiter(&arb);
        }

        void vif1(const std::vector<uint32_t> &words)
        {
            std::memcpy(mem.getRDRAM() + 0x4000, words.data(), words.size() * 4);
            mem.writeIORegister(0x10009010u, 0x4000u);
            mem.writeIORegister(0x10009020u, static_cast<uint32_t>(words.size() / 4));
            mem.writeIORegister(0x10009000u, 0x101u);
            mem.processPendingTransfers();
        }

        void gif(const std::vector<uint8_t> &bytes)
        {
            std::memcpy(mem.getRDRAM() + 0x8000, bytes.data(), bytes.size());
            mem.writeIORegister(0x1000A010u, 0x8000u);
            mem.writeIORegister(0x1000A020u, static_cast<uint32_t>(bytes.size() / 16));
            mem.writeIORegister(0x1000A000u, 0x101u);
            mem.processPendingTransfers();
        }
    };

    // The stream of the game: lift, mask, FLUSHA, DIRECT <one packet>; three times (one transfer).
    std::vector<uint32_t> gameStream(const uint64_t ids[3], bool endMasked)
    {
        std::vector<uint32_t> w;
        for (int i = 0; i < 3; ++i)
        {
            w.insert(w.end(), {mskpath3(false), mskpath3(true), kFlusha, direct(2)});
            const auto p = packet(ids[i]);
            const auto *q = reinterpret_cast<const uint32_t *>(p.data());
            w.insert(w.end(), q, q + 8);
        }
        if (!endMasked)
            w.insert(w.end(), {kNop, kNop, kNop, mskpath3(false)});
        return w;
    }

    std::string join(const std::vector<std::string> &v)
    {
        std::string s;
        for (const auto &x : v)
            s += (s.empty() ? "" : " ") + x;
        return s;
    }

    void testGameShape()
    {
        Rig r;
        if (!r.ok) { expect(false, "initialize"); return; }
        r.vif1({kNop, kNop, kNop, mskpath3(true)});
        std::vector<uint8_t> three = packet(1);
        const auto two = twoTagPacket(2);
        const auto p3 = packet(3);
        three.insert(three.end(), two.begin(), two.end());
        three.insert(three.end(), p3.begin(), p3.end());
        r.gif(three);
        expect(r.host.seen.empty(), "held while the mask is on: " + join(r.host.seen));
        const uint64_t ids[3] = {11, 12, 13};
        r.vif1(gameStream(ids, true));
        const std::string got = join(r.host.seen);
        expect(got == "p3:1 p2:11 p3:2 p2:12 p3:3 p2:13", "order: " + got);
    }

    void testDrainAtEnd()
    {
        Rig r;
        if (!r.ok) { expect(false, "initialize"); return; }
        r.vif1({kNop, kNop, kNop, mskpath3(true)});
        auto b = packet(1);
        const auto two = twoTagPacket(2);
        b.insert(b.end(), two.begin(), two.end());
        r.gif(b);
        r.vif1({kNop, kNop, kNop, mskpath3(false)});
        expect(join(r.host.seen) == "p3:1 p3:2", "mask left off drains the rest: " + join(r.host.seen));
    }

    void testStaysQueuedMasked()
    {
        Rig r;
        if (!r.ok) { expect(false, "initialize"); return; }
        r.vif1({kNop, kNop, kNop, mskpath3(true)});
        auto b = packet(1);
        const auto two = twoTagPacket(2);
        b.insert(b.end(), two.begin(), two.end());
        r.gif(b);
        const uint64_t ids[3] = {11, 12, 13};
        r.vif1(gameStream(ids, true));
        // Three liftings release two packets; the third lifting finds the queue empty.
        expect(join(r.host.seen) == "p3:1 p2:11 p3:2 p2:12 p2:13", "short queue: " + join(r.host.seen));
    }

    void testMaskOffPassesThrough()
    {
        Rig r;
        if (!r.ok) { expect(false, "initialize"); return; }
        r.gif(packet(7));
        expect(join(r.host.seen) == "p3:7", "mask off: straight through: " + join(r.host.seen));
    }
} // namespace

int main()
{
    testGameShape();
    testDrainAtEnd();
    testStaysQueuedMasked();
    testMaskOffPassesThrough();
    if (g_failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("path3_release_test: ok");
    return 0;
}
