#include "fpk.h"

#include <algorithm>

namespace don_setup {

namespace {

constexpr uint32_t kHeaderSize = 0x10;
constexpr uint32_t kEntrySize = 0x30;
constexpr size_t kNameSize = 0x20;
constexpr uint32_t kMaxEntries = 4096;
constexpr uint32_t kTotalSlack = 0x800;  // the pack's total size may exceed the file by this much

uint32_t get32(const std::vector<uint8_t>& b, size_t at, bool big) {
    const uint8_t* p = b.data() + at;
    return big ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
               : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
}

}  // namespace

bool fpk_parse(const std::vector<uint8_t>& buf, FpkPack& out) {
    if (buf.size() < kHeaderSize) return false;
    for (bool big : {false, true}) {
        const uint32_t zero = get32(buf, 0, big), count = get32(buf, 4, big), hsize = get32(buf, 8, big),
                       total = get32(buf, 12, big);
        if (zero != 0 || hsize != kHeaderSize || count == 0 || count >= kMaxEntries ||
            uint64_t(total) > uint64_t(buf.size()) + kTotalSlack)
            continue;
        if (uint64_t(hsize) + uint64_t(count) * kEntrySize > buf.size()) return false;
        out = FpkPack{};
        out.big_endian = big;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t o = hsize + size_t(i) * kEntrySize;
            FpkEntry e;
            for (size_t k = 0; k < kNameSize && buf[o + k] != 0; ++k) e.name.push_back(char(buf[o + k]));
            e.offset = get32(buf, o + kNameSize + 4, big);
            e.packed = get32(buf, o + kNameSize + 8, big);
            e.unpacked = get32(buf, o + kNameSize + 12, big);
            out.entries.push_back(std::move(e));
        }
        return true;
    }
    return false;
}

bool fpk_decompress(const uint8_t* src, size_t n, size_t out_size, std::vector<uint8_t>& out) {
    out.clear();
    size_t p = 0;
    unsigned bits = 0, nbits = 0;
    bool ok = true;
    auto bit = [&]() -> unsigned {
        if (nbits == 0) {
            if (p >= n) { ok = false; return 0; }
            bits = src[p++];
            nbits = 8;
        }
        --nbits;
        return (bits >> nbits) & 1;
    };
    while (out.size() < out_size && p < n) {
        if (bit()) {
            out.push_back(src[p++]);
            continue;
        }
        long off;
        unsigned len;
        if (bit()) {
            if (p + 2 > n) return false;
            const unsigned hi = src[p], lo = src[p + 1];
            p += 2;
            off = long(((hi << 8) | lo) >> 3) - 0x2000;
            len = lo & 7;
            if (len == 0) {
                if (p >= n) return false;
                len = unsigned(src[p++]) + 1;
            } else {
                len += 2;
            }
        } else {
            const unsigned hi2 = bit();
            const unsigned lo2 = bit();
            len = (hi2 << 1 | lo2) + 2;
            if (!ok || p >= n) return false;
            off = long(src[p++]) - 0x100;
        }
        if (!ok) return false;
        const long start = long(out.size()) + off;
        if (start < 0) return false;
        for (unsigned k = 0; k < len; ++k) out.push_back(out[size_t(start) + k]);
    }
    if (out.size() > out_size) out.resize(out_size);
    return true;
}

bool fpk_read_entry(const std::vector<uint8_t>& buf, const FpkEntry& e, std::vector<uint8_t>& out) {
    const size_t begin = std::min<size_t>(e.offset, buf.size());
    const size_t end = std::min<size_t>(size_t(e.offset) + e.packed, buf.size());
    if (e.packed == e.unpacked) {
        out.assign(buf.begin() + long(begin), buf.begin() + long(end));
        return true;
    }
    return fpk_decompress(buf.data() + begin, end - begin, e.unpacked, out);
}

}  // namespace don_setup
