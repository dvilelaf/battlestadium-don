// SPDX-License-Identifier: GPL-3.0-or-later
// T-108 patch 0021: the display registers the replaced sceGs* handlers produce with a host GS installed.
//
// The reference values are the table of docs/knowledge/18-display-registers.md section 1, measured on the
// console with tools/verify/dump_regs.py. The sequence below is the one the game itself performs at boot:
//
//   sceGsResetGraph(0, interlace=1, omode=2 (NTSC), ffmode=0 (frame))
//   sceGsSetDefDBuff(dbuff, psm=0 (PSMCT32), w=640, h=448), ztest=0, zpsm=0x31, clear=0 on the stack
//   sceGsSwapDBuff(dbuff, 0)                       // sceGsPutDispEnv + sceGsPutDrawEnv
//
// With a host GS installed (PS2Runtime::setHostGs) the handlers must write what the game's own SDK code
// writes (docs/knowledge/18 section 2 and 6); without one they keep the legacy values, which the second
// half of this test pins down (measured against the tree before this patch).
//
// What is NOT the library's, and therefore reported but not held to the table: PMODE (the table's 0x8067 is
// what the engine writes; the SDK's sceGsSetDefDispEnv builds 0x66), the DBY bit and the FBP alternation of
// DISPFB2 (the table's 0x80000001400/0x8000000148C: the engine's), the second circuit's DISPLAY DH of 446
// (the engine writes one less for the other field) and IMR (the SDK's ResetGraph writes 0xFF00, the table
// has 0x7F00).

#include "host_test_support.h"

#include "Syscalls/Common.h" // ps2_stubs::sceGs*, ps2_syscalls::GsSetCrt
#include "runtime/gs/ps2_host_gs.h"

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

    std::string hex(uint64_t value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
        return text;
    }

    // A host GS that only records: the handlers must not need anything from it, but its presence is what
    // puts them in host-GS mode (PS2Runtime::hostGs()).
    struct Host
    {
        std::vector<std::vector<uint8_t>> packets;
        Ps2HostGs iface;

        Host()
        {
            iface.user = this;
            iface.gif_transfer = [](void *user, int, const uint8_t *data, uint32_t bytes) {
                auto *host = static_cast<Host *>(user);
                host->packets.emplace_back(data, data + bytes);
            };
        }
    };

    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    // The stand-in packet the legacy sceGsResetGraph sends carries A+D entries whose register byte is 0x41
    // or 0x42 (which a real GS reads as SCISSOR_2 and ALPHA_1) or 0x59..0x5F (not GS registers at all).
    bool hasResetGraphEntry(const std::vector<uint8_t> &packet)
    {
        for (size_t off = 0; off + 16u <= packet.size(); off += 16u)
        {
            const uint8_t reg = packet[off + 8u];
            if (reg == 0x41u || reg == 0x42u || (reg >= 0x59u && reg <= 0x5Fu))
                return true;
        }
        return false;
    }

    // The DBuff structure and the stack arguments live in guest RAM, above the game's image, in a place
    // nothing else in this test touches.
    constexpr uint32_t kStack = 0x00200000u;
    constexpr uint32_t kDBuff = 0x00210000u;

    uint32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
    {
        R5900Context ctx;
        SET_GPR_U32(&ctx, 29, kStack);
        SET_GPR_U32(&ctx, 4, a0);
        SET_GPR_U32(&ctx, 5, a1);
        SET_GPR_U32(&ctx, 6, a2);
        SET_GPR_U32(&ctx, 7, a3);
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return getRegU32(&ctx, 2);
    }

    // The SDK's own display set-up, exactly as the game calls it (sceGsSetDefDBuff's 5th..7th arguments
    // are on the stack at sp+16, sp+20, sp+24: ztest, zpsm, clear).
    void sdkDisplaySetUp(PS2Runtime &rt)
    {
        uint8_t *rdram = rt.memory().getRDRAM();
        const uint32_t extra[3] = {0u, 0x31u, 0u};
        std::memcpy(rdram + kStack + 16, extra, sizeof(extra));
        call(rt, ps2_stubs::sceGsResetGraph, 0u, 1u, 2u, 0u);
        call(rt, ps2_stubs::sceGsSetDefDBuff, kDBuff, 0u, 640u, 448u);
        call(rt, ps2_stubs::sceGsSwapDBuff, kDBuff, 0u);
    }

    struct Regs
    {
        uint64_t pmode, smode1, smode2, srfsh, synch1, synch2, syncv;
        uint64_t dispfb1, display1, dispfb2, display2, imr;
    };

    Regs snapshot(PS2Runtime &rt)
    {
        const GSRegisters &gs = rt.memory().gs();
        return {gs.pmode,       gs.smode1,  gs.smode2, gs.srfsh,  gs.synch1, gs.synch2,
                gs.syncv,       gs.dispfb1, gs.display1, gs.dispfb2, gs.display2, gs.imr};
    }

    void clearDisplayRegisters(PS2Runtime &rt)
    {
        GSRegisters &gs = rt.memory().gs();
        gs.pmode = gs.smode1 = gs.smode2 = gs.srfsh = gs.synch1 = gs.synch2 = gs.syncv = 0u;
        gs.dispfb1 = gs.display1 = gs.dispfb2 = gs.display2 = gs.imr = 0u;
    }

    void printRegs(const char *what, const Regs &r)
    {
        std::printf("[%s] PMODE=%s SMODE1=%s SMODE2=%s SRFSH=%s SYNCH1=%s SYNCH2=%s SYNCV=%s\n", what,
                    hex(r.pmode).c_str(), hex(r.smode1).c_str(), hex(r.smode2).c_str(), hex(r.srfsh).c_str(),
                    hex(r.synch1).c_str(), hex(r.synch2).c_str(), hex(r.syncv).c_str());
        std::printf("[%s] DISPFB1=%s DISPLAY1=%s DISPFB2=%s DISPLAY2=%s IMR=%s\n", what,
                    hex(r.dispfb1).c_str(), hex(r.display1).c_str(), hex(r.dispfb2).c_str(),
                    hex(r.display2).c_str(), hex(r.imr).c_str());
    }

    // docs/knowledge/18-display-registers.md section 1, the registers the video registers belong to the
    // BIOS/library. All six of the first group come from the SetGsCrt syscall the SDK calls.
    void checkReferenceVideoRegisters(const Regs &r)
    {
        expect(r.smode1 == 0x740814504ull, "SMODE1 is the console's 0x740814504, got " + hex(r.smode1));
        expect(r.smode2 == 0x1ull, "SMODE2 is the console's 0x1 (INT=1, FFMD=0), got " + hex(r.smode2));
        expect(r.srfsh == 0x8ull, "SRFSH is the console's 0x8, got " + hex(r.srfsh));
        expect(r.synch1 == 0x7F5B61F06F040ull, "SYNCH1 is the console's 0x7F5B61F06F040, got " + hex(r.synch1));
        expect(r.synch2 == 0x33A4D8ull, "SYNCH2 is the console's 0x33A4D8, got " + hex(r.synch2));
        expect(r.syncv == 0xC7800601A01801ull, "SYNCV is the console's 0xC7800601A01801, got " + hex(r.syncv));
        // DISPLAY2 is the circuit the SDK drives for a revision other than 1. Its value is the SDK formula's
        // for 640x448 interlace frame, which is the table's DISPLAY1 (the table's DISPLAY2 has DH 446: the
        // engine writes the other field's height there).
        expect(r.display2 == 0x1BF9FF0183227Cull,
               "DISPLAY2 is the SDK's 640x448 interlace-frame display 0x1BF9FF0183227C (= the table's DISPLAY1), got " +
                   hex(r.display2));
        // DISPFB2 is the SDK formula's (psm 0, FBW 10, FBP 0) = the table's DISPFB1; the table's DISPFB2 adds
        // DBY=1 and the engine alternates FBP 0/140.
        expect(r.dispfb2 == 0x1400ull,
               "DISPFB2 is the SDK's 0x1400 (psm 0, FBW 10, FBP 0 = the table's DISPFB1), got " + hex(r.dispfb2));
        // The SDK's own sceGsSetDefDispEnv PMODE is 0x66; the table's 0x8067 is what the engine writes.
        expect(r.pmode == 0x66ull, "PMODE is the SDK's 0x66, got " + hex(r.pmode));
        expect(r.imr == 0xFF00ull, "IMR is the SDK ResetGraph's 0xFF00 (the table's 0x7F00 is the engine's), got " +
                                       hex(r.imr));
        // Circuit 1 is not touched: the SDK drives circuit 2 for a revision other than 1.
        expect(r.dispfb1 == 0ull && r.display1 == 0ull,
               "the revision is not 1, so the handlers leave DISPFB1/DISPLAY1 alone, got " + hex(r.dispfb1) + "/" +
                   hex(r.display1));
    }

    // The legacy (no host GS) values, measured against the tree before this patch: the runtime's own GS keeps
    // the values it was written for. Asserted here so that host-GS support does not change them.
    void checkLegacyRegisters(const Regs &r)
    {
        expect(r.smode1 == 0ull, "without a host GS SMODE1 stays untouched (0), got " + hex(r.smode1));
        expect(r.srfsh == 0ull && r.synch1 == 0ull && r.synch2 == 0ull && r.syncv == 0ull,
               "without a host GS SRFSH/SYNCH1/SYNCH2/SYNCV stay untouched (0)");
        expect(r.smode2 == 0x1ull, "without a host GS SMODE2 is the legacy interlace value 0x1, got " + hex(r.smode2));
        expect(r.pmode == 0x8007ull,
               "without a host GS PMODE is the legacy 0x8007 (EN1, EN2, CRTMD, ALP 0x80), got " + hex(r.pmode));
        expect(r.dispfb1 == 0x1400ull && r.dispfb2 == 0x1400ull,
               "without a host GS both circuits get the legacy DISPFB 0x1400, got " + hex(r.dispfb1) + "/" +
                   hex(r.dispfb2));
        expect(r.display1 == 0x1BF27F0002027Cull && r.display2 == 0x1BF27F0002027Cull,
               "without a host GS both circuits get the legacy DISPLAY (MAGH 0, DW 639, DX 636, DY 32) "
               "0x1BF27F0002027C, got " + hex(r.display1) + "/" + hex(r.display2));
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    Host host;
    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("host-gs-display-test"), "initialize() with the host owning the window");
    uint8_t *rdram = rt.memory().getRDRAM();
    expect(rdram != nullptr, "guest RAM");

    // ---- With a host GS: the console's own display registers ------------------------------------------
    rt.setHostGs(&host.iface);
    expect(rt.hostGs() != nullptr, "the host GS is installed");

    clearDisplayRegisters(rt);
    sdkDisplaySetUp(rt);
    const Regs withHost = snapshot(rt);
    printRegs("host", withHost);
    checkReferenceVideoRegisters(withHost);
    size_t standIn = 0;
    size_t packetBytes = 0;
    for (const auto &packet : host.packets)
    {
        packetBytes += packet.size();
        standIn += hasResetGraphEntry(packet);
    }
    std::printf("[host] %zu GIF packet(s), %zu byte(s) forwarded; %zu of them carry a sceGsResetGraph "
                "stand-in entry (0x41/0x42/0x59..0x5F)\n",
                host.packets.size(), packetBytes, standIn);
    expect(standIn == 0,
           "with a host GS no forwarded packet carries a sceGsResetGraph stand-in entry (0x41/0x42/0x59..0x5F): the "
           "handler writes no display register through the GIF; the packets that are forwarded are the draw "
           "environment's own register writes");
    expect(host.packets.size() == 8u,
           "the display set-up forwards exactly the 8 draw-environment register writes, got " +
               std::to_string(host.packets.size()));

    // ---- Without a host GS: the legacy registers, unchanged --------------------------------------------
    rt.setHostGs(nullptr);
    expect(rt.hostGs() == nullptr, "the host GS is uninstalled");
    clearDisplayRegisters(rt);
    sdkDisplaySetUp(rt);
    const Regs withoutHost = snapshot(rt);
    printRegs("legacy", withoutHost);
    checkLegacyRegisters(withoutHost);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "host_gs_display_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("host_gs_display_test: all checks passed\n");
    return 0;
}
