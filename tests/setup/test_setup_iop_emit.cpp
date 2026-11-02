// FPK unpacking, IRX relocation and C++ emission of the IOP module (T-694), on SYNTHETIC bytes only: no game file.
// Every expected string is the output of the Python reference (tools/fpk.py, tools/iop_recomp/make_native.py and
// emit_adxi.py) on the same synthetic input, so the C++ port is held to the Python text byte for byte.
// Regenerate with work/t694/gen/gen_golden.py if the Python emitter changes on purpose.
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "fpk.h"
#include "iop_emit.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static std::vector<uint8_t> from_hex(const std::string& h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return v;
}

static const char* kPackLe = "000000000200000010000000880000004d4f44554c45532f6f746865722e697278000000000000000000000000000000000000007000000010000000100000004d4f44554c45532f4352495f414458492e4952580000000000000000000000000000000080000000080000000c000000101112131415161718191a1b1c1d1e1fca4142fe4380ffeb";
static const char* kPackBe = "000000000000000200000010000000884d4f44554c45532f6f746865722e697278000000000000000000000000000000000000000000007000000010000000104d4f44554c45532f4352495f414458492e4952580000000000000000000000000000000000000080000000080000000c101112131415161718191a1b1c1d1e1fca4142fe4380ffeb";
static const char* kRaw = "101112131415161718191a1b1c1d1e1f";
static const char* kDecoded = "414241424142434142434142";
static const char* kPacked = "ca4142fe4380ffeb";
static const char* kIrx = "000000000000000000000000000000000000000000000000000000000000000000020000000000000000000000002800030000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000043c108084240400000c00000000001000000000053cf07fa5240200000c0000000005000000040000000600000008000000040000001000000002000000140000000500000018000000060000001c0000000400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000000000000000000001000020000000000000000000000004000000000000000000000009000000000000000000000020010000380000000000000001000000040000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
static const char* kReloc6e000 = "0600043c1060842404b8010c0000000000f006000700053cf05fa52402b8010c";
static const char* kReloc18000 = "0100043c100084240460000c00000000009001000200053cf0ffa5240260000c";
static const char* kTiny = "f0ffbd270c00bfaf0000828c0000000004004010010042240db8010c00000000201842000c00bf8f1000bd270800e003000000001a0085001210000010180000030083a00800e003000000000000a28c211842000800e003000000000800e0030500002424e0060034e006000ce0060000000000";

static const char* kTinyBody = R"X(    switch (xdyn)
    {
    case 0x6e000u: goto L_6e000;
    case 0x6e00cu: goto L_6e00c;
    case 0x6e018u: goto L_6e018;
    case 0x6e020u: goto L_6e020;
    case 0x6e024u: goto L_6e024;
    case 0x6e034u: goto L_6e034;
    default: xpc = xdyn; goto fin;
    }
    // function 6e000-6e034
    L_6e000: if (__builtin_expect(left < 6u, 0)) { xpc = 0x6e000u; goto fin; } left -= 6u;
      r29 = r29 + 0xfffffff0u; /* 6e000: 27bdfff0 */
      { uint32_t a_ = r29 + 0xcu; uint32_t pa_ = a_ & 0x1FFFFFFFu; if ((a_ & 3u) || (pa_ >= kRamSize)) { xpc = 0x6e004u; left += 5u; goto fin; } if (pa_ - kCodeLo < kCodeSize) { g_stale = true; xpc = 0x6e004u; left += 5u; goto fin; } mem.write32(a_, r31); } /* 6e004: afbf000c */
      { uint32_t a_ = r4 + 0x0u; uint32_t pa_ = a_ & 0x1FFFFFFFu; if ((a_ & 3u) || (pa_ >= kRamSize)) { xpc = 0x6e008u; left += 4u; goto fin; } r2 = ld32(ram, pa_); } /* 6e008: 8c820000 */
      { /* 6e010: 10400004 */
      bool c_ = r2 == 0u;
      r2 = r2 + 0x1u; /* 6e014: 24420001 */
      if (c_) goto L_6e024;
      goto L_6e018;
      }
    L_6e00c: if (__builtin_expect(left < 3u, 0)) { xpc = 0x6e00cu; goto fin; } left -= 3u;
      { /* 6e010: 10400004 */
      bool c_ = r2 == 0u;
      r2 = r2 + 0x1u; /* 6e014: 24420001 */
      if (c_) goto L_6e024;
      goto L_6e018;
      }
    L_6e018: if (__builtin_expect(left < 2u, 0)) { xpc = 0x6e018u; goto fin; } left -= 2u;
      { /* 6e018: 0c01b80d */
      r31 = 0x6e020u;
      goto L_6e034;
      }
    L_6e020: if (__builtin_expect(left < 1u, 0)) { xpc = 0x6e020u; goto fin; } left -= 1u;
      { int32_t t_; if (__builtin_add_overflow((int32_t)r2, (int32_t)r2, &t_)) { xpc = 0x6e020u; left += 1u; goto fin; } r3 = (uint32_t)t_; } /* 6e020: 00421820 */
      goto L_6e024;
    L_6e024: if (__builtin_expect(left < 4u, 0)) { xpc = 0x6e024u; goto fin; } left -= 4u;
      { uint32_t a_ = r29 + 0xcu; uint32_t pa_ = a_ & 0x1FFFFFFFu; if ((a_ & 3u) || (pa_ >= kRamSize)) { xpc = 0x6e024u; left += 4u; goto fin; } r31 = ld32(ram, pa_); } /* 6e024: 8fbf000c */
      r29 = r29 + 0x10u; /* 6e028: 27bd0010 */
      { /* 6e02c: 03e00008 */
      uint32_t jt_ = r31;
      xdyn = jt_; goto dispatch;
      }
    // function 6e034-6e04c
    L_6e034: if (__builtin_expect(left < 6u, 0)) { xpc = 0x6e034u; goto fin; } left -= 6u;
      { int32_t n_ = (int32_t)r4, d_ = (int32_t)r5; if (d_ == 0 || (n_ == INT32_MIN && d_ == -1)) { xpc = 0x6e034u; left += 6u; goto fin; } lo_ = (uint32_t)(n_ / d_); hi_ = (uint32_t)(n_ % d_); } /* 6e034: 0085001a */
      r2 = lo_; /* 6e038: 00001012 */
      r3 = hi_; /* 6e03c: 00001810 */
      { uint32_t a_ = r4 + 0x3u; uint32_t pa_ = a_ & 0x1FFFFFFFu; if ((pa_ >= kRamSize)) { xpc = 0x6e040u; left += 3u; goto fin; } if (pa_ - kCodeLo < kCodeSize) { g_stale = true; xpc = 0x6e040u; left += 3u; goto fin; } mem.write8(a_, (uint8_t)r3); } /* 6e040: a0830003 */
      { /* 6e044: 03e00008 */
      uint32_t jt_ = r31;
      xdyn = jt_; goto dispatch;
      }
    // function 6e04c-6e05c: NOT TRANSLATED (load-delay hazard at 6e04c))X";
static const char* kTinyHash = R"X(constexpr uint64_t kCodeHash = 0x0d8e3c4237cf0faaull; // FNV-1a of the code words, for the host's guard)X";

int main() {
    // FPK: both byte orders, a stored and a compressed entry (short copy, long copy, literals).
    for (const char* hex : {kPackLe, kPackBe}) {
        FpkPack p;
        CHECK(fpk_parse(from_hex(hex), p));
        CHECK(p.big_endian == (hex == kPackBe) && p.entries.size() == 2);
        CHECK(p.entries.size() == 2 && p.entries[1].name == "MODULES/CRI_ADXI.IRX");
        std::vector<uint8_t> a, b;
        CHECK(p.entries.size() == 2 && fpk_read_entry(from_hex(hex), p.entries[0], a) && a == from_hex(kRaw));
        CHECK(p.entries.size() == 2 && fpk_read_entry(from_hex(hex), p.entries[1], b) && b == from_hex(kDecoded));
    }
    std::vector<uint8_t> out;
    const auto packed = from_hex(kPacked);
    CHECK(fpk_decompress(packed.data(), packed.size(), 12, out) && out == from_hex(kDecoded));
    CHECK(fpk_decompress(packed.data(), packed.size(), 5, out) && out.size() == 5);  // stops at the unpacked size
    const uint8_t bad_ref[] = {0x00, 0xFE};  // short copy with nothing to copy from
    CHECK(!fpk_decompress(bad_ref, sizeof bad_ref, 8, out));
    const uint8_t cut[] = {0x00};  // short copy without its offset byte
    CHECK(!fpk_decompress(cut, sizeof cut, 8, out));
    FpkPack none;
    CHECK(!fpk_parse(std::vector<uint8_t>(64, 0xAB), none) && !fpk_parse(std::vector<uint8_t>(8, 0), none));
    CHECK(!fpk_parse(std::vector<uint8_t>(64, 0), none));  // count 0

    // Relocation: R_MIPS_32, R_MIPS_26 and a HI16/LO16 pair with a carry, for two bases.
    CHECK(relocate_irx(from_hex(kIrx), 0x6E000).size() == kIopImageSize);
    const auto r1 = relocate_irx(from_hex(kIrx), 0x6E000), r2 = relocate_irx(from_hex(kIrx), 0x18000);
    CHECK(std::vector<uint8_t>(r1.begin(), r1.begin() + 0x20) == from_hex(kReloc6e000));
    CHECK(std::vector<uint8_t>(r2.begin(), r2.begin() + 0x20) == from_hex(kReloc18000));
    bool threw = false;
    try { relocate_irx(std::vector<uint8_t>(20, 0), 0x6E000); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    // A tiny module: a function with a load in front of a leader, a division, a store, a refused load-delay
    // hazard, an import stub and a jump table in the data after the code.
    IopEmitStats st;
    const std::string text = emit_adxi_native(from_hex(kTiny), 0x6E000, 0x6E000, 0x6E064, &st);
    const size_t a = text.find("    switch (xdyn)"), b = text.find("fin:\n");
    CHECK(a != std::string::npos && b != std::string::npos && text.substr(a, b - a) == std::string(kTinyBody) + "\n");
    CHECK(text.find(kTinyHash) != std::string::npos);
    CHECK(st.functions == (size_t)2 && st.refused == (size_t)1 && st.blocks == (size_t)6 &&
          st.translated_instructions == (size_t)22 && st.entries == (size_t)6);
    CHECK(text == emit_adxi_native(from_hex(kTiny), 0x6E000, 0x6E000, 0x6E064, nullptr));
    CHECK(text.rfind("}\n") == text.size() - 2);
    bool threw2 = false;
    try { emit_adxi_native(from_hex(kTiny), 0x6E000, 0x6E000, 0x7FFFF, nullptr); } catch (const std::runtime_error&) { threw2 = true; }
    CHECK(threw2);

    std::cerr << (g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
