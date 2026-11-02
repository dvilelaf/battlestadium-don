// SPDX-License-Identifier: GPL-3.0-or-later
// T-205 part 1: the runtime's host VU1 hook (ps2_host_vu1.h), tested with a fake host VU1 that records every
// call. No window, no PS2Runtime::initialize(), no game, no microVU. A VIF1 stream built by hand (STCYCL, ITOP,
// two UNPACK formats, MPG, MSCAL, BASE/OFFSET, a second UNPACK with TOPS, MSCAL, MSCNT, FLUSH) is delivered
// through the VIF1 DMA channel twice: with no host VU1 (the runtime's own path, recording what it would have
// started) and with the fake installed; the unpacked data, the microcode, the start addresses and the TOP/ITOP
// values must agree. Also: ordering of the calls, XGKICK packets reaching the GIF arbiter on path 1 (host GS and
// the runtime's own GS), continuation of a program stopped by its budget, the memory-mapped windows, the VIF1
// tap, and the fallbacks (no host, data() returning null, thread_end).

#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/ps2_host_gs.h"
#include "runtime/ps2_host_vu1.h"
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

    constexpr uint32_t kData = PS2_VU1_DATA_SIZE;
    constexpr uint32_t kCode = PS2_VU1_CODE_SIZE;

    // ---- The stream ----------------------------------------------------------------------------------

    uint32_t cmd(uint32_t op, uint32_t num, uint32_t imm) { return (op << 24) | (num << 16) | imm; }

    const uint32_t kMicro[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u}; // two instruction words
    const uint32_t kVec1[12] = {0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u, 0xA0u, 0xB0u,
                                0xC0u, 0xD0u, 0xE0u, 0xF0u, 0x100u, 0x110u};
    const uint32_t kV8[2] = {0x80FF7F01u, 0x04030201u}; // V4-8, sign extended: 2 vectors
    const uint32_t kVec2[8] = {0x1111u, 0x2222u, 0x3333u, 0x4444u, 0x5555u, 0x6666u, 0x7777u, 0x8888u};

    std::vector<uint32_t> buildStream()
    {
        std::vector<uint32_t> w;
        auto put = [&](uint32_t v) { w.push_back(v); };
        put(cmd(0x01, 0, 0x0404));      // STCYCL CL=4 WL=4
        put(cmd(0x04, 0, 0x0055));      // ITOP 0x55
        put(cmd(0x6C, 3, 0x0010));      // UNPACK V4-32, 3 vectors at vector 0x10
        for (uint32_t v : kVec1) put(v);
        put(cmd(0x6E, 2, 0x0020));      // UNPACK V4-8, 2 vectors at vector 0x20
        for (uint32_t v : kV8) put(v);
        put(cmd(0x4A, 2, 0x0010));      // MPG, 2 instructions at byte 0x80
        for (uint32_t v : kMicro) put(v);
        put(cmd(0x14, 0, 0x0010));      // MSCAL at byte 0x80
        put(cmd(0x03, 0, 0x0100));      // BASE 0x100
        put(cmd(0x02, 0, 0x0100));      // OFFSET 0x100 (TOPS = BASE)
        put(cmd(0x6C, 2, 0x8004));      // UNPACK V4-32, 2 vectors at TOPS + 4
        for (uint32_t v : kVec2) put(v);
        put(cmd(0x14, 0, 0x0020));      // MSCAL at byte 0x100
        put(cmd(0x17, 0, 0));           // MSCNT
        put(cmd(0x11, 0, 0));           // FLUSH
        while (w.size() % 4)
            put(0);
        return w;
    }

    // Delivered the way the game does: a normal-mode VIF1 DMA transfer from RDRAM.
    void deliver(PS2Memory &mem, const std::vector<uint32_t> &words, uint32_t at = 0x4000u)
    {
        std::memcpy(mem.getRDRAM() + at, words.data(), words.size() * 4);
        mem.writeIORegister(0x10009010u, at);
        mem.writeIORegister(0x10009020u, static_cast<uint32_t>(words.size() / 4));
        mem.writeIORegister(0x10009000u, 0x101u);
        mem.processPendingTransfers();
    }

    // One A+D GIF packet (tag NLOOP=1 EOP=1 PACKED NREG=1 REGS=A+D, then register 0x4C = value).
    std::vector<uint8_t> adPacket(uint64_t value)
    {
        const uint64_t q[4] = {1ull | (1ull << 15) | (1ull << 60), 0x0Eull, value, 0x4Cull};
        std::vector<uint8_t> p(sizeof(q));
        std::memcpy(p.data(), q, sizeof(q));
        return p;
    }

    // ---- The fake host VU1 -----------------------------------------------------------------------------

    struct Call
    {
        enum Kind { ThreadBegin, ThreadEnd, Data, WriteMicro, SetTop, Execute, Continue, Running, Reset } kind;
        uint32_t a = 0;
        uint32_t b = 0;
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> dataSnapshot; // Execute only: the data memory at that moment
    };

    struct FakeVu1
    {
        alignas(64) uint8_t mem[kData] = {};
        uint8_t micro[kCode] = {};
        std::vector<Call> calls;
        Ps2HostVu1 iface;
        Ps2HostVu1XgkickFn sink = nullptr;
        void *sinkUser = nullptr;
        bool returnNullData = false;

        // Behaviour: packets produced by the next executes / continues, and how long a program keeps running.
        std::vector<std::vector<uint8_t>> executePackets; // executePackets[k]: sent by the k-th execute
        std::vector<std::vector<uint8_t>> continuePackets;
        std::vector<int> runFor; // runFor[k]: the k-th execute leaves the program running for that many continues
        bool alwaysRunning = false;
        int left = 0;
        uint32_t executeCount = 0;
        uint32_t continueCount = 0;

        FakeVu1()
        {
            iface.user = this;
            iface.thread_begin = [](void *u, Ps2HostVu1XgkickFn s, void *su) {
                auto *f = static_cast<FakeVu1 *>(u);
                f->sink = s;
                f->sinkUser = su;
                f->calls.push_back({Call::ThreadBegin});
            };
            iface.thread_end = [](void *u) { static_cast<FakeVu1 *>(u)->calls.push_back({Call::ThreadEnd}); };
            iface.data = [](void *u) -> uint8_t * {
                auto *f = static_cast<FakeVu1 *>(u);
                f->calls.push_back({Call::Data});
                return f->returnNullData ? nullptr : f->mem;
            };
            iface.write_micro = [](void *u, uint32_t addr, const uint8_t *d, uint32_t n) {
                auto *f = static_cast<FakeVu1 *>(u);
                f->calls.push_back({Call::WriteMicro, addr, n, std::vector<uint8_t>(d, d + n)});
                if (addr + n <= kCode)
                    std::memcpy(f->micro + addr, d, n);
            };
            iface.set_top = [](void *u, uint32_t top, uint32_t itop) {
                static_cast<FakeVu1 *>(u)->calls.push_back({Call::SetTop, top, itop});
            };
            iface.execute = [](void *u, uint32_t pc, uint32_t cycles) -> uint32_t {
                auto *f = static_cast<FakeVu1 *>(u);
                Call c{Call::Execute, pc, cycles};
                c.dataSnapshot.assign(f->mem, f->mem + kData);
                f->calls.push_back(std::move(c));
                const uint32_t k = f->executeCount++;
                f->left = f->alwaysRunning ? 1 : (k < f->runFor.size() ? f->runFor[k] : 0);
                if (k < f->executePackets.size() && !f->executePackets[k].empty() && f->sink)
                    f->sink(f->sinkUser, f->executePackets[k].data(), static_cast<uint32_t>(f->executePackets[k].size()));
                return 1000u + k; // an engine-specific figure: nothing may depend on it
            };
            iface.continue_ = [](void *u, uint32_t cycles) -> uint32_t {
                auto *f = static_cast<FakeVu1 *>(u);
                f->calls.push_back({Call::Continue, cycles});
                const uint32_t k = f->continueCount++;
                if (k < f->continuePackets.size() && !f->continuePackets[k].empty() && f->sink)
                    f->sink(f->sinkUser, f->continuePackets[k].data(), static_cast<uint32_t>(f->continuePackets[k].size()));
                if (!f->alwaysRunning && f->left > 0)
                    --f->left;
                return 7u;
            };
            iface.running = [](void *u) -> int {
                auto *f = static_cast<FakeVu1 *>(u);
                f->calls.push_back({Call::Running});
                return f->left > 0 ? 1 : 0;
            };
            iface.reset = [](void *u) { static_cast<FakeVu1 *>(u)->calls.push_back({Call::Reset}); };
        }

        std::vector<const Call *> of(Call::Kind kind) const
        {
            std::vector<const Call *> out;
            for (const Call &c : calls)
                if (c.kind == kind)
                    out.push_back(&c);
            return out;
        }
        // The calls that matter for ordering, as letters: B begin, D data, R reset, W write_micro, T set_top,
        // X execute, C continue, E end (running() polls and the like are left out).
        std::string order() const
        {
            std::string s;
            for (const Call &c : calls)
            {
                switch (c.kind)
                {
                case Call::ThreadBegin: s += 'B'; break;
                case Call::Data: s += 'D'; break;
                case Call::Reset: s += 'R'; break;
                case Call::WriteMicro: s += 'W'; break;
                case Call::SetTop: s += 'T'; break;
                case Call::Execute: s += 'X'; break;
                case Call::Continue: s += 'C'; break;
                case Call::ThreadEnd: s += 'E'; break;
                default: break;
                }
            }
            return s;
        }
    };

    // ---- A fake host GS (patch 0005) -------------------------------------------------------------------

    struct FakeGs
    {
        struct Transfer
        {
            int path;
            std::vector<uint8_t> data;
        };
        std::vector<Transfer> transfers;
        Ps2HostGs iface;
        FakeGs()
        {
            iface.user = this;
            iface.gif_transfer = [](void *u, int path, const uint8_t *d, uint32_t n) {
                static_cast<FakeGs *>(u)->transfers.push_back({path, std::vector<uint8_t>(d, d + n)});
            };
        }
        int countOnPath(int path, const std::vector<uint8_t> &bytes) const
        {
            int n = 0;
            for (const Transfer &t : transfers)
                n += (t.path == path && t.data == bytes) ? 1 : 0;
            return n;
        }
    };

    // What the runtime's own path was asked to start.
    struct Starts
    {
        struct S
        {
            bool cont;
            uint32_t pc, top, itop;
        };
        std::vector<S> list;
    };

    void hookInterpreterCallbacks(PS2Memory &mem, Starts &starts)
    {
        mem.setVu1MscalCallback([&starts](uint32_t pc, uint32_t top, uint32_t itop) { starts.list.push_back({false, pc, top, itop}); });
        mem.setVu1MscntCallback([&starts](uint32_t top, uint32_t itop) { starts.list.push_back({true, 0, top, itop}); });
    }

    // ---- Tests -----------------------------------------------------------------------------------------

    void testStreamBothWays()
    {
        const std::vector<uint32_t> stream = buildStream();

        // The runtime's own path: no host VU1.
        PS2Memory ref;
        if (!expect(ref.initialize(), "PS2Memory::initialize (reference)"))
            return;
        Starts refStarts;
        hookInterpreterCallbacks(ref, refStarts);
        std::vector<std::vector<uint8_t>> tapChunks;
        ref.setVif1Tap(
            [](void *u, const uint8_t *d, uint32_t n) {
                static_cast<std::vector<std::vector<uint8_t>> *>(u)->emplace_back(d, d + n);
            },
            &tapChunks);
        deliver(ref, stream);
        expect(!ref.hostVu1Bound() && ref.hostVu1() == nullptr, "no host VU1 installed on the reference");
        expect(refStarts.list.size() == 3, "reference: the runtime's own callbacks received 2 MSCAL and 1 MSCNT");
        expect(!tapChunks.empty(), "the VIF1 tap saw the DMA chunk");
        // The values to check against below come from the reference, plus fixed ones for the unpack itself.
        expect(refStarts.list.size() == 3 && !refStarts.list[0].cont && refStarts.list[0].pc == 0x80 &&
                   refStarts.list[0].itop == 0x55 && refStarts.list[1].pc == 0x100 && refStarts.list[2].cont,
               "reference: start addresses and ITOP as the stream says");

        // The same stream with the fake installed, a fake GS behind the arbiter.
        FakeVu1 fake;
        FakeGs gs;
        const std::vector<uint8_t> pkt1 = adPacket(0x1111), pkt2 = adPacket(0x2222);
        fake.executePackets = {pkt1, {}, pkt2};
        PS2Memory mem;
        if (!expect(mem.initialize(), "PS2Memory::initialize (host)"))
            return;
        GifArbiter arb;
        arb.setProcessPacketFn([](const uint8_t *, uint32_t) {});
        arb.setHostGs(&gs.iface, &mem.gs());
        mem.setGifArbiter(&arb);
        Starts hostStarts;
        hookInterpreterCallbacks(mem, hostStarts); // must NOT be called: the host takes over
        mem.setHostVu1(&fake.iface);
        expect(mem.hostVu1() != nullptr && !mem.hostVu1Bound(), "installed, not bound before the game thread starts");
        mem.beginHostVu1();
        expect(mem.hostVu1Bound(), "bound after beginHostVu1");
        expect(fake.order() == "BDR", "begin: thread_begin, data, reset, in that order (got " + fake.order() + ")");
        expect(fake.sink == &ps2HostVu1Xgkick && fake.sinkUser == &mem, "thread_begin receives the runtime's XGKICK sink");
        expect(mem.getVU1Data() == fake.mem, "the runtime's VU1 data memory is the host's");
        fake.calls.clear();

        deliver(mem, stream);

        expect(hostStarts.list.empty(), "the runtime's own interpreter callbacks were not called");
        expect(fake.order() == "WTXTXTX", "call order: write_micro, then (set_top, execute) x 3 (got " + fake.order() + ")");

        // Data memory: byte for byte what the runtime's own VU1 memory holds for the same stream.
        expect(std::memcmp(fake.mem, ref.getVU1Data(), kData) == 0, "unpacked data equals the runtime's own VU1 data memory");
        // And that is not trivially zero: the first vectors landed where the UNPACK addresses say.
        expect(std::memcmp(fake.mem + 0x10 * 16, kVec1, sizeof(kVec1)) == 0, "V4-32 UNPACK at vector 0x10");
        const uint32_t v8[8] = {0x01u, 0x7Fu, 0xFFFFFFFFu, 0xFFFFFF80u, 0x01u, 0x02u, 0x03u, 0x04u};
        expect(std::memcmp(fake.mem + 0x20 * 16, v8, sizeof(v8)) == 0, "V4-8 UNPACK (sign extended) at vector 0x20");
        expect(std::memcmp(fake.mem + 0x104 * 16, kVec2, sizeof(kVec2)) == 0, "UNPACK with the TOPS flag at TOPS + 4");

        // Microcode.
        const auto writes = fake.of(Call::WriteMicro);
        expect(writes.size() == 1 && writes[0]->a == 0x80 && writes[0]->b == 16 &&
                   std::memcmp(writes[0]->bytes.data(), kMicro, 16) == 0,
               "write_micro: address 0x80, 16 bytes of the MPG data");
        expect(std::memcmp(fake.micro + 0x80, kMicro, 16) == 0, "the fake's code memory holds the microcode");
        expect(std::memcmp(mem.getVU1Code(), ref.getVU1Code(), kCode) == 0, "the runtime's code mirror equals the reference's");

        // Starts and TOP/ITOP, against the reference's.
        const auto execs = fake.of(Call::Execute);
        const auto tops = fake.of(Call::SetTop);
        bool starts = execs.size() == 3 && tops.size() == 3 && refStarts.list.size() == 3;
        for (size_t i = 0; starts && i < 3; ++i)
        {
            const auto &r = refStarts.list[i];
            starts = tops[i]->a == r.top && tops[i]->b == r.itop &&
                     execs[i]->a == (r.cont ? PS2_HOST_VU1_CONTINUE_PC : r.pc) && execs[i]->b == PS2_HOST_VU1_SLICE_CYCLES;
        }
        expect(starts, "execute / set_top arguments equal what the runtime's own path was asked to start");
        expect(execs.size() == 3 && execs[0]->a == 0x80 && execs[1]->a == 0x100 && execs[2]->a == PS2_HOST_VU1_CONTINUE_PC,
               "execute start addresses: 0x80, 0x100, continue");

        // Ordering between UNPACK and execute: the data of a later UNPACK was not there yet at the first start.
        if (execs.size() == 3)
        {
            const auto &s0 = execs[0]->dataSnapshot;
            const auto &s1 = execs[1]->dataSnapshot;
            const std::vector<uint8_t> zeros(sizeof(kVec2), 0);
            expect(std::memcmp(s0.data() + 0x10 * 16, kVec1, sizeof(kVec1)) == 0, "first start sees the first UNPACK");
            expect(std::memcmp(s0.data() + 0x104 * 16, zeros.data(), zeros.size()) == 0, "first start does not yet see the second UNPACK");
            expect(std::memcmp(s1.data() + 0x104 * 16, kVec2, sizeof(kVec2)) == 0, "second start sees the second UNPACK");
        }

        // Statistics: the figures the fake returned are only summed.
        expect(mem.hostVu1Stats().executes == 3 && mem.hostVu1Stats().continues == 0 && mem.hostVu1Stats().unfinished == 0 &&
                   mem.hostVu1Stats().microWrites == 1 && mem.hostVu1Stats().cycles == 1000u + 1001u + 1002u,
               "statistics");

        // XGKICK: the packets the fake produced arrive on path 1, byte for byte; nothing else on path 1.
        expect(gs.countOnPath(1, pkt1) == 1 && gs.countOnPath(1, pkt2) == 1, "XGKICK packets forwarded to the host GS on path 1");
        int path1 = 0;
        for (const auto &t : gs.transfers)
            path1 += t.path == 1 ? 1 : 0;
        expect(path1 == 2, "exactly the two XGKICK packets on path 1");

        // The tap replayed on a fresh memory with a fresh fake gives the same data memory and calls.
        FakeVu1 fake2;
        PS2Memory mem2;
        mem2.initialize();
        mem2.setHostVu1(&fake2.iface);
        mem2.beginHostVu1();
        fake2.calls.clear();
        for (const auto &chunk : tapChunks)
            mem2.processVIF1Data(chunk.data(), static_cast<uint32_t>(chunk.size()));
        expect(std::memcmp(fake2.mem, fake.mem, kData) == 0 && fake2.order() == "WTXTXTX",
               "replaying the tapped chunks reproduces the data memory and the calls");

        // end: the data memory is taken back, thread_end is called, the pointer is the runtime's again.
        fake.calls.clear();
        mem.endHostVu1();
        expect(fake.order() == "E", "endHostVu1 calls thread_end");
        expect(!mem.hostVu1Bound() && mem.getVU1Data() != fake.mem && std::memcmp(mem.getVU1Data(), fake.mem, kData) == 0,
               "after end the runtime's own memory holds the host's data");
        mem.setHostVu1(nullptr);
        expect(mem.hostVu1() == nullptr, "removal");
    }

    void testXgkickReachesRuntimeGs()
    {
        // No host GS: the packet goes through the arbiter to the runtime's own GS (its process function).
        FakeVu1 fake;
        const std::vector<uint8_t> pkt = adPacket(0x3333);
        fake.executePackets = {pkt};
        PS2Memory mem;
        mem.initialize();
        GifArbiter arb;
        std::vector<std::vector<uint8_t>> own;
        arb.setProcessPacketFn([&own](const uint8_t *d, uint32_t n) { own.emplace_back(d, d + n); });
        mem.setGifArbiter(&arb);
        mem.setHostVu1(&fake.iface);
        mem.beginHostVu1();
        // The host library calls the sink from inside execute, as microVU does.
        deliver(mem, {cmd(0x14, 0, 0x0000), 0, 0, 0});
        bool seen = false;
        for (const auto &p : own)
            seen |= p == pkt;
        expect(seen, "XGKICK packet reaches the runtime's own GS when no host GS is installed");
        mem.endHostVu1();
    }

    void testContinuation()
    {
        FakeVu1 fake;
        FakeGs gs;
        const std::vector<uint8_t> pktA = adPacket(0xA), pktB = adPacket(0xB);
        fake.runFor = {2};            // the first start stays running for two continues
        fake.continuePackets = {{}, pktB};
        fake.executePackets = {pktA};
        PS2Memory mem;
        mem.initialize();
        GifArbiter arb;
        arb.setProcessPacketFn([](const uint8_t *, uint32_t) {});
        arb.setHostGs(&gs.iface, &mem.gs());
        mem.setGifArbiter(&arb);
        mem.setHostVu1(&fake.iface);
        mem.beginHostVu1();
        fake.calls.clear();
        deliver(mem, {cmd(0x14, 0, 0x0008), cmd(0x14, 0, 0x0010), 0, 0});
        expect(fake.order() == "TXCCTX", "a program stopped by its budget is continued until it ends, then the next one starts (got " + fake.order() + ")");
        const auto cont = fake.of(Call::Continue);
        expect(cont.size() == 2 && cont[0]->a == PS2_HOST_VU1_SLICE_CYCLES && cont[1]->a == PS2_HOST_VU1_SLICE_CYCLES,
               "continuation budget");
        expect(gs.countOnPath(1, pktA) == 1 && gs.countOnPath(1, pktB) == 1, "packets from execute and from the continuation both forwarded");
        expect(gs.transfers.size() >= 2 && gs.transfers[0].data == pktA && gs.transfers[1].data == pktB, "in the order the host produced them");
        expect(mem.hostVu1Stats().continues == 2 && mem.hostVu1Stats().unfinished == 0, "continuation statistics");
        mem.endHostVu1();

        // A program that never ends: bounded.
        FakeVu1 loop;
        loop.alwaysRunning = true;
        PS2Memory mem2;
        mem2.initialize();
        mem2.setHostVu1(&loop.iface);
        mem2.beginHostVu1();
        loop.calls.clear();
        deliver(mem2, {cmd(0x14, 0, 0x0000), 0, 0, 0});
        expect(loop.of(Call::Continue).size() == PS2_HOST_VU1_MAX_SLICES - 1, "endless program: bounded number of slices");
        expect(mem2.hostVu1Stats().unfinished == 1, "endless program counted as unfinished");
        mem2.endHostVu1();

        // No running()/continue_: one execute per start, nothing else.
        FakeVu1 plain;
        plain.iface.running = nullptr;
        plain.iface.continue_ = nullptr;
        plain.iface.set_top = nullptr;
        plain.iface.reset = nullptr;
        plain.iface.thread_begin = nullptr;
        plain.iface.thread_end = nullptr;
        PS2Memory mem3;
        mem3.initialize();
        mem3.setHostVu1(&plain.iface);
        mem3.beginHostVu1();
        plain.calls.clear();
        deliver(mem3, {cmd(0x14, 0, 0x0004), cmd(0x17, 0, 0), 0, 0});
        expect(plain.order() == "XX", "optional callbacks may be null (got " + plain.order() + ")");
        mem3.endHostVu1();
    }

    void testWindows()
    {
        FakeVu1 fake;
        PS2Memory mem;
        mem.initialize();
        // Written before the host is bound: carried over by begin.
        mem.write32(0x1100C000u + 0x40, 0xCAFEBABEu);
        mem.write32(0x11008000u + 0x20, 0x01020304u);
        mem.setHostVu1(&fake.iface);
        mem.beginHostVu1();
        expect(fake.order() == "BDRW", "begin pushes microcode that was already written (got " + fake.order() + ")");
        uint32_t v = 0;
        std::memcpy(&v, fake.mem + 0x40, 4);
        expect(v == 0xCAFEBABEu, "data written before binding is carried over");
        fake.calls.clear();

        // Data window: the same memory as the host's.
        mem.write32(0x1100C000u + 0x100, 0x12345678u);
        std::memcpy(&v, fake.mem + 0x100, 4);
        expect(v == 0x12345678u, "EE store to the data window lands in the host data memory");
        fake.mem[0x200] = 0x5A;
        expect(mem.read8(0x1100C000u + 0x200) == 0x5A, "EE load from the data window reads the host data memory");
        mem.write128(0x1100C000u + 0x300, _mm_set_epi32(4, 3, 2, 1));
        uint32_t q[4];
        std::memcpy(q, fake.mem + 0x300, 16);
        expect(q[0] == 1 && q[3] == 4, "128-bit store to the data window");

        // Code window: forwarded with the exact range written.
        mem.write8(0x11008000u + 0x10, 0xAB);
        mem.write16(0x11008000u + 0x12, 0xCDEF);
        mem.write32(0x11008000u + 0x14, 0x11223344u);
        mem.write64(0x11008000u + 0x18, 0x8877665544332211ull);
        mem.write128(0x11008000u + 0x20, _mm_set_epi32(0x0D0C0B0A, 0x09080706, 0x05040302, 0x01FFEEDD));
        const auto w = fake.of(Call::WriteMicro);
        const uint32_t addrs[5] = {0x10, 0x12, 0x14, 0x18, 0x20};
        const uint32_t sizes[5] = {1, 2, 4, 8, 16};
        bool ok = w.size() == 5;
        for (size_t i = 0; ok && i < 5; ++i)
            ok = w[i]->a == addrs[i] && w[i]->b == sizes[i];
        expect(ok, "each EE store to the code window is one write_micro with its address and size");
        expect(fake.micro[0x10] == 0xAB && fake.micro[0x13] == 0xCD && fake.micro[0x14] == 0x44 && fake.micro[0x1F] == 0x88 &&
                   fake.micro[0x20] == 0xDD && fake.micro[0x2F] == 0x0D,
               "the host's code memory holds what the window stores wrote");
        expect(mem.read32(0x11008000u + 0x14) == 0x11223344u, "the window reads back through the runtime's mirror");
        mem.endHostVu1();
    }

    void testFallbacks()
    {
        // data() returning null: the runtime stays on its own VU1.
        FakeVu1 nullData;
        nullData.returnNullData = true;
        PS2Memory mem;
        mem.initialize();
        Starts starts;
        hookInterpreterCallbacks(mem, starts);
        mem.setHostVu1(&nullData.iface);
        mem.beginHostVu1();
        expect(!mem.hostVu1Bound(), "data() returning null leaves the hook unbound");
        deliver(mem, {cmd(0x14, 0, 0x0010), cmd(0x4A, 1, 0x0000), 0x1u, 0x2u});
        expect(starts.list.size() == 1 && starts.list[0].pc == 0x80, "unbound: the runtime's own MSCAL callback runs");
        expect(nullData.of(Call::Execute).empty() && nullData.of(Call::WriteMicro).empty(), "unbound: no host call");
        mem.endHostVu1();
        expect(nullData.order() == "BDE", "unbound: thread_end still called");

        // An incomplete interface is not installed.
        Ps2HostVu1 partial;
        partial.data = nullData.iface.data;
        PS2Memory mem2;
        mem2.initialize();
        mem2.setHostVu1(&partial);
        expect(mem2.hostVu1() == nullptr, "an interface without write_micro and execute is rejected");

        // Installed but the game thread never started: nothing is delegated.
        FakeVu1 idle;
        PS2Memory mem3;
        mem3.initialize();
        Starts s3;
        hookInterpreterCallbacks(mem3, s3);
        mem3.setHostVu1(&idle.iface);
        deliver(mem3, {cmd(0x14, 0, 0x0010), 0, 0, 0});
        expect(s3.list.size() == 1 && idle.calls.empty(), "before begin the runtime's own path is used");

        // The VIF1 FIFO goes through the same interpreter (and the tap).
        PS2Memory mem4;
        mem4.initialize();
        FakeVu1 fifo;
        mem4.setHostVu1(&fifo.iface);
        mem4.beginHostVu1();
        fifo.calls.clear();
        int taps = 0;
        mem4.setVif1Tap([](void *u, const uint8_t *, uint32_t n) { *static_cast<int *>(u) += n == 16 ? 1 : 0; }, &taps);
        mem4.write128(0x10005000u, _mm_set_epi32(0, 0, 0, static_cast<int>(cmd(0x14, 0, 0x0018))));
        expect(fifo.order() == "TX" && fifo.of(Call::Execute)[0]->a == 0xC0 && taps == 1,
               "an MSCAL written to the VIF1 FIFO starts the host VU1 and is tapped");
        mem4.endHostVu1();
    }
} // namespace

int main()
{
    testStreamBothWays();
    testXgkickReachesRuntimeGs();
    testContinuation();
    testWindows();
    testFallbacks();
    if (g_failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("host_vu1_hook_test: all checks passed");
    return 0;
}
