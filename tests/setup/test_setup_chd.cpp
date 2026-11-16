// Issue #6: CHD disc images. A synthetic ISO9660 image is wrapped in a CHD (version 5, uncompressed hunks, written
// here, so no chdman and no game file are needed) as chdman's createdvd makes it ("DVD " metadata) and as createcd
// makes it (MODE2_RAW frames of 2448 bytes, a "CHT2" track). identify_disc must take both like the plain ISO and
// extract_disc must give back the same bytes; a CHD that needs a parent, and a file that only has the CHD tag, are E15.
#include "disc_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                                \
    do {                                                                                        \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put32le(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void put32be(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i)); }
static void put64be(Bytes& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = uint8_t(v >> (56 - 8 * i)); }

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (auto& c : b) { x = x * 1664525u + 1013904223u; c = uint8_t(x >> 24); }
    return b;
}

static void dir_record(Bytes& d, size_t& off, const std::string& name, uint32_t lba, uint32_t size, bool is_dir,
                       bool special = false, uint8_t sp = 0) {
    const size_t nl = special ? 1 : name.size();
    const size_t len = 33 + nl + ((nl % 2) ? 0 : 1);
    d[off] = uint8_t(len);
    put32le(d, off + 2, lba);
    put32le(d, off + 10, size);
    d[off + 25] = is_dir ? 2 : 0;
    d[off + 32] = uint8_t(nl);
    if (special) d[off + 33] = sp; else std::memcpy(&d[off + 33], name.data(), nl);
    off += len;
}

struct F { std::string name; Bytes data; };

// The files in the root: 16 PVD, 17 terminator, 18 root directory, then the files.
static Bytes make_iso(const std::vector<F>& files) {
    const uint32_t root_lba = 18;
    uint32_t next = root_lba + 1;
    std::vector<uint32_t> lba;
    for (auto& f : files) { lba.push_back(next); next += uint32_t((f.data.size() + 2047) / 2048); }
    Bytes iso(size_t(next) * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    iso[16 * 2048 + 6] = 1;
    put32le(iso, 16 * 2048 + 80, next);
    {
        Bytes tmp(64);
        size_t o = 0;
        dir_record(tmp, o, "", root_lba, 2048, true, true, 0);
        std::memcpy(&iso[16 * 2048 + 156], tmp.data(), 34);
    }
    iso[17 * 2048] = 255;
    std::memcpy(&iso[17 * 2048 + 1], "CD001", 5);
    Bytes root(2048, 0);
    size_t o = 0;
    dir_record(root, o, "", root_lba, 2048, true, true, 0);
    dir_record(root, o, "", root_lba, 2048, true, true, 1);
    for (size_t i = 0; i < files.size(); ++i)
        dir_record(root, o, files[i].name + ";1", lba[i], uint32_t(files[i].data.size()), false);
    std::memcpy(&iso[root_lba * 2048], root.data(), 2048);
    for (size_t i = 0; i < files.size(); ++i)
        std::memcpy(&iso[size_t(lba[i]) * 2048], files[i].data.data(), files[i].data.size());
    return iso;
}

// A version 5 CHD with uncompressed hunks: header, metadata, map, then each hunk at a multiple of hunk_bytes (the
// map stores offset / hunk_bytes; 0 would mean "all zero").
static Bytes make_chd(const Bytes& logical, uint32_t hunk_bytes, uint32_t unit_bytes, const char tag[4],
                      const std::string& meta, bool with_parent = false) {
    const uint64_t hunks = (logical.size() + hunk_bytes - 1) / hunk_bytes;
    const uint64_t meta_at = 124, map_at = meta_at + 16 + meta.size() + 1;
    const uint64_t data_at = (map_at + hunks * 4 + hunk_bytes - 1) / hunk_bytes * hunk_bytes;
    Bytes c(size_t(data_at + hunks * hunk_bytes), 0);
    std::memcpy(&c[0], "MComprHD", 8);
    put32be(c, 8, 124);
    put32be(c, 12, 5);
    put64be(c, 32, logical.size());
    put64be(c, 40, map_at);
    put64be(c, 48, meta_at);
    put32be(c, 56, hunk_bytes);
    put32be(c, 60, unit_bytes);
    if (with_parent) c[104] = 0x5a;  // a parent's SHA-1: this CHD holds only the differences
    std::memcpy(&c[meta_at], tag, 4);
    put32be(c, meta_at + 4, uint32_t(meta.size() + 1));
    std::memcpy(&c[meta_at + 16], meta.c_str(), meta.size() + 1);
    for (uint64_t h = 0; h < hunks; ++h) {
        put32be(c, size_t(map_at + h * 4), uint32_t(data_at / hunk_bytes + h));
        const size_t from = size_t(h * hunk_bytes), n = std::min<size_t>(hunk_bytes, logical.size() - from);
        std::memcpy(&c[size_t(data_at + h * hunk_bytes)], &logical[from], n);
    }
    return c;
}

// The ISO as chdman createcd stores a MODE2 disc: one 2448-byte frame per sector (2352 of sector, the 2048 bytes of
// data 24 bytes in, then 96 of subcode).
static Bytes as_cd_frames(const Bytes& iso) {
    const size_t sectors = iso.size() / 2048;
    Bytes out(sectors * 2448, 0);
    for (size_t s = 0; s < sectors; ++s) {
        uint8_t* f = &out[s * 2448];
        f[0] = 0;
        std::memset(f + 1, 0xff, 10);
        f[15] = 2;  // mode 2
        std::memcpy(f + 24, &iso[s * 2048], 2048);
    }
    return out;
}

static void write_file(const fs::path& p, const Bytes& b) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static Bytes read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path root = fs::temp_directory_path() / "don_test_setup_chd";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    const Bytes elf = pattern(70000, 1), afs = pattern(300001, 2), cnf_data = {'B', 'O', 'O', 'T', '2', '\n'};
    const Bytes iso = make_iso({{"SYSTEM.CNF", cnf_data}, {"SLPS_256.75", elf}, {"DATA.AFS", afs}});
    DiscTable table;
    table.elf_sha256 = sha256_hex(elf.data(), elf.size());
    table.variants = {{"test", "Test edition", sha1_hex(afs.data(), afs.size())}};

    struct Case { const char* file; Bytes chd; };
    const Case cases[] = {
        {"dvd.chd", make_chd(iso, 4096, 2048, "DVD ", "")},
        {"dvd-odd-hunk.chd", make_chd(iso, 3 * 2048, 2048, "DVD ", "")},
        {"cd.chd", make_chd(as_cd_frames(iso), 8 * 2448, 2448, "CHT2",
                            "TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:" + std::to_string(iso.size() / 2048) +
                                " PREGAP:0 PGTYPE:MODE2_RAW PGSUB:RW POSTGAP:0")},
    };
    for (const Case& c : cases) {
        write_file(root / c.file, c.chd);
        const DiscIdentity id = identify_disc(root / c.file, table, nullptr);
        CHECK(id.known && id.variant_id == "test");
        if (!id.known) std::cerr << c.file << ": error " << int(id.error) << " " << id.detail << "\n";
        const fs::path out = root / (std::string(c.file) + "-out");
        std::string error;
        CHECK(extract_disc(root / c.file, out, nullptr, error));
        CHECK(read_file(out / "SLPS_256.75") == elf);
        CHECK(read_file(out / "DATA.AFS") == afs);
    }

    // A CHD that holds only differences from a parent CHD: refused with a reason, not read as zeros.
    write_file(root / "child.chd", make_chd(iso, 4096, 2048, "DVD ", "", true));
    {
        const DiscIdentity id = identify_disc(root / "child.chd", table, nullptr);
        CHECK(!id.known && id.error == ErrorId::E15 && id.detail.find("parent") != std::string::npos);
    }
    // Only the tag: E15 (it was E10, "convert it first", before CHDs were read).
    {
        Bytes b(64, 0);
        std::memcpy(b.data(), "MComprHD", 8);
        write_file(root / "tag-only.chd", b);
        const DiscIdentity id = identify_disc(root / "tag-only.chd", table, nullptr);
        CHECK(!id.known && id.error == ErrorId::E15 && id.detail.rfind("CHD", 0) == 0);
    }

    fs::remove_all(root, ec);
    if (g_fail == 0) std::cout << "test_setup_chd: all checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
