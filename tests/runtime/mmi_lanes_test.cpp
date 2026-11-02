// SPDX-License-Identifier: GPL-3.0-or-later
// T-202 part 1: the lanes of the runtime's MMI macros, against the console's definitions.
//
// Why this test exists: the port draws the 3D world wrong while every 2D element is right (journal 008),
// and journal 013 traced it to the game's own vector data: at the title, 1080 sixteen-byte blocks that
// the console writes as `a a a a` (a = 0x3F009A34) come out of the port as `a' 0 a' <not written>`.
// The writer is FUN_0025b8f0 (0x25b8f0-0x25be44, the per-object constant block the game uploads to VU1
// address 0x3FB with `UNPACK V4-32`); generated/FUN_0025b8f0_0x25b8f0.cpp emits its lane juggling as
//
//   0x25bcac: pexew $v0, $v0        -> SET_GPR_VEC(ctx, 2, PS2_PEXEW(GPR_VEC(ctx, 2)));
//   0x25bcb0: pexew $v1, $v1        -> SET_GPR_VEC(ctx, 3, PS2_PEXEW(GPR_VEC(ctx, 3)));
//   0x25bcc4: ppacw $v0, $v0, $v1   -> SET_GPR_VEC(ctx, 2, PS2_PPACW(GPR_VEC(ctx, 2), GPR_VEC(ctx, 3)));
//
// so the four lanes of the constant are decided by PS2_PEXEW and PS2_PPACW. The console's definitions
// (PCSX2 2.8.2, third_party/pcsx2/pcsx2/MMI.cpp, the reference of this port) are, on the four 32-bit
// lanes of a general register:
//
//   PEXEW(rt):  rd[0]=rt[2]  rd[1]=rt[1]  rd[2]=rt[0]  rd[3]=rt[3]     (it swaps lanes 0 and 2)
//   PPACW(rs,rt): rd[0]=rt[0] rd[1]=rt[2] rd[2]=rs[0] rd[3]=rs[2]
//
// The test checks every MMI macro the game's generated code actually emits (the list comes from
// `grep -rho 'PS2_P[A-Z0-9]*(' generated/`, 18 names) against a reference written from PCSX2's code,
// with a nonzero value in every lane and in every halfword/byte, and then the exact sequence above, whose
// four lanes must all be equal (the console's block is `a a a a` for every object).
//
// It FAILS while PS2_PEXEW has its lanes in the wrong order, and passes once it has them as above.
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void check(bool ok, const char *what, const uint32_t *got, const uint32_t *want)
    {
        ++g_checks;
        if (ok)
            return;
        ++g_failures;
        std::fprintf(stderr, "FAIL %-34s got  %08x %08x %08x %08x\n", what, got[0], got[1], got[2], got[3]);
        std::fprintf(stderr, "%-39s want %08x %08x %08x %08x\n", "", want[0], want[1], want[2], want[3]);
    }

    struct Lanes
    {
        uint32_t u[4];
    };

    Lanes from(__m128i v)
    {
        Lanes out;
        std::memcpy(out.u, &v, sizeof(out.u));
        return out;
    }

    __m128i to(const Lanes &l)
    {
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(l.u));
    }

    // The inputs: every lane, halfword and byte different, and the top bit set in some of them (the
    // MMI instructions are integer operations: the reference below is exact).
    const Lanes kA{{0x80010203u, 0x40050607u, 0x0008090Au, 0xFFFF0B0Cu}};
    const Lanes kB{{0x01020304u, 0x05060708u, 0x090A0B0Cu, 0x0D0E0F10u}};

    // ---- the reference: PCSX2 2.8.2, third_party/pcsx2/pcsx2/MMI.cpp -------------------------------

    Lanes ref_pexew(const Lanes &rt)
    {
        return {{rt.u[2], rt.u[1], rt.u[0], rt.u[3]}};
    }
    Lanes ref_pexeh(const Lanes &rt) // the same swap, one halfword down: US[0]=US[2], US[1]=US[1], ...
    {
        Lanes rd{};
        uint16_t s[8];
        std::memcpy(s, rt.u, sizeof s);
        uint16_t d[8] = {};
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        d[4] = s[6];
        d[5] = s[5];
        d[6] = s[4];
        d[7] = s[7];
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    Lanes ref_ppacw(const Lanes &rs, const Lanes &rt)
    {
        return {{rt.u[0], rt.u[2], rs.u[0], rs.u[2]}};
    }
    Lanes ref_pcpyld(const Lanes &rs, const Lanes &rt)
    {
        return {{rt.u[0], rt.u[1], rs.u[0], rs.u[1]}}; // UD[0] = Rt.UD[0], UD[1] = Rs.UD[0]
    }
    Lanes ref_pextlw(const Lanes &rs, const Lanes &rt)
    {
        return {{rt.u[0], rs.u[0], rt.u[1], rs.u[1]}};
    }
    Lanes ref_pextuw(const Lanes &rs, const Lanes &rt)
    {
        return {{rt.u[2], rs.u[2], rt.u[3], rs.u[3]}};
    }
    Lanes ref_pand(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = rs.u[i] & rt.u[i];
        return rd;
    }
    Lanes ref_por(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = rs.u[i] | rt.u[i];
        return rd;
    }
    Lanes ref_pxor(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = rs.u[i] ^ rt.u[i];
        return rd;
    }
    Lanes ref_pnor(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = ~(rs.u[i] | rt.u[i]);
        return rd;
    }
    Lanes ref_halfword_op(const Lanes &rs, const Lanes &rt, int op) // 0 add, 1 sub, 2 max, 3 min
    {
        Lanes rd{};
        uint16_t a[8];
        std::memcpy(a, rs.u, sizeof a);
        uint16_t b[8];
        std::memcpy(b, rt.u, sizeof b);
        uint16_t d[8] = {};
        for (int i = 0; i < 8; ++i)
        {
            const int16_t sa = static_cast<int16_t>(a[i]);
            const int16_t sb = static_cast<int16_t>(b[i]);
            switch (op)
            {
            case 0: d[i] = static_cast<uint16_t>(sa + sb); break;
            case 1: d[i] = static_cast<uint16_t>(sa - sb); break;
            case 2: d[i] = static_cast<uint16_t>(sa > sb ? sa : sb); break;
            default: d[i] = static_cast<uint16_t>(sa < sb ? sa : sb); break;
            }
        }
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    Lanes ref_byte_op(const Lanes &rs, const Lanes &rt, int op) // 0 add, 1 sub
    {
        Lanes rd{};
        uint8_t a[16];
        std::memcpy(a, rs.u, sizeof a);
        uint8_t b[16];
        std::memcpy(b, rt.u, sizeof b);
        uint8_t d[16] = {};
        for (int i = 0; i < 16; ++i)
            d[i] = static_cast<uint8_t>(op == 0 ? a[i] + b[i] : a[i] - b[i]);
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    Lanes ref_word_sub(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = rs.u[i] - rt.u[i];
        return rd;
    }
    Lanes ref_pcgtw(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        for (int i = 0; i < 4; ++i)
            rd.u[i] = static_cast<int32_t>(rs.u[i]) > static_cast<int32_t>(rt.u[i]) ? 0xFFFFFFFFu : 0;
        return rd;
    }
    Lanes ref_pcgth(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        int16_t a[8];
        std::memcpy(a, rs.u, sizeof a);
        int16_t b[8];
        std::memcpy(b, rt.u, sizeof b);
        uint16_t d[8] = {};
        for (int i = 0; i < 8; ++i)
            d[i] = a[i] > b[i] ? 0xFFFF : 0;
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    // PEXTLB / PEXTUB interleave the *bytes* of rt and rs, starting from the low/high end.
    Lanes ref_pextlb(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        uint8_t a[16];
        std::memcpy(a, rs.u, sizeof a);
        uint8_t b[16];
        std::memcpy(b, rt.u, sizeof b);
        uint8_t d[16] = {};
        for (int i = 0; i < 8; ++i)
        {
            d[2 * i] = b[i];
            d[2 * i + 1] = a[i];
        }
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    Lanes ref_pextub(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        uint8_t a[16];
        std::memcpy(a, rs.u, sizeof a);
        uint8_t b[16];
        std::memcpy(b, rt.u, sizeof b);
        uint8_t d[16] = {};
        for (int i = 0; i < 8; ++i)
        {
            d[2 * i] = b[8 + i];
            d[2 * i + 1] = a[8 + i];
        }
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }
    Lanes ref_ppacb(const Lanes &rs, const Lanes &rt)
    {
        Lanes rd{};
        uint8_t a[16];
        std::memcpy(a, rs.u, sizeof a);
        uint8_t b[16];
        std::memcpy(b, rt.u, sizeof b);
        uint8_t d[16] = {};
        for (int i = 0; i < 8; ++i)
        {
            d[i] = b[2 * i];
            d[8 + i] = a[2 * i];
        }
        std::memcpy(rd.u, d, sizeof d);
        return rd;
    }

    void same(const char *what, const Lanes &got, const Lanes &want)
    {
        check(std::memcmp(got.u, want.u, sizeof(got.u)) == 0, what, got.u, want.u);
    }

    Lanes macro_of(__m128i v)
    {
        return from(v);
    }
} // namespace

int main()
{
    // The macros the game's generated code emits (see the file comment), each against PCSX2's definition.
    same("PS2_PCPYLD", macro_of(PS2_PCPYLD(to(kA), to(kB))), ref_pcpyld(kA, kB));
    same("PS2_PPACW", macro_of(PS2_PPACW(to(kA), to(kB))), ref_ppacw(kA, kB));
    same("PS2_PPACB", macro_of(PS2_PPACB(to(kA), to(kB))), ref_ppacb(kA, kB));
    same("PS2_PEXEW", macro_of(PS2_PEXEW(to(kA))), ref_pexew(kA));
    same("PS2_PEXEH", macro_of(PS2_PEXEH(to(kA))), ref_pexeh(kA));
    same("PS2_PEXTLW", macro_of(PS2_PEXTLW(to(kA), to(kB))), ref_pextlw(kA, kB));
    same("PS2_PEXTUW", macro_of(PS2_PEXTUW(to(kA), to(kB))), ref_pextuw(kA, kB));
    same("PS2_PEXTLB", macro_of(PS2_PEXTLB(to(kA), to(kB))), ref_pextlb(kA, kB));
    same("PS2_PEXTUB", macro_of(PS2_PEXTUB(to(kA), to(kB))), ref_pextub(kA, kB));
    same("PS2_PAND", macro_of(PS2_PAND(to(kA), to(kB))), ref_pand(kA, kB));
    same("PS2_POR", macro_of(PS2_POR(to(kA), to(kB))), ref_por(kA, kB));
    same("PS2_PXOR", macro_of(PS2_PXOR(to(kA), to(kB))), ref_pxor(kA, kB));
    same("PS2_PNOR", macro_of(PS2_PNOR(to(kA), to(kB))), ref_pnor(kA, kB));
    same("PS2_PADDH", macro_of(PS2_PADDH(to(kA), to(kB))), ref_halfword_op(kA, kB, 0));
    same("PS2_PSUBH", macro_of(PS2_PSUBH(to(kA), to(kB))), ref_halfword_op(kA, kB, 1));
    same("PS2_PMAXH", macro_of(PS2_PMAXH(to(kA), to(kB))), ref_halfword_op(kA, kB, 2));
    same("PS2_PMINH", macro_of(PS2_PMINH(to(kA), to(kB))), ref_halfword_op(kA, kB, 3));
    same("PS2_PSUBB", macro_of(PS2_PSUBB(to(kA), to(kB))), ref_byte_op(kA, kB, 1));
    same("PS2_PSUBW", macro_of(PS2_PSUBW(to(kA), to(kB))), ref_word_sub(kA, kB));
    same("PS2_PCGTH", macro_of(PS2_PCGTH(to(kA), to(kB))), ref_pcgth(kA, kB));
    same("PS2_PCGTW", macro_of(PS2_PCGTW(to(kA), to(kB))), ref_pcgtw(kA, kB));

    // The exact construct of FUN_0025b8f0, 0x25bc9c..0x25bcc8, on the 128-bit view of the registers.
    // mfc1 puts the float in lane 0 and the sign extension in lane 1, and leaves lanes 2 and 3 as they
    // were: the game's display-list buffer, where the UNPACK code word 0x6C0103FB sits (it wrote it a few
    // instructions earlier at 0x25bc4c). The game then uses pexew twice to move the lane-0 values to lane
    // 2 and ppacw to gather x, y, z, w:
    //
    //   f6 = b47 * c ; v0 = mfc1(f6)     -> [w, 0, code, old3]
    //   v0 = pexew(v0)                   -> [code, 0, w, old3]      (reference: lane0 <- lane2)
    //   f6 = b46 * c ; v0 = mfc1(f6)     -> [z, 0, w, old3]         (mfc1 leaves lanes 2,3)
    //   v0 = ppacw(v0, v1)               -> [x, y, z, w]            (rt[0], rt[2], rs[0], rs[2])
    //
    // With the reference semantics the four lanes are `a a a a` for an object whose four bytes are equal
    // (what the console holds). With PS2_PEXEW as it is today they are `a 0 a 0x6C0103FB`, which is
    // exactly what work/ramdump/t114-title.bin holds at 0x5aacf0: the port's 3D constant has its y and w
    // lanes wrong. This is about the lanes, not the value: the 1-ULP difference is T-206's business.
    const uint32_t a = 0x3F009A34u;   // the value the console's block holds (0.502353)
    const uint32_t stale = 0x6C0103FBu; // the UNPACK V4-32 code word still in the register
    Lanes w0{{a, 0, stale, 0}}, y0{{a, 0, stale, 0}};
    Lanes ref_w = ref_pexew(w0), ref_y = ref_pexew(y0);
    Lanes ref_z{{a, 0, ref_w.u[2], ref_w.u[3]}}, ref_x{{a, 0, ref_y.u[2], ref_y.u[3]}};
    Lanes ref_block = ref_ppacw(ref_z, ref_x);
    Lanes got_w = from(PS2_PEXEW(to(w0))), got_y = from(PS2_PEXEW(to(y0)));
    Lanes got_z{{a, 0, got_w.u[2], got_w.u[3]}}, got_x{{a, 0, got_y.u[2], got_y.u[3]}};
    Lanes got_block = from(PS2_PPACW(to(got_z), to(got_x)));
    same("FUN_0025b8f0 pexew (w)", got_w, ref_w);
    same("FUN_0025b8f0 pexew (y)", got_y, ref_y);
    same("FUN_0025b8f0 ppacw", got_block, ref_block);
    check(ref_block.u[0] == a && ref_block.u[1] == a && ref_block.u[2] == a && ref_block.u[3] == a,
          "the reference gives the console's `a a a a`", ref_block.u, ref_block.u);
    check(got_block.u[0] == a && got_block.u[1] == a && got_block.u[2] == a && got_block.u[3] == a,
          "the port's constant block is `a a a a`", got_block.u, ref_block.u);

    std::printf("mmi_lanes_test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
