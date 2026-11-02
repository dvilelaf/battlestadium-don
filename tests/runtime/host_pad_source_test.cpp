// SPDX-License-Identifier: GPL-3.0-or-later
// T-110 part 1: the host pad source (PS2Runtime::setHostPad, runtime/ps2_host_pad.h). A real PS2Runtime
// with the host owning the window; the pad handlers the generated code calls (scePadRead...) are called
// the way the generated code calls them: handler(rdram, ctx, runtime) with the arguments in a0..a3.
// Run it with DISPLAY unset (the test unsets it itself). No game, no generated code.
//
// What it proves:
//   * two ports x two slots: every pad's buffer carries the state of its own (port, slot);
//   * a pad the source says is not connected reads as not connected (scePadRead 0, buffer 0xFF status,
//     scePadGetState 0, scePadInfoMode 0); a (port, slot) outside the description cannot be opened;
//   * scePadGetPortMax / scePadGetSlotMax follow the description ({4,1} multitap shape as well);
//   * pressure bytes come from the source after scePadEnterPressMode;
//   * with a host-owned window and no source: nothing connected and no raylib call at all;
//   * (raylib builds) without a source and without a host-owned window the legacy path is intact:
//     2 ports, 1 slot, raylib asked for input, the pad reads as connected.

#include "host_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2_stubs
{
    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

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

    int32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
    {
        R5900Context ctx;
        ctx.r[4] = _mm_set_epi32(0, 0, 0, static_cast<int>(a0));
        ctx.r[5] = _mm_set_epi32(0, 0, 0, static_cast<int>(a1));
        ctx.r[6] = _mm_set_epi32(0, 0, 0, static_cast<int>(a2));
        ctx.r[7] = _mm_set_epi32(0, 0, 0, static_cast<int>(a3));
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return static_cast<int32_t>(_mm_extract_epi32(ctx.r[2], 0));
    }

    // Guest buffers: one 32-byte DMA string and one data buffer per pad.
    uint32_t dmaAddr(int port, int slot) { return 0x00010000u + static_cast<uint32_t>(port * 4 + slot) * 0x100u; }
    uint32_t dataAddr(int port, int slot) { return 0x00020000u + static_cast<uint32_t>(port * 4 + slot) * 0x100u; }

    struct FakeSource
    {
        Ps2HostPadState pads[2][4];
        int reads = 0;
        Ps2HostPad iface;

        FakeSource()
        {
            iface.user = this;
            iface.read = [](void *u, int port, int slot, Ps2HostPadState *out) {
                auto *self = static_cast<FakeSource *>(u);
                ++self->reads;
                *out = self->pads[port][slot];
            };
        }
    };

    uint16_t buttonsFor(int port, int slot) { return static_cast<uint16_t>(0xFFFFu ^ (1u << (4 + port * 4 + slot))); }

    void fillDistinct(FakeSource &src, uint32_t ports, const uint32_t *slots)
    {
        src.iface.port_count = ports;
        for (uint32_t p = 0; p < ports; ++p)
            src.iface.slots_per_port[p] = slots[p];
        for (int p = 0; p < 2; ++p)
        {
            for (int s = 0; s < 4; ++s)
            {
                Ps2HostPadState &st = src.pads[p][s];
                st = Ps2HostPadState{};
                st.connected = 1;
                st.buttons = buttonsFor(p, s);
                st.rx = static_cast<uint8_t>(0x10 + p * 0x20 + s);
                st.ry = static_cast<uint8_t>(0x11 + p * 0x20 + s);
                st.lx = static_cast<uint8_t>(0x12 + p * 0x20 + s);
                st.ly = static_cast<uint8_t>(0x13 + p * 0x20 + s);
            }
        }
    }

    void openAll(PS2Runtime &rt, int ports, int slots)
    {
        call(rt, ps2_stubs::scePadInit);
        for (int p = 0; p < ports; ++p)
            for (int s = 0; s < slots; ++s)
                expect(call(rt, ps2_stubs::scePadPortOpen, p, s, dmaAddr(p, s)) == 1,
                       "scePadPortOpen(" + std::to_string(p) + "," + std::to_string(s) + ") succeeds");
    }

    std::string join(const std::vector<std::string> &v)
    {
        std::string out;
        for (const auto &s : v)
            out += (out.empty() ? "" : ",") + s;
        return out;
    }

    void scenarioTwoByTwo()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-pad-test"), "initialize() with the host owning the window");
        FakeSource src;
        const uint32_t slots[2] = {2, 2};
        fillDistinct(src, 2, slots);
        src.pads[1][1].connected = 0; // the fourth pad is unplugged
        rt.setHostPad(&src.iface);

        expect(call(rt, ps2_stubs::scePadGetPortMax) == 2, "GetPortMax is 2");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 0) == 2, "GetSlotMax(0) is 2");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 1) == 2, "GetSlotMax(1) is 2");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 2) == 0, "GetSlotMax(2) is 0");

        openAll(rt, 2, 2);
        expect(call(rt, ps2_stubs::scePadPortOpen, 0, 2, dmaAddr(0, 2)) == 0, "port 0 slot 2 cannot be opened");
        expect(call(rt, ps2_stubs::scePadPortOpen, 2, 0, dmaAddr(0, 3)) == 0, "port 2 cannot be opened");

        uint8_t *ram = rt.memory().getRDRAM();
        for (int p = 0; p < 2; ++p)
        {
            for (int s = 0; s < 2; ++s)
            {
                const std::string tag = "pad " + std::to_string(p) + "," + std::to_string(s) + ": ";
                const uint8_t *d = ram + dataAddr(p, s);
                const int32_t rc = call(rt, ps2_stubs::scePadRead, p, s, dataAddr(p, s));
                if (p == 1 && s == 1)
                {
                    expect(rc == 0, tag + "not connected: scePadRead returns 0, got " + std::to_string(rc));
                    expect(d[0] == 0xFF, tag + "not connected: status byte is 0xFF");
                    expect(d[2] == 0xFF && d[3] == 0xFF, tag + "not connected: no button bits");
                    expect(call(rt, ps2_stubs::scePadGetState, p, s) == 0, tag + "GetState is 0 (disconnected)");
                    expect(call(rt, ps2_stubs::scePadInfoMode, p, s, 1, 0) == 0, tag + "InfoMode is 0");
                    continue;
                }
                expect(rc == 1, tag + "scePadRead returns 1, got " + std::to_string(rc));
                expect(d[0] == 0, tag + "status byte is 0 (OK)");
                const uint16_t want = buttonsFor(p, s);
                expect(d[2] == (want & 0xFF) && d[3] == (want >> 8), tag + "buttons are the pad's own");
                expect(d[4] == 0x10 + p * 0x20 + s, tag + "rx");
                expect(d[5] == 0x11 + p * 0x20 + s, tag + "ry");
                expect(d[6] == 0x12 + p * 0x20 + s, tag + "lx");
                expect(d[7] == 0x13 + p * 0x20 + s, tag + "ly");
                expect(call(rt, ps2_stubs::scePadGetState, p, s) != 0, tag + "GetState is not disconnected");
                expect(call(rt, ps2_stubs::scePadInfoMode, p, s, 1, 0) != 0, tag + "InfoMode answers");
            }
        }
        expect(src.reads > 0, "the source was read");

        // The state follows the source: unplug pad 0,0, change pad 0,1.
        src.pads[0][0].connected = 0;
        src.pads[0][1].buttons = 0x1234;
        expect(call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0)) == 0, "pad 0,0 unplugged: read returns 0");
        expect(call(rt, ps2_stubs::scePadRead, 0, 1, dataAddr(0, 1)) == 1, "pad 0,1 still reads");
        expect(ram[dataAddr(0, 1) + 2] == 0x34 && ram[dataAddr(0, 1) + 3] == 0x12, "pad 0,1 buttons follow the source");
        rt.setHostPad(nullptr);
    }

    void scenarioPressure()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-pad-test"), "initialize()");
        FakeSource src;
        const uint32_t slots[2] = {1, 1};
        fillDistinct(src, 2, slots);
        src.pads[0][0].has_pressure = 1;
        for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
            src.pads[0][0].pressure[i] = static_cast<uint8_t>(0x40 + i);
        rt.setHostPad(&src.iface);
        openAll(rt, 2, 1);
        uint8_t *ram = rt.memory().getRDRAM();

        call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0));
        bool zero = true;
        for (int i = 8; i < 20; ++i)
            zero = zero && ram[dataAddr(0, 0) + i] == 0;
        expect(zero, "pressure bytes are 0 before scePadEnterPressMode");

        expect(call(rt, ps2_stubs::scePadEnterPressMode, 0, 0) == 1, "scePadEnterPressMode succeeds");
        call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0));
        bool ok = true;
        for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
            ok = ok && ram[dataAddr(0, 0) + 8 + i] == 0x40 + i;
        expect(ok, "pressure bytes 8..19 come from the source");

        // The shoulder buttons: bytes 16..19 are L1, R1, L2, R2 (the DualShock 2 order). The source's
        // pressure[8..11] follow the buffer.
        Ps2HostPadState &pad = src.pads[0][0];
        pad.buttons = 0xFFFF;
        pad.pressure[8] = 0x11;  // L1
        pad.pressure[9] = 0x22;  // R1
        pad.pressure[10] = 0x33; // L2
        pad.pressure[11] = 0x44; // R2
        call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0));
        expect(ram[dataAddr(0, 0) + 16] == 0x11 && ram[dataAddr(0, 0) + 17] == 0x22 &&
                   ram[dataAddr(0, 0) + 18] == 0x33 && ram[dataAddr(0, 0) + 19] == 0x44,
               "source pressure[8..11] = L1, R1, L2, R2 are bytes 16..19 in that order");

        // Without pressure values from the source the bytes derive from the buttons: each shoulder
        // button, pressed alone (active low: L2 bit 8, R2 bit 9, L1 bit 10, R1 bit 11), sets only its own byte.
        pad.has_pressure = 0;
        struct Shoulder
        {
            const char *name;
            int bit;
            int byte;
        };
        const Shoulder shoulders[] = {{"L1", 10, 16}, {"R1", 11, 17}, {"L2", 8, 18}, {"R2", 9, 19}};
        for (const Shoulder &sh : shoulders)
        {
            pad.buttons = static_cast<uint16_t>(~(1u << sh.bit));
            call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0));
            bool only = true;
            for (int b = 16; b < 20; ++b)
                only = only && ram[dataAddr(0, 0) + b] == (b == sh.byte ? 0xFF : 0x00);
            expect(only, std::string(sh.name) + " pressed alone sets only byte " + std::to_string(sh.byte));
        }
        rt.setHostPad(nullptr);
    }

    void scenarioMultitapShape()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("host-pad-test"), "initialize()");
        FakeSource src;
        const uint32_t slots[2] = {4, 1};
        fillDistinct(src, 1, slots);
        rt.setHostPad(&src.iface);
        call(rt, ps2_stubs::scePadInit); // the pad table is global: forget what earlier scenarios opened
        expect(call(rt, ps2_stubs::scePadGetPortMax) == 1, "{4}: GetPortMax is 1");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 0) == 4, "{4}: GetSlotMax(0) is 4");
        for (int s = 0; s < 4; ++s)
            expect(call(rt, ps2_stubs::scePadGetState, 0, s) == 99, "{4}: GetState of a slot not opened yet is 99 (T-306)");
        openAll(rt, 1, 4);
        expect(call(rt, ps2_stubs::scePadPortOpen, 1, 0, dmaAddr(1, 0)) == 0, "{4}: port 1 cannot be opened");
        uint8_t *ram = rt.memory().getRDRAM();
        for (int s = 0; s < 4; ++s)
        {
            expect(call(rt, ps2_stubs::scePadRead, 0, s, dataAddr(0, s)) == 1, "{4}: slot reads");
            expect(ram[dataAddr(0, s) + 4] == 0x10 + s, "{4}: slot " + std::to_string(s) + " has its own state");
        }
        rt.setHostPad(nullptr);
        expect(call(rt, ps2_stubs::scePadGetPortMax) == 2, "topology back to 2 ports after setHostPad(nullptr)");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 0) == 1, "topology back to 1 slot after setHostPad(nullptr)");
    }

    void scenarioNoSourceHostOwned()
    {
        rt_test::raylibLog().take();
        {
            PS2Runtime rt;
            rt.setHostOwnsWindow(true);
            expect(rt.initialize("host-pad-test"), "initialize()");
            openAll(rt, 2, 1);
            uint8_t *ram = rt.memory().getRDRAM();
            std::memset(ram + dataAddr(0, 0), 0x55, 32);
            expect(call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0)) == 0, "no source, host-owned: not connected");
            expect(ram[dataAddr(0, 0)] == 0xFF, "no source, host-owned: status byte 0xFF");
            expect(call(rt, ps2_stubs::scePadGetState, 0, 0) == 0, "no source, host-owned: GetState 0");
        }
        const auto calls = rt_test::raylibLog().take();
        expect(calls.empty(), "no source, host-owned: no raylib call, got [" + join(calls) + "]");
    }

#ifdef DON_RT_RAYLIB_INTERPOSED
    void scenarioLegacy()
    {
        rt_test::raylibLog().take();
        PS2Runtime rt;
        expect(!rt.hostOwnsWindow(), "legacy: the runtime owns the window");
        expect(rt.initialize("host-pad-test"), "legacy: initialize() (recorded raylib, no display)");
        rt_test::raylibLog().take();
        openAll(rt, 2, 1);
        expect(call(rt, ps2_stubs::scePadGetPortMax) == 2, "legacy: 2 ports");
        expect(call(rt, ps2_stubs::scePadGetSlotMax, 0) == 1, "legacy: 1 slot");
        uint8_t *ram = rt.memory().getRDRAM();
        expect(call(rt, ps2_stubs::scePadRead, 0, 0, dataAddr(0, 0)) == 1, "legacy: the pad reads as connected");
        expect(ram[dataAddr(0, 0) + 2] == 0xFF && ram[dataAddr(0, 0) + 3] == 0xFF, "legacy: nothing pressed");
        const auto calls = rt_test::raylibLog().take();
        bool keyboard = false;
        for (const auto &c : calls)
            keyboard = keyboard || c == "IsKeyDown";
        expect(keyboard, "legacy: raylib was asked for the keyboard, got [" + join(calls) + "]");
        std::printf("[host_pad_source_test] legacy path raylib calls: %s\n", join(calls).c_str());
    }
#endif
}

int main()
{
    unsetenv("DISPLAY");
    scenarioTwoByTwo();
    scenarioPressure();
    scenarioMultitapShape();
    scenarioNoSourceHostOwned();
#ifdef DON_RT_RAYLIB_INTERPOSED
    scenarioLegacy();
#else
    std::printf("[host_pad_source_test] no raylib in this build: legacy scenario not applicable\n");
#endif
    if (g_failures != 0)
    {
        std::fprintf(stderr, "host_pad_source_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("host_pad_source_test: all checks passed\n");
    return 0;
}
