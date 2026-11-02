// SPDX-License-Identifier: GPL-3.0-or-later
// T-108: the runtime's host GS hook (ps2_host_gs.h), tested with a fake host that records every call.
// No window, no PS2Runtime::initialize(), no game. Covers the GIF arbiter forwarding, the SIGNAL/FINISH/LABEL
// scanner against the runtime's own GS, GS::writeRegister and the FIFO read redirects, CSR.RESET, and the
// GIF DMA channel and VIF1 DIRECT paths through PS2Memory.

#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_types.h"
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

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        }
        return condition;
    }

    struct Call
    {
        enum Kind { Transfer, Vsync, Reset, Read } kind;
        int path = 0;
        std::vector<uint8_t> data;
    };

    struct FakeHost
    {
        std::vector<Call> calls;
        std::vector<uint8_t> fifo;
        Ps2HostGs iface;

        FakeHost()
        {
            iface.user = this;
            iface.gif_transfer = [](void *u, int path, const uint8_t *d, uint32_t n) {
                auto *h = static_cast<FakeHost *>(u);
                h->calls.push_back({Call::Transfer, path, std::vector<uint8_t>(d, d + n)});
            };
            iface.reset = [](void *u) { static_cast<FakeHost *>(u)->calls.push_back({Call::Reset, 0, {}}); };
            iface.vsync = [](void *u, const uint64_t *, int f) {
                static_cast<FakeHost *>(u)->calls.push_back({Call::Vsync, f, {}});
            };
            iface.read_fifo = [](void *u, uint8_t *dst, uint32_t n) -> uint32_t {
                auto *h = static_cast<FakeHost *>(u);
                h->calls.push_back({Call::Read, 0, {}});
                const uint32_t k = n < h->fifo.size() ? n : static_cast<uint32_t>(h->fifo.size());
                std::memcpy(dst, h->fifo.data(), k);
                return k;
            };
        }
        size_t count(Call::Kind k) const
        {
            size_t c = 0;
            for (const auto &x : s_(calls))
                c += x.kind == k;
            return c;
        }
        static const std::vector<Call> &s_(const std::vector<Call> &v) { return v; }
    };

    void put64(std::vector<uint8_t> &v, uint64_t a, uint64_t b)
    {
        const size_t o = v.size();
        v.resize(o + 16);
        std::memcpy(&v[o], &a, 8);
        std::memcpy(&v[o + 8], &b, 8);
    }
    // PACKED A+D tag with nloop writes.
    void adTag(std::vector<uint8_t> &v, uint32_t nloop)
    {
        put64(v, (uint64_t)nloop | (1ull << 15) | (0ull << 58) | (1ull << 60), 0xEull);
    }
    void adWrite(std::vector<uint8_t> &v, uint8_t reg, uint64_t value) { put64(v, value, reg); }

    // A stream with SIGNAL, FINISH, LABEL, pseudo-registers and noise that must not be mistaken for them.
    std::vector<uint8_t> makeStream()
    {
        std::vector<uint8_t> s;
        adTag(s, 3);
        adWrite(s, GS_REG_SIGNAL, 0x00000000FFFF1234ull);
        adWrite(s, 0x4C, 0); // FRAME_1: not a status register
        adWrite(s, GS_REG_LABEL, 0x0000FFFF0000ABCDull);
        // PACKED with 2 registers (RGBAQ=1, XYZ2=5), looks like A+D data in its payload.
        put64(s, 2ull | (1ull << 15) | (1ull << 60), 0x51ull);
        put64(s, 0x61ull, 0x61ull);
        put64(s, 0x61ull, 0x61ull);
        // REGLIST, 1 loop, 2 regs (0xE,0xE)
        put64(s, 1ull | (1ull << 15) | (1ull << 58) | (2ull << 60), 0xEEull);
        put64(s, GS_REG_FINISH, 0);
        // IMAGE with 2 quadwords that look like an A+D write of FINISH.
        put64(s, 2ull | (1ull << 15) | (2ull << 58), 0);
        put64(s, 0, GS_REG_FINISH);
        put64(s, 0, GS_REG_FINISH);
        adTag(s, 3);
        adWrite(s, 0x59, 0x1111);
        adWrite(s, 0x5C, 0x2222);
        adWrite(s, 0x5F, 0x3333);
        adTag(s, 1);
        adWrite(s, GS_REG_FINISH, 0);
        return s;
    }

    void compareRegs(const GSRegisters &a, const GSRegisters &b, const std::string &what)
    {
        expect(a.csr.load() == b.csr.load(), what + ": csr");
        expect(a.siglblid == b.siglblid, what + ": siglblid");
        expect(a.dispfb1 == b.dispfb1 && a.dispfb2 == b.dispfb2 && a.display1 == b.display1 &&
                   a.display2 == b.display2 && a.pmode == b.pmode && a.smode2 == b.smode2 &&
                   a.extbuf == b.extbuf && a.extdata == b.extdata && a.extwrite == b.extwrite &&
                   a.bgcolor == b.bgcolor && a.imr == b.imr && a.busdir == b.busdir,
               what + ": privileged registers");
    }

    void testScannerMatchesInternalGs()
    {
        const std::vector<uint8_t> stream = makeStream();
        std::vector<uint8_t> vram(4u << 20);
        GSRegisters ref{};
        GS gs;
        gs.init(vram.data(), (uint32_t)vram.size(), &ref);
        gs.processGIFPacket(stream.data(), (uint32_t)stream.size());
        expect((ref.csr.load() & 3) == 3, "reference run sets SIGNAL and FINISH");

        for (uint32_t chunk : {16u, 32u, 48u, 80u, 160u, (uint32_t)stream.size()})
        {
            GSRegisters regs{};
            GifHostScanner sc;
            for (size_t off = 0; off < stream.size(); off += chunk)
            {
                const uint32_t n = (uint32_t)std::min<size_t>(chunk, stream.size() - off);
                sc.feed(GifPathId::Path3, stream.data() + off, n, regs);
            }
            compareRegs(regs, ref, "scanner chunk " + std::to_string(chunk));
        }

        // Interleaved paths: path 3 split in the middle of a tag's payload, path 1 in between.
        GSRegisters regs{};
        GifHostScanner sc;
        std::vector<uint8_t> p1;
        adTag(p1, 1);
        adWrite(p1, GS_REG_FINISH, 0);
        std::vector<uint8_t> p3;
        adTag(p3, 1);
        adWrite(p3, GS_REG_SIGNAL, 0xFFFFFFFF00000007ull);
        sc.feed(GifPathId::Path3, p3.data(), 16, regs); // tag only
        sc.feed(GifPathId::Path1, p1.data(), 32, regs); // whole FINISH packet
        expect((regs.csr.load() & 3) == 2, "interleaved: FINISH seen, SIGNAL not yet");
        sc.feed(GifPathId::Path3, p3.data() + 16, 16, regs);
        expect((regs.csr.load() & 3) == 3 && (regs.siglblid & 0xFFFFFFFF) == 7, "interleaved: SIGNAL after its tag");
    }

    void testArbiterForwarding()
    {
        FakeHost host;
        GSRegisters regs{};
        GifArbiter arb;
        std::vector<uint8_t> processed;
        arb.setProcessPacketFn([&](const uint8_t *d, uint32_t n) { processed.insert(processed.end(), d, d + n); });

        std::vector<uint8_t> a, b, c;
        adTag(a, 1);
        adWrite(a, 0x4C, 1);
        adTag(b, 1);
        adWrite(b, 0x4D, 2);
        adTag(c, 1);
        adWrite(c, GS_REG_FINISH, 0);

        // No host: the packets reach the processing function, nothing else changes.
        arb.submit(GifPathId::Path3, a.data(), (uint32_t)a.size());
        arb.drain();
        expect(processed == a, "without a host the process function gets the packet");

        arb.setHostGs(&host.iface, &regs);
        processed.clear();
        arb.submit(GifPathId::Path1, a.data(), (uint32_t)a.size());
        arb.submit(GifPathId::Path2, b.data(), (uint32_t)b.size());
        arb.submit(GifPathId::Path3, c.data(), (uint32_t)c.size());
        arb.drain();
        expect(processed.empty(), "with a host the process function gets nothing");
        expect(host.calls.size() == 3, "three packets forwarded");
        if (host.calls.size() == 3)
        {
            // Which order the arbiter delivers is its own rule; all must be present, byte for byte.
            size_t seen = 0;
            for (const auto &cl : host.calls)
                seen += (cl.data == a && cl.path == 1) + (cl.data == b && cl.path == 2) + (cl.data == c && cl.path == 3);
            expect(seen == 3, "paths and bytes forwarded unchanged");
        }
        expect((regs.csr.load() & 2) != 0, "FINISH tracked in host mode");

        // A struct with a null gif_transfer is "no host".
        Ps2HostGs none;
        arb.setHostGs(&none, &regs);
        expect(arb.hostGs() == nullptr, "null gif_transfer counts as no host");
        processed.clear();
        arb.submit(GifPathId::Path3, a.data(), (uint32_t)a.size());
        arb.drain();
        expect(processed == a, "no host: process function again");

        // Partial quadword dropped.
        arb.setHostGs(&host.iface, &regs);
        host.calls.clear();
        std::vector<uint8_t> odd = a;
        odd.resize(odd.size() + 5, 0);
        arb.submit(GifPathId::Path3, odd.data(), (uint32_t)odd.size());
        arb.drain();
        expect(host.calls.size() == 1 && host.calls[0].data.size() == a.size(), "size rounded down to a quadword");

        // notifyGsReset: queued packets first, then reset.
        host.calls.clear();
        arb.submit(GifPathId::Path3, a.data(), (uint32_t)a.size());
        arb.notifyGsReset();
        expect(host.calls.size() == 2 && host.calls[0].kind == Call::Transfer && host.calls[1].kind == Call::Reset,
               "reset after the queued packet");
    }

    void testGsRedirects()
    {
        FakeHost host;
        std::vector<uint8_t> vram(4u << 20);
        GSRegisters regs{};
        GS gs;
        gs.init(vram.data(), (uint32_t)vram.size(), &regs);
        GifArbiter arb;
        arb.setProcessPacketFn([&](const uint8_t *d, uint32_t n) { gs.processGIFPacket(d, n); });
        arb.setHostGs(&host.iface, &regs);
        gs.setHostGs(&host.iface, &arb);

        gs.writeRegister(GS_REG_FINISH, 0);
        expect(host.calls.size() == 1 && host.calls[0].path == 3 && host.calls[0].data.size() == 32,
               "writeRegister becomes a 32-byte path 3 packet");
        if (!host.calls.empty() && host.calls[0].data.size() == 32)
        {
            uint64_t tag, rd, v, r;
            std::memcpy(&tag, &host.calls[0].data[0], 8);
            std::memcpy(&rd, &host.calls[0].data[8], 8);
            std::memcpy(&v, &host.calls[0].data[16], 8);
            std::memcpy(&r, &host.calls[0].data[24], 8);
            expect((tag & 0x7FFF) == 1 && (tag >> 60) == 1 && rd == 0xE && r == GS_REG_FINISH && v == 0,
                   "A+D packet layout");
        }
        expect((regs.csr.load() & 2) != 0, "FINISH written through writeRegister reaches the CSR");

        host.fifo = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t buf[16] = {};
        const uint32_t n = gs.consumeLocalToHostBytes(buf, 16);
        expect(n == 8 && std::memcmp(buf, host.fifo.data(), 8) == 0, "read_fifo reached through consumeLocalToHostBytes");
    }

    void testMemoryPaths()
    {
        FakeHost host;
        PS2Memory mem;
        if (!expect(mem.initialize(), "PS2Memory::initialize"))
            return;
        GifArbiter arb;
        arb.setProcessPacketFn([](const uint8_t *, uint32_t) {});
        arb.setHostGs(&host.iface, &mem.gs());
        mem.setGifArbiter(&arb);

        // CSR.RESET through write32, write64 and with the bit clear.
        mem.write32(0x12001000u, 0x200u);
        expect(host.count(Call::Reset) == 1, "write32 CSR.RESET calls reset");
        mem.write64(0x12001000u, 0x200u);
        expect(host.count(Call::Reset) == 2, "write64 CSR.RESET calls reset");
        mem.write32(0x12001000u, 0x2u);
        expect(host.count(Call::Reset) == 2, "CSR write without the bit does not reset");

        // GIF DMA channel: an A+D packet in RDRAM.
        host.calls.clear();
        std::vector<uint8_t> pkt;
        adTag(pkt, 1);
        adWrite(pkt, 0x4C, 0x55);
        std::memcpy(mem.getRDRAM() + 0x1000, pkt.data(), pkt.size());
        mem.writeIORegister(0x1000A010u, 0x1000u);
        mem.writeIORegister(0x1000A020u, (uint32_t)(pkt.size() / 16));
        mem.writeIORegister(0x1000A000u, 0x101u);
        mem.processPendingTransfers();
        bool dma = false;
        for (const auto &c : host.calls)
            dma |= c.kind == Call::Transfer && c.path == 3 && c.data == pkt;
        expect(dma, "GIF DMA packet forwarded on path 3");

        // VIF1 DIRECT: vifcode 0x50 with 2 quadwords, then the data.
        host.calls.clear();
        // Quadword 0: three NOPs then DIRECT (the vifcode is the highest word), then the packet.
        // The VIF1 interpreter parses each call whole, so it goes through the VIF1 DMA channel.
        uint32_t vifWords[4] = {0, 0, 0, 0x50000000u | (uint32_t)(pkt.size() / 16)};
        std::memcpy(mem.getRDRAM() + 0x2000, vifWords, 16);
        std::memcpy(mem.getRDRAM() + 0x2010, pkt.data(), pkt.size());
        mem.writeIORegister(0x10009010u, 0x2000u);
        mem.writeIORegister(0x10009020u, (uint32_t)(1 + pkt.size() / 16));
        mem.writeIORegister(0x10009000u, 0x101u);
        mem.processPendingTransfers();
        bool vif = false;
        for (const auto &c : host.calls)
            vif |= c.kind == Call::Transfer && c.path == 2 && c.data == pkt;
        expect(vif, "VIF1 DIRECT packet forwarded on path 2");
    }
} // namespace

int main()
{
    testScannerMatchesInternalGs();
    testArbiterForwarding();
    testGsRedirects();
    testMemoryPaths();
    if (g_failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("host_gs_hook_test: all checks passed");
    return 0;
}
