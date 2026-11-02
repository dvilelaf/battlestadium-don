// SPDX-License-Identifier: GPL-3.0-or-later
// T-110 part 2: the host program (src/host) with a synthetic guest: no game, no ELF, no generated code.
//
// The guest is a host function at 0x100000 that follows the path of the SDK calls a game makes at start-up:
//   sceGsResetGraph, sceGsSetDefDBuff, sceGsSwapDBuff (the runtime's own handlers, ps2_stubs::*),
// and then, once per vertical blank, builds a GIF packet in guest memory and sends it through the GIF DMA
// channel (the registers 0x1000A000...), exactly as a game does, and waits for the next vertical blank.
// Every display register comes from those handlers: the guest writes no privileged register itself (with a
// host GS installed the handlers produce what the game's own SDK code produces, patch 0021).
//
// The scene (640x448, PSMCT32, 60 frames per second), documented here because tests/host/test_host_synthetic.py
// checks the same numbers:
//   background            whole frame, (0x20,0x40,0x80)
//   three static boxes    context 1, y 40..119: red x 40..139, green x 200..299, blue x 360..459
//   scissor box           context 2, (0xC8,0x64,0x32), x 20..619, y 160..219: clipped by SCISSOR_2
//   blend box             context 1 with alpha blending (ALPHA_1: (Cs-Cd)*As+Cd, As = 0x30), white,
//                         x 20..619, y 300..359: shows ALPHA_1
//   moving box            context 1, yellow (0xFF,0xFF,0), x = 10 + (5*frame mod 540) .. +59, y 390..429
//   pad indicators        (T-112) context 1, y 200..249: x 10..59 red (0xFF,0,0) while CIRCLE is held on
//                         port 0 slot 0, black otherwise; x 80..129 green (0,0xFF,0) while LEFT is held
//
// The scene's own state (SCISSOR_2, ALPHA_1 and the context-2 frame, z-buffer, offset and test) is sent
// AFTER the display set-up, not before: sceGsResetGraph writes CSR.RESET, and a GS reset clears the drawing
// registers (PCSX2: gsCSRwrite zeroes the privileged registers and calls GSDrawingEnvironment::Reset), so a
// program sets its drawing state after ResetGraph, as a game does. The display set-up itself sets context 1
// (the draw environment of sceGsSetDefDBuff) and PRMODECONT.
//
// DON_TEST_PERFRAME=1 also sets SCISSOR_2 and ALPHA_1 at the start of every frame.
// DON_TEST_FAIL=n makes the guest throw at vertical blank n (exit code 4).
#include "host.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x01000000u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount =
    (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};

// The runtime calls don_game_module() (patch 0088); this synthetic guest has no game module, so it is empty, as in
// the runner's register_functions.cpp.
const DonGameModule &don_game_module()
{
    static const DonGameModule module = {nullptr, 0u};
    return module;
}

namespace
{
    constexpr uint32_t kEntry = 0x00100000u;
    constexpr int kWidth = 640, kHeight = 448;

    // GS registers (A+D addresses).
    constexpr uint64_t PRIM = 0x00, RGBAQ = 0x01, XYZ2 = 0x05, ALPHA_1 = 0x42, SCISSOR_2 = 0x41, TEST_2 = 0x48,
                       FRAME_2 = 0x4D, ZBUF_2 = 0x4F, XYOFFSET_2 = 0x19;
    constexpr uint64_t SPRITE = 6, ABE = 1u << 6, CTXT2 = 1u << 9;

    struct Qword
    {
        uint64_t lo, hi;
    };

    struct Packet
    {
        std::vector<Qword> q;
        size_t tag = 0;
        void begin() { q.clear(); q.push_back({0, 0xE}); }
        void ad(uint64_t reg, uint64_t value) { q.push_back({value, reg}); }
        void finish() { q[0].lo = (q.size() - 1) | (1ull << 15) | (1ull << 60); }
    };

    uint64_t xyz(int x, int y)
    {
        // The draw environment's XYOFFSET is (2048 - w/2, 2048 - h/2) in 12.4.
        return static_cast<uint64_t>((2048 - kWidth / 2 + x) * 16) |
               (static_cast<uint64_t>((2048 - kHeight / 2 + y) * 16) << 16);
    }

    uint64_t rgbaq(int r, int g, int b, int a = 0x80)
    {
        return r | (g << 8) | (b << 16) | (static_cast<uint64_t>(a) << 24) | (0x3F800000ull << 32);
    }

    void box(Packet &p, uint64_t prim, int r, int g, int b, int a, int x0, int y0, int x1, int y1)
    {
        // x1, y1 exclusive.
        p.ad(PRIM, prim);
        p.ad(RGBAQ, rgbaq(r, g, b, a));
        p.ad(XYZ2, xyz(x0, y0));
        p.ad(XYZ2, xyz(x1, y1));
    }

    constexpr uint64_t kScissorFull = 0ull | (uint64_t(kWidth - 1) << 16) | (0ull << 32) | (uint64_t(kHeight - 1) << 48);
    // ALPHA_1: A = Cs, B = Cd, C = As, D = Cd.
    constexpr uint64_t kAlpha = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6) | (0x80ull << 32);

    // T-206: four single-precision results whose bit pattern differs between the console's floating-point
    // mode (round toward zero, FTZ and DAZ on) and the host's (round to nearest, denormals kept). The
    // operands are volatile so that this is computed at run time, on the game thread, with whatever mode
    // that thread has; the low byte of each result's pattern is painted as a pixel (see the scene below).
    // The two expected sets of bytes are stated in tests/host/test_host_synthetic.py:
    //   multiply  255.0f * 0.00197001174f   console 0x3F009A34   host 0x3F009A35  (journal 013's pair)
    //   add       1.0f + 8.9e-8f            console 0x3F800000   host 0x3F800001
    //   divide    1.0f / 3.0f               console 0x3EAAAAAA   host 0x3EAAAAAB
    //   multiply  1e-38f * 0.5f (denormal)  console 0x00000000   host 0x003671F7
    void fp_mode_bytes(uint8_t out[4])
    {
        volatile float a = 255.0f, b = 0.00197001174f;
        volatile float c = 1.0f, d = 8.9e-8f;
        volatile float e = 1.0f, f = 3.0f;
        volatile float g = 1e-38f, h = 0.5f;
        const float results[4] = {a * b, c + d, e / f, g * h};
        for (int i = 0; i < 4; ++i)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, const_cast<const float *>(&results[i]), sizeof(bits));
            out[i] = static_cast<uint8_t>(bits & 0xFFu);
        }
    }

    // T-206 step 3: the project's floating-point flags must not fuse a*b+c (-ffp-contract=off). With
    // these operands the fused result is 0x34000001 and the unfused one 0x34000000, so the pixel's low
    // byte must be 0x00. Computed here, in code compiled with the same options as the guest.
    uint8_t fma_byte()
    {
        volatile float x = 1.0f + 1.1920929e-7f, y = 1.0f + 1.1920929e-7f, z = -(1.0f + 1.1920929e-7f);
        const float result = x * y + z;
        uint32_t bits = 0;
        std::memcpy(&bits, const_cast<const float *>(&result), sizeof(bits));
        return static_cast<uint8_t>(bits & 0xFFu);
    }

    struct Guest
    {
        int fail_at = 0;
        bool perframe = false;
        bool started = false;
        int frame = 0;
        uint32_t dbuff = 0;   // sceGsDBuff in guest memory
        uint32_t packet = 0;  // GIF packet buffer in guest memory
        uint32_t padData = 0; // 32 bytes of scePadRead data (T-112)
    };
    Guest g;

    void call(uint8_t *rdram, R5900Context &c, PS2Runtime *rt,
              void (*fn)(uint8_t *, R5900Context *, PS2Runtime *), uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0,
              uint32_t a3 = 0)
    {
        (void)rdram;
        c.r[4] = _mm_set_epi64x(0, a0);
        c.r[5] = _mm_set_epi64x(0, a1);
        c.r[6] = _mm_set_epi64x(0, a2);
        c.r[7] = _mm_set_epi64x(0, a3);
        fn(rdram, &c, rt);
    }

    uint32_t callR(uint8_t *rdram, R5900Context &c, PS2Runtime *rt,
                   void (*fn)(uint8_t *, R5900Context *, PS2Runtime *), uint32_t a0, uint32_t a1 = 0,
                   uint32_t a2 = 0, uint32_t a3 = 0)
    {
        call(rdram, c, rt, fn, a0, a1, a2, a3);
        return getRegU32(&c, 2);
    }

    void sendPacket(PS2Runtime *rt, uint8_t *rdram, const Packet &p)
    {
        const uint32_t bytes = static_cast<uint32_t>(p.q.size() * sizeof(Qword));
        std::memcpy(rdram + g.packet, p.q.data(), bytes);
        auto &mem = rt->memory();
        mem.writeIORegister(0x1000A010u, g.packet);
        mem.writeIORegister(0x1000A020u, static_cast<uint32_t>(p.q.size()));
        mem.writeIORegister(0x1000A000u, 0x101u);
        mem.processPendingTransfers();
    }

    void guestEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g.fail_at != 0 && g.frame >= g.fail_at)
            throw std::runtime_error("synthetic guest failure (DON_TEST_FAIL)");

        if (!g.started)
        {
            g.started = true;
            g.dbuff = rt->guestMalloc(0x400, 64);
            g.packet = rt->guestMalloc(0x1000, 64);
            const uint32_t stack = rt->guestMalloc(0x100, 16);
            R5900Context c = *ctx;
            c.r[29] = _mm_set_epi64x(0, stack);

            // 1. The SDK start-up: reset, default double buffer (640x448, PSMCT32, ZTEST 0, ZPSM 0x31, no
            // clear), swap to buffer 0. SMODE1, DISPLAY and the rest come from the handlers alone: the guest
            // writes no privileged register. The 5th..7th arguments are on the stack at sp+16, sp+20, sp+24.
            call(rdram, c, rt, ps2_stubs::sceGsResetGraph, 0, 1, 2, 0);
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(c.r[29], 0));
            const uint32_t extra[3] = {0, 0x31, 0};
            std::memcpy(rdram + sp + 16, extra, sizeof(extra));
            call(rdram, c, rt, ps2_stubs::sceGsSetDefDBuff, g.dbuff, 0, kWidth, kHeight);
            call(rdram, c, rt, ps2_stubs::sceGsSwapDBuff, g.dbuff, 0);

            // 2. The scene's own state, in context 2 and for the blend in context 1. It comes after the
            // reset, which cleared the drawing registers (see the header).
            Packet state;
            state.begin();
            state.ad(SCISSOR_2, kScissorFull);
            state.ad(ALPHA_1, kAlpha);
            state.ad(FRAME_2, 0ull | (uint64_t(kWidth / 64) << 16));
            const uint64_t zbp = uint64_t(kWidth / 64) * ((kHeight + 31) / 32);
            state.ad(ZBUF_2, zbp | (1ull << 32));
            state.ad(XYOFFSET_2, (uint64_t(2048 - kWidth / 2) << 4) | ((uint64_t(2048 - kHeight / 2) << 4) << 32));
            state.ad(TEST_2, (1ull << 16) | (1ull << 17));
            state.finish();
            sendPacket(rt, rdram, state);

            // 3. The pad, the way the game's own SDK code does it (T-112): init, open port 0 slot 0 with a
            // 32-byte DMA buffer, and from then on scePadRead into it every frame.
            call(rdram, c, rt, ps2_stubs::scePadInit, 0);
            g.padData = rt->guestMalloc(0x20, 64);
            call(rdram, c, rt, ps2_stubs::scePadPortOpen, 0, 0, g.padData);
        }

        // T-112: the pad of port 0 slot 0, read the way the game does it. The state must be "stable" (6)
        // before the data is trusted (the game checks the same thing first, see docs/knowledge/14 section E);
        // bytes 2 and 3 of the 32-byte buffer are the button word, active low.
        uint8_t padBytes[32] = {};
        R5900Context padCtx = *ctx;
        call(rdram, padCtx, rt, ps2_stubs::scePadRead, 0, 0, g.padData);
        const bool padStable = callR(rdram, padCtx, rt, ps2_stubs::scePadGetState, 0, 0) == 6;
        std::memcpy(padBytes, rdram + g.padData, sizeof(padBytes));
        const bool circleHeld = padStable && (padBytes[3] & 0x20u) == 0u; // bit 13
        const bool leftHeld = padStable && (padBytes[2] & 0x80u) == 0u;   // bit 7

        Packet p;
        p.begin();
        if (g.perframe)
        {
            p.ad(SCISSOR_2, kScissorFull);
            p.ad(ALPHA_1, kAlpha);
        }
        // Context 1 is what sceGsSetDefDBuff set (frame, offset, scissor, test).
        box(p, SPRITE, 0x20, 0x40, 0x80, 0x80, 0, 0, kWidth, kHeight);
        box(p, SPRITE, 0xE0, 0x20, 0x20, 0x80, 40, 40, 140, 120);
        box(p, SPRITE, 0x20, 0xE0, 0x20, 0x80, 200, 40, 300, 120);
        box(p, SPRITE, 0x20, 0x20, 0xE0, 0x80, 360, 40, 460, 120);
        box(p, SPRITE | CTXT2, 0xC8, 0x64, 0x32, 0x80, 20, 160, 620, 220);
        box(p, SPRITE | ABE, 0xFF, 0xFF, 0xFF, 0x30, 20, 300, 620, 360);
        box(p, SPRITE, circleHeld ? 0xFF : 0x00, 0x00, 0x00, 0x80, 10, 200, 60, 250);
        box(p, SPRITE, 0x00, leftHeld ? 0xFF : 0x00, 0x00, 0x80, 80, 200, 130, 250);
        const int mx = 10 + (5 * g.frame) % 540;
        box(p, SPRITE, 0xFF, 0xFF, 0x00, 0x80, mx, 390, mx + 60, 430);
        // T-206: the game thread's floating-point mode, four 1-pixel sprites at y 440 (a row the rest of
        // the scene does not use), x = 10, 30, 50, 70, greyscale: r = g = b = the low byte of the result.
        // tests/host/test_host_synthetic.py reads these pixels and compares them with the two sets above.
        uint8_t fp[4];
        fp_mode_bytes(fp);
        for (int i = 0; i < 4; ++i)
            box(p, SPRITE, fp[i], fp[i], fp[i], 0x80, 10 + 20 * i, 440, 11 + 20 * i, 441);
        const uint8_t fma = fma_byte(); // T-206 step 3: 0x00 means a*b+c was not fused
        box(p, SPRITE, fma, fma, fma, 0x80, 90, 440, 91, 441);
        p.finish();
        sendPacket(rt, rdram, p);

        ++g.frame;
        rt->eeWaitVSyncTicks(1, kEntry);
    }

    bool setup(PS2Runtime &runtime, const DonHostOptions &, void *)
    {
        g.perframe = std::getenv("DON_TEST_PERFRAME") != nullptr;
        if (const char *v = std::getenv("DON_TEST_FAIL"))
            g.fail_at = std::atoi(v);
        if (!runtime.registerFunction(kEntry, guestEntry))
            return false;
        runtime.cpu().pc = kEntry;
        return true;
    }
}

int main(int argc, char **argv)
{
    DonHostGame game;
    game.title = "DON host synthetic test";
    game.needs_elf = false;
    game.setup = setup;
    return don_host_run(argc, argv, game);
}
