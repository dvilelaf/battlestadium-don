// Internal to the setup code: hashes and the ISO9660 reader behind disc_image.h and disc_manifest.h.
// Moved out of disc_image.cpp without changes (task T-622).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>

#include "disc_image.h"

namespace don_setup {

// ---------------------------------------------------------------- hashes

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

class Sha1 {
public:
    void update(const uint8_t* p, size_t n) {
        total_ += n;
        while (n) {
            size_t take = std::min(n, size_t(64) - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (fill_ != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        return hex(h_, 5);
    }
    static std::string hex(const uint32_t* h, int words) {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (int i = 0; i < words; ++i)
            for (int b = 28; b >= 0; b -= 4) s += d[(h[i] >> b) & 15];
        return s;
    }
private:
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

class Sha256 {
public:
    void update(const uint8_t* p, size_t n) {
        total_ += n;
        while (n) {
            size_t take = std::min(n, size_t(64) - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    std::string finish() {
        uint64_t bits = total_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (fill_ != 56) update(&z, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        update(len, 8);
        return Sha1::hex(h_, 8);
    }
private:
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

// ---------------------------------------------------------------- image access

inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint64_t le64(const uint8_t* p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }

// Random access to the bytes of the ISO, whether it is plain, gzip or a deflated/stored zip member.
// Compressed sources are read forward only; a backward seek restarts the stream.
enum class ReadFail { None, Eof, Io, Corrupt };  // the image ended early / the OS failed / gz or zip data is bad

class ImageReader {
public:
    ~ImageReader() { end_inflate(); }

    bool open(const std::filesystem::path& path, std::string& err);

    // Read exactly n bytes at offset; false when the image ends first.
    bool read_at(uint64_t off, void* buf, size_t n);
    // Why the last read_at or drain failed.
    ReadFail fail() const { return fail_; }
    // Reads a gz/zip stream to its end so its CRC and length are checked; true for a plain image.
    bool drain();
    bool compressed() const { return mode_ == Mode::Gzip || mode_ == Mode::Deflate; }
    // Bytes of the ISO known to exist so far (the whole size for plain and stored images).
    uint64_t have() const;
    // False when the OS reported no size for a plain image (tellg gave -1 or 0, as some devices do): reads are then
    // limited to bound_to(), which starts at 0, instead of to a size that is not known.
    bool size_known() const { return size_known_; }
    void bound_to(uint64_t bytes) { file_size_ = bytes; }
    // Times a compressed stream was started over after open; 0 means one forward pass (E21).
    uint64_t restarts() const { return restarts_; }
    // Set when open failed on a zip we refuse: no-iso, several-isos, password, unsupported-method or split (E11).
    const std::string& zip_problem() const { return zip_problem_; }

private:
    enum class Mode { Raw, Stored, Gzip, Deflate };

    bool open_zip(std::string& err);

    bool restart(std::string& err);
    void end_inflate();
    bool inflate_some(uint8_t* out, size_t n, bool tolerate_end = false);

    std::ifstream in_;
    uint64_t file_size_ = 0, base_ = 0, csize_ = 0, pos_ = 0, consumed_ = 0;
    Mode mode_ = Mode::Raw;
    ReadFail fail_ = ReadFail::None;
    std::string zip_problem_;
    uint64_t restarts_ = 0, zip_usize_ = 0;
    uint32_t zip_crc_ = 0, crc_ = 0;
    z_stream zs_{};
    bool zs_ready_ = false, done_ = false, size_known_ = true;
    std::array<uint8_t, 65536> in_buf_;
};

constexpr uint32_t kSector = 2048;

struct IsoFile {
    std::string path;  // relative, '/' separated
    uint64_t lba = 0, size = 0;
};

std::string clean_name(const uint8_t* p, size_t n);
bool safe_name(const std::string& s);
// Walks the directory tree. Returns false when it is not an ISO9660 image.
bool list_files(ImageReader& img, std::vector<IsoFile>& files);
// Reads one file of the root directory (name compared without case or ";1") into `out`: the descriptor, the root
// directory's extent and that file only; no subdirectory is read. False when it is not an ISO9660 image, the file is
// absent, empty or larger than max_size, or a read fails. Bounds the reads by the volume size when the image's size
// is unknown.
bool read_root_file(ImageReader& img, const char* name, uint64_t max_size, std::string& out);
bool ieq(const std::string& a, const std::string& b);
const IsoFile* find_file(const std::vector<IsoFile>& files, const char* name);

template <class Hasher>
bool hash_file(ImageReader& img, const IsoFile& f, Hasher& h, uint64_t& done, uint64_t total, const char* what,
               const ProgressFn& progress) {
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t off = 0; off < f.size;) {
        size_t n = size_t(std::min<uint64_t>(buf.size(), f.size - off));
        if (!img.read_at(f.lba * kSector + off, buf.data(), n)) return false;
        h.update(buf.data(), n);
        off += n;
        done += n;
        if (progress) progress(total ? double(done) / double(total) : 1.0, what);
    }
    return true;
}

}  // namespace don_setup
