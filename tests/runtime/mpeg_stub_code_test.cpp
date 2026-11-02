// SPDX-License-Identifier: GPL-3.0-or-later
// T-304 Part 2: the MPEG stubs must not write into the game's own code.
//
// The replaced stubs used to write to absolute guest addresses that are inside the game's functions:
// `sceMpegCreate` stored its first argument at 0x1717BC and nine "reference" pointers at
// 0x171800..0x171823 (both inside FUN_00171710, 0x171710-0x17197B), `sceMpegClearRefBuff` followed those
// nine words as pointers and wrote 0 at `value + 0x28` (also code when the word is an instruction), and
// `sceMpegReset` wrote 0 at 0x171904 (inside the same function). The console's memory has the game's
// instructions there (work/ref/x/02/ee_ram.bin; tools/verify/ram_diff.py against a run's dump shows
// exactly those nine words differing), so every one of those writes destroys executable code. The
// consequence is a jump through the wreckage: the crash of T-304 with microVU and sound (one in three
// runs) is a call or return through a pointer that held a guest-looking value.
//
// What this test proves: after sceMpegCreate, sceMpegClearRefBuff and sceMpegReset have run, every one of
// those words still holds what it held before, while the stub's own legitimate writes (the fields of the
// structure the game passes) still happen. It needs no game and no generated code: a real PS2Runtime and
// a sentinel pattern in RAM.
//
// Run it with DISPLAY unset (the test unsets it itself).

#include "host_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_stubs
{
    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
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

    // Every word the three stubs used to write, with the game's function that contains it.
    struct Guard
    {
        uint32_t addr;
        const char *where;
    };
    const Guard kCodeWords[] = {
        {0x001717BCu, "inside FUN_00171710"},
        {0x00171800u, "inside FUN_00171710"},
        {0x00171804u, "inside FUN_00171710"},
        {0x00171808u, "inside FUN_00171710"},
        {0x0017180Cu, "inside FUN_00171710"},
        {0x00171810u, "inside FUN_00171710"},
        {0x00171814u, "inside FUN_00171710"},
        {0x00171818u, "inside FUN_00171710"},
        {0x0017181Cu, "inside FUN_00171710"},
        {0x00171820u, "inside FUN_00171710"},
        {0x00171904u, "inside FUN_00171710"},
    };

    uint32_t rd32(uint8_t *ram, uint32_t addr) { return *reinterpret_cast<uint32_t *>(ram + addr); }
    void wr32(uint8_t *ram, uint32_t addr, uint32_t value) { *reinterpret_cast<uint32_t *>(ram + addr) = value; }

    void plantSentinels(uint8_t *ram, std::vector<uint32_t> &before)
    {
        before.clear();
        // A pattern that is clearly not a valid pointer: a checkerboard of small values.
        for (size_t i = 0; i < sizeof(kCodeWords) / sizeof(kCodeWords[0]); ++i)
        {
            const uint32_t v = 0x0BAD0000u | static_cast<uint32_t>(i + 1);
            wr32(ram, kCodeWords[i].addr, v);
            before.push_back(v);
        }
    }

    void checkSentinels(uint8_t *ram, const std::vector<uint32_t> &before, const std::string &after)
    {
        for (size_t i = 0; i < before.size(); ++i)
        {
            const uint32_t now = rd32(ram, kCodeWords[i].addr);
            if (now == before[i])
                continue;
            char message[256];
            std::snprintf(message, sizeof(message),
                          "%s: 0x%08X (%s) is 0x%08X, not 0x%08X (the game's code was overwritten)",
                          after.c_str(), kCodeWords[i].addr, kCodeWords[i].where, now, before[i]);
            expect(false, message);
        }
    }

    void testMpegStubsDoNotWriteTheGamesCode()
    {
        unsetenv("DISPLAY");
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        if (!expect(rt.initialize("mpeg-stub-code-test"), "PS2Runtime::initialize"))
            return;
        uint8_t *ram = rt.memory().getRDRAM();

        // The MPEG structure and its buffer: ordinary guest memory, away from the code.
        const uint32_t mpeg = 0x001F0000u;
        const uint32_t buffer = 0x001F8000u;
        const uint32_t size = 0x00002000u;

        std::vector<uint32_t> before;
        plantSentinels(ram, before);

        const int32_t created = call(rt, ps2_stubs::sceMpegCreate, mpeg, buffer, size);
        expect(created != 0, "sceMpegCreate returns a non-zero handle");
        // Its legitimate write: the buffer address (aligned) goes into the structure.
        expect(rd32(ram, mpeg + 0x40) == buffer, "sceMpegCreate still stores the buffer at +0x40");
        checkSentinels(ram, before, "after sceMpegCreate");

        call(rt, ps2_stubs::sceMpegClearRefBuff);
        checkSentinels(ram, before, "after sceMpegClearRefBuff");

        call(rt, ps2_stubs::sceMpegReset, mpeg);
        checkSentinels(ram, before, "after sceMpegReset");

        // And once more with the words at zero: the old clear only wrote when the word was non-zero, so
        // this catches a "fix" that simply moved the write behind that condition.
        std::vector<uint32_t> zeros;
        for (size_t i = 0; i < sizeof(kCodeWords) / sizeof(kCodeWords[0]); ++i)
        {
            wr32(ram, kCodeWords[i].addr, 0u);
            zeros.push_back(0u);
        }
        call(rt, ps2_stubs::sceMpegClearRefBuff);
        checkSentinels(ram, zeros, "after sceMpegClearRefBuff with zero words");
    }
} // namespace

int main()
{
    testMpegStubsDoNotWriteTheGamesCode();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "mpeg_stub_code_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("mpeg_stub_code_test: all checks passed\n");
    return 0;
}
