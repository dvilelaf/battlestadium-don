// SPDX-License-Identifier: GPL-3.0-or-later
// T-305 patch 0065: sceGsSetDefDBuff places the second buffer at the page the game's own SDK code does.
//
// The movie player of Battle Stadium D.O.N calls sceGsResetGraph(0, interlace=1, NTSC, ffmode=1) and then
// sceGsSetDefDBuff(dbuff, psm 0, w=640, h=224). The game's own code (guest 0x100EE0 with 0x100A40) halves the
// page count of the frame only when interlace == 1 and ffmode == 0, and puts the second buffer's frame-buffer
// page at half the z-buffer page count: 640x224 PSMCT32 is 10 x 7 = 70 pages, so the second buffer is at page
// 70 (the reference console writes FRAME_1 = 0xA0046 there). The replaced handler tested ffmode the other way
// round, found page 35, and the two field buffers overlapped in GS memory.
//
// The game's DBuff structure keeps the second buffer's DISPFB at +0x38 and its FRAME_1 value at +0x60 (the
// two fields the SDK code patches).

#include "host_test_support.h"

#include "Syscalls/Common.h"
#include "runtime/gs/ps2_host_gs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    constexpr uint32_t kStack = 0x00200000u;
    constexpr uint32_t kDBuff = 0x00210000u;

    void call(PS2Runtime &rt, Handler fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
    {
        R5900Context ctx;
        SET_GPR_U32(&ctx, 29, kStack);
        SET_GPR_U32(&ctx, 4, a0);
        SET_GPR_U32(&ctx, 5, a1);
        SET_GPR_U32(&ctx, 6, a2);
        SET_GPR_U32(&ctx, 7, a3);
        fn(rt.memory().getRDRAM(), &ctx, &rt);
    }

    uint64_t read64(PS2Runtime &rt, uint32_t addr)
    {
        uint64_t value = 0;
        std::memcpy(&value, rt.memory().getRDRAM() + addr, sizeof(value));
        return value;
    }

    // Returns the second buffer's frame-buffer page as DISPFB (+0x38) and FRAME_1 (+0x60) hold it.
    void setUp(PS2Runtime &rt, uint32_t ffmode, uint32_t h, uint32_t &dispfbPage, uint32_t &framePage)
    {
        const uint32_t extra[3] = {0u, 0x31u, 0u}; // ztest, zpsm, clear: sceGsSetDefDBuff's stack arguments
        std::memcpy(rt.memory().getRDRAM() + kStack + 16, extra, sizeof(extra));
        std::memset(rt.memory().getRDRAM() + kDBuff, 0, 0x200);
        call(rt, ps2_stubs::sceGsResetGraph, 0u, 1u, 2u, ffmode);
        call(rt, ps2_stubs::sceGsSetDefDBuff, kDBuff, 0u, 640u, h);
        dispfbPage = static_cast<uint32_t>(read64(rt, kDBuff + 0x38u) & 0x1FFu);
        framePage = static_cast<uint32_t>(read64(rt, kDBuff + 0x60u) & 0x1FFu);
    }
}

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    Ps2HostGs iface;
    iface.gif_transfer = [](void *, int, const uint8_t *, uint32_t) {};
    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("gs-dbuff-frame-mode-test"), "initialize()");
    rt.setHostGs(&iface);

    uint32_t dispfb = 0, frame = 0;

    // The movie player: interlace 1, ffmode 1, 640x224 -> 70 pages per field buffer.
    setUp(rt, 1u, 224u, dispfb, frame);
    std::printf("[movie] second buffer page: DISPFB=%u FRAME=%u\n", dispfb, frame);
    expect(dispfb == 70u, "movie mode: the second buffer's DISPFB page is 70, got " + std::to_string(dispfb));
    expect(frame == 70u, "movie mode: the second buffer's FRAME page is 70, got " + std::to_string(frame));

    // The boot display (interlace 1, ffmode 0): the SDK code leaves the second page at 0 (one buffer).
    setUp(rt, 0u, 448u, dispfb, frame);
    std::printf("[boot] second buffer page: DISPFB=%u FRAME=%u\n", dispfb, frame);
    expect(dispfb == 0u, "boot mode: the second buffer's DISPFB page stays 0, got " + std::to_string(dispfb));

    if (g_failures != 0)
    {
        std::fprintf(stderr, "gs_dbuff_frame_mode_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gs_dbuff_frame_mode_test: all checks passed\n");
    return 0;
}
