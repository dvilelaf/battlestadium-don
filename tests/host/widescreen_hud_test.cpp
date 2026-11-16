// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #2: compress_square_hud (src/host/widescreen_hud.h) narrows the fight HUD's square pieces by 3/4 around their
// centre, and leaves alone the bars (not square), 3D triangle strips, pieces low on the screen and other packets.
#include "../../src/host/widescreen_hud.h"

#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
void expect(bool ok, const char *what)
{
    if (!ok)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void put64(std::vector<uint8_t> &b, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint64_t get64(const std::vector<uint8_t> &b, size_t at)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}

// One GIF tag: PRE, the given PRIM, NLOOP 3, registers ST, RGBAQ, XYZF2, then three vertices (pixels relative to the
// game's offset 1728, 1824).
void triangle(std::vector<uint8_t> &b, uint32_t prim, const double xy[3][2])
{
    const uint64_t lo = 3ull | (1ull << 15) | (1ull << 46) | (uint64_t(prim) << 47) | (0ull << 58) | (3ull << 60);
    const uint64_t hi = 0x2ull | (0x1ull << 4) | (0x4ull << 8);
    put64(b, lo);
    put64(b, hi);
    for (int v = 0; v < 3; ++v)
    {
        put64(b, 0); // ST
        put64(b, 0);
        put64(b, 0x80808080u); // RGBAQ
        put64(b, 0);
        const uint64_t x = static_cast<uint64_t>((1728.0 + xy[v][0]) * 16.0);
        const uint64_t y = static_cast<uint64_t>((1824.0 + xy[v][1]) * 16.0);
        put64(b, x | (y << 32));
        put64(b, 0);
    }
}

double vertex_x(const std::vector<uint8_t> &b, size_t tag, int v)
{
    return (get64(b, tag + 16 + (v * 3 + 2) * 16) & 0xFFFF) / 16.0 - 1728.0;
}
} // namespace

int main()
{
    constexpr uint32_t kTexturedFan = 0x55; // triangle fan, texture, alpha blend: the HUD's PRIM
    std::vector<uint8_t> b;
    const double portrait[3][2] = {{28.9375, 8.4375}, {28.9375, 72.4375}, {92.9375, 8.4375}}; // 64 x 64
    const double bar[3][2] = {{32.9375, 26.9375}, {32.9375, 59.9375}, {606.3125, 26.9375}};  // 573 x 33
    const double low[3][2] = {{300.0, 380.0}, {300.0, 444.0}, {364.0, 380.0}};               // square, bottom
    const double tiny[3][2] = {{10.0, 10.0}, {10.0, 18.0}, {18.0, 10.0}};                    // 8 x 8
    triangle(b, kTexturedFan, portrait); // tag at 0
    triangle(b, kTexturedFan, bar);      // tag at 160
    triangle(b, kTexturedFan, low);      // tag at 320
    triangle(b, kTexturedFan, tiny);     // tag at 480
    triangle(b, 0x54, portrait);         // tag at 640: a triangle strip, as the 3D scene draws
    std::vector<uint8_t> original = b;

    don_widescreen::HudState state;
    const int changed = don_widescreen::compress_square_hud(b.data(), static_cast<uint32_t>(b.size()), state);
    expect(changed == 1, "only the portrait triangle changes");
    // The portrait's centre is x = 60.9375; its 64-pixel width becomes 48.
    expect(vertex_x(b, 0, 0) > 36.9 && vertex_x(b, 0, 0) < 37.0, "the left edge moves in by 8 pixels");
    expect(vertex_x(b, 0, 2) > 84.9 && vertex_x(b, 0, 2) < 85.0, "the right edge moves in by 8 pixels");
    bool rest_same = true;
    for (size_t i = 160; i < b.size(); ++i)
        rest_same = rest_same && b[i] == original[i];
    expect(rest_same, "the bar, the low square, the tiny square and the strip are untouched");
    // The y coordinates of the portrait are untouched too.
    bool y_same = true;
    for (int v = 0; v < 3; ++v)
        y_same = y_same && (get64(b, 16 + (v * 3 + 2) * 16) >> 16) == (get64(original, 16 + (v * 3 + 2) * 16) >> 16);
    expect(y_same, "only x changes");

    // A truncated buffer is not read past its end.
    std::vector<uint8_t> cut(original.begin(), original.begin() + 100);
    cut.resize(96);
    don_widescreen::compress_square_hud(cut.data(), static_cast<uint32_t>(cut.size()), state);

    if (failures == 0)
        std::printf("widescreen_hud_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
