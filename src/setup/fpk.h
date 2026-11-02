// Eighting FPK packs (the game's MODULES/1000.FPK holds the IOP sound module): parse and decompress, as the
// developer tool tools/fpk.py does (task T-694). The pack is read, never executed.
//
// Compression is PRS-like, control bits read MSB first:
//   1            literal byte
//   0 0 s1 s0    short copy: length = s + 2, one offset byte (negative, -256..-1)
//   0 1          long copy: two bytes (hi, lo); offset = ((hi << 8 | lo) >> 3) - 0x2000,
//                length = (lo & 7) + 2, or the next byte + 1 when (lo & 7) == 0
// There is no end marker: the stream stops when the unpacked size is reached.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace don_setup {

struct FpkEntry {
    std::string name;
    uint32_t offset = 0;
    uint32_t packed = 0;
    uint32_t unpacked = 0;
};

struct FpkPack {
    bool big_endian = false;  // GameCube packs; the PS2 ones are little-endian
    std::vector<FpkEntry> entries;
};

// False when the bytes are not an FPK header (zero, count, header size 0x10, total size) in either byte order.
bool fpk_parse(const std::vector<uint8_t>& buf, FpkPack& out);

// False on a truncated stream or a back-reference before the start. A stream that ends early gives fewer bytes.
bool fpk_decompress(const uint8_t* src, size_t size, size_t out_size, std::vector<uint8_t>& out);

// The entry's bytes: stored as they are when packed == unpacked, else decompressed.
bool fpk_read_entry(const std::vector<uint8_t>& buf, const FpkEntry& entry, std::vector<uint8_t>& out);

}  // namespace don_setup
