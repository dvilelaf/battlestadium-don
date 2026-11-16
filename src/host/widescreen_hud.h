// SPDX-License-Identifier: GPL-3.0-or-later
// Widescreen (issue #2): keeps the round pieces of the fight HUD round when the picture is shown 16:9.
//
// During a fight the picture is shown 16:9 (gs_glue.cpp), so the game's 2D HUD, drawn for 4:3, is 4/3 wider than
// meant. The health bars may stay wide, but the portraits in their circles, the "1P"/"Com" badges and the corner
// gears look wrong stretched. They reach the GS as GIF packets of one textured triangle each (**Measured** on a
// fight frame captured with DON_GS_PACKETS: a tag with PRE set, PRIM = triangle fan with texture, NLOOP 3 and the
// registers ST, RGBAQ, XYZF2), two triangles per square, both with the square's bounding box. compress_square_hud
// finds the triangles whose bounding box is a square of 24 to 136 pixels in the top 100 lines of the picture and
// moves their x towards the square's centre by 3/4, so each piece keeps its place and its shape. The bars, the
// names and the 3D scene (triangle strips, or not square) are left as they are.
//
// Pure C++ on a byte buffer (no SDL, no GS library), so tests/host/widescreen_hud_test.cpp drives it directly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace don_widescreen
{

struct HudState
{
    // XYOFFSET_1 in GS units (1/16 pixel), as the game last set it through A+D; the game's own value until then.
    uint32_t offset_x = 1728u * 16u;
    uint32_t offset_y = 1824u * 16u;
};

namespace detail
{
inline uint64_t load64(const uint8_t *p)
{
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store64(uint8_t *p, uint64_t v) { std::memcpy(p, &v, sizeof v); }
} // namespace detail

// Rewrites, in place, the x of the HUD's square pieces in a buffer of GIF packets (`bytes` a multiple of 16).
// Returns how many triangles it changed. Packets it does not understand are skipped whole; it never reads past
// `bytes`.
inline int compress_square_hud(uint8_t *data, uint32_t bytes, HudState &state)
{
    using detail::load64;
    using detail::store64;
    constexpr uint32_t kQuad = 16;
    constexpr int kRegST = 0x2, kRegRGBAQ = 0x1, kRegXYZF2 = 0x4, kRegXYZ2 = 0x5, kRegAD = 0xE;
    constexpr uint64_t kXYOffset1 = 0x18;
    constexpr double kFactor = 0.75;                    // (4:3) / (16:9)
    constexpr double kMinSide = 24.0, kMaxSide = 136.0; // pixels
    constexpr double kMaxBottom = 100.0;                // pixels from the top of the picture
    int changed = 0;
    uint32_t q = 0;
    while (q + kQuad <= bytes)
    {
        const uint64_t lo = load64(data + q), hi = load64(data + q + 8);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFF);
        const bool pre = ((lo >> 46) & 1) != 0;
        const uint32_t prim = static_cast<uint32_t>((lo >> 47) & 0x7FF);
        const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3);
        uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        q += kQuad;
        if (flg == 1) // REGLIST: 64-bit words, padded to a quadword
        {
            const uint64_t words = uint64_t(nloop) * nreg;
            q += static_cast<uint32_t>((words + 1) / 2 * kQuad);
            continue;
        }
        if (flg != 0) // IMAGE (2) and the disabled mode (3): nloop quadwords of data
        {
            q += nloop * kQuad;
            continue;
        }
        const uint64_t body = uint64_t(nloop) * nreg * kQuad;
        if (q + body > bytes)
            break;
        // A+D writes of XYOFFSET_1 anywhere in a packed packet: the offset the coordinates are relative to.
        bool has_ad = false;
        for (uint32_t r = 0; r < nreg; ++r)
            has_ad = has_ad || ((hi >> (4 * r)) & 0xF) == kRegAD;
        if (has_ad)
        {
            for (uint32_t i = 0; i < nloop * nreg; ++i)
                if (((hi >> (4 * (i % nreg))) & 0xF) == kRegAD)
                {
                    const uint8_t *qw = data + q + i * kQuad;
                    if ((load64(qw + 8) & 0xFF) == kXYOffset1)
                    {
                        const uint64_t v = load64(qw);
                        state.offset_x = static_cast<uint32_t>(v & 0xFFFF);
                        state.offset_y = static_cast<uint32_t>((v >> 32) & 0xFFFF);
                    }
                }
        }
        const bool hud_triangle = pre && (prim & 7) == 5 && (prim & 0x10) != 0 && nloop == 3 && nreg == 3 &&
                                  ((hi >> 0) & 0xF) == kRegST && ((hi >> 4) & 0xF) == kRegRGBAQ &&
                                  (((hi >> 8) & 0xF) == kRegXYZF2 || ((hi >> 8) & 0xF) == kRegXYZ2);
        if (hud_triangle)
        {
            uint32_t xs[3], ys[3];
            for (int v = 0; v < 3; ++v)
            {
                const uint64_t xyz = load64(data + q + (v * 3 + 2) * kQuad);
                xs[v] = static_cast<uint32_t>(xyz & 0xFFFF);
                ys[v] = static_cast<uint32_t>((xyz >> 32) & 0xFFFF);
            }
            const uint32_t x0 = std::min(xs[0], std::min(xs[1], xs[2])), x1 = std::max(xs[0], std::max(xs[1], xs[2]));
            const uint32_t y0 = std::min(ys[0], std::min(ys[1], ys[2])), y1 = std::max(ys[0], std::max(ys[1], ys[2]));
            const double w = (x1 - x0) / 16.0, h = (y1 - y0) / 16.0;
            const double bottom = (static_cast<double>(y1) - state.offset_y) / 16.0;
            if (std::fabs(w - h) < 1.0 && w >= kMinSide && w <= kMaxSide && bottom <= kMaxBottom)
            {
                const double centre = (x0 + x1) / 2.0;
                for (int v = 0; v < 3; ++v)
                {
                    uint8_t *qw = data + q + (v * 3 + 2) * kQuad;
                    const uint64_t xyz = load64(qw);
                    const double x = centre + ((xyz & 0xFFFF) - centre) * kFactor;
                    const uint64_t nx = static_cast<uint64_t>(std::lround(x)) & 0xFFFF;
                    store64(qw, (xyz & ~uint64_t(0xFFFF)) | nx);
                }
                ++changed;
            }
        }
        q += static_cast<uint32_t>(body);
    }
    return changed;
}

} // namespace don_widescreen
