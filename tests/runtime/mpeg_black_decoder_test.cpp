// SPDX-License-Identifier: GPL-3.0-or-later
// T-305 patch 0070: with no FFmpeg the MPEG stub behaves as a decoder that outputs black pictures.
//
// The game's player (CRI Sofdec) sends the movie to the IPU over DMA channel 4 and then asks sceMpegGetPicture for
// pictures; it does not look at their content. A runtime built without FFmpeg has no decoder, so no picture was ever
// made and the call waited for ever (journal 034, follow-up 15). This test starts channel 4 with a synthetic stream
// (a 64x32 sequence header, three picture start codes with no picture data, a sequence end; nothing from the game)
// and checks that:
//
//   * the first sceMpegGetPicture answers at once with the stream's size (64x32) and counter 0;
//   * the picture is black (the buffer, filled with 0xEE beforehand, is overwritten with RGB 0, alpha 0x80);
//   * the channel is consumed: STR is clear and D_STAT bit 4 is set.
//
// Only for a runtime built WITHOUT FFmpeg (the CMake list adds it when DON_RT_FFMPEG is OFF).
// Run it with DISPLAY unset (the test unsets it).

#include "host_test_support.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_stubs
{
    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

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

    void call(PS2Runtime &rt, Handler fn, uint32_t a0, uint32_t a1, uint32_t a2)
    {
        R5900Context ctx;
        ctx.r[29] = _mm_set_epi32(0, 0, 0, 0x00200000);
        ctx.r[4] = _mm_set_epi32(0, 0, 0, static_cast<int>(a0));
        ctx.r[5] = _mm_set_epi32(0, 0, 0, static_cast<int>(a1));
        ctx.r[6] = _mm_set_epi32(0, 0, 0, static_cast<int>(a2));
        ctx.r[7] = _mm_set_epi32(0, 0, 0, 0);
        fn(rt.memory().getRDRAM(), &ctx, &rt);
    }

    uint32_t rd32(uint8_t *ram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, ram + addr, sizeof(value));
        return value;
    }

    std::vector<uint8_t> makeStream()
    {
        // Sequence header 64x32, frame rate code 3; then three pictures (start code, a few bytes); sequence end.
        std::vector<uint8_t> s = {0x00, 0x00, 0x01, 0xB3, 0x04, 0x00, 0x20, 0x13, 0xFF, 0xFF, 0xE0, 0x18};
        for (int i = 0; i < 3; ++i)
        {
            const uint8_t picture[] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x0F, 0xFF, 0xF8, 0x11, 0x22, 0x33, 0x44};
            s.insert(s.end(), picture, picture + sizeof(picture));
        }
        const uint8_t end[] = {0x00, 0x00, 0x01, 0xB7};
        s.insert(s.end(), end, end + sizeof(end));
        return s;
    }
}

int main()
{
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    PS2Runtime rt;
    rt.setHostOwnsWindow(true);
    expect(rt.initialize("mpeg-black-decoder-test"), "initialize()");
    uint8_t *ram = rt.memory().getRDRAM();

    const uint32_t mpeg = 0x001F0000u;
    const uint32_t buffer = 0x001F8000u;
    const uint32_t chain = 0x00300000u;
    const uint32_t image = 0x00400000u;
    call(rt, ps2_stubs::sceMpegCreate, mpeg, buffer, 0x2000u);

    const std::vector<uint8_t> stream = makeStream();
    const uint32_t qwc = static_cast<uint32_t>((stream.size() + 15u) / 16u);
    std::memset(ram + chain, 0, 16u + qwc * 16u);
    const uint64_t tag = static_cast<uint64_t>(qwc) | (7ull << 28); // end tag with its payload
    std::memcpy(ram + chain, &tag, sizeof(tag));
    std::memcpy(ram + chain + 16u, stream.data(), stream.size());
    rt.memory().write32(0x1000B430u, chain); // D4_TADR
    rt.memory().write32(0x1000B400u, 0x145u); // D4_CHCR: STR, source chain, from memory

    std::memset(ram + image, 0xEE, 64u * 32u * 4u);
    call(rt, ps2_stubs::sceMpegGetPicture, mpeg, image, 0u);
    const uint32_t width = rd32(ram, mpeg + 0u);
    const uint32_t height = rd32(ram, mpeg + 4u);
    const uint32_t count = rd32(ram, mpeg + 8u);
    std::printf("[picture 0] %ux%u, counter %u\n", width, height, count);
    expect(width == 64u && height == 32u, "the picture has the stream's size 64x32, got " + std::to_string(width) + "x" +
                                              std::to_string(height));
    expect(count == 0u, "the picture counter is 0, got " + std::to_string(count));
    bool black = true;
    for (uint32_t i = 0; i < 64u * 32u; ++i)
    {
        const uint8_t *p = ram + image + i * 4u;
        black = black && p[0] == 0u && p[1] == 0u && p[2] == 0u && p[3] == 0x80u;
    }
    expect(black, "the picture is black (RGB 0, alpha 0x80) over its whole buffer");

    const uint32_t chcr = rt.memory().read32(0x1000B400u);
    const uint32_t dstat = rt.memory().read32(0x1000E010u);
    std::printf("[end] D4_CHCR=0x%X D_STAT=0x%X\n", chcr, dstat);
    expect((chcr & 0x100u) == 0u, "the chain was consumed: channel 4 STR is clear");
    expect((dstat & (1u << 4)) != 0u, "the chain was consumed: D_STAT bit 4 (channel 4) is set");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "mpeg_black_decoder_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("mpeg_black_decoder_test: all checks passed\n");
    return 0;
}
