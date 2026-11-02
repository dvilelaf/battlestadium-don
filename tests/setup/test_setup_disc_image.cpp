// Synthetic ISO9660 images (plain, gzip, zip stored/deflate) against identify_disc and extract_disc.
#include "disc_image.h"

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (auto& c : b) { x = x * 1664525u + 1013904223u; c = uint8_t(x >> 24); }
    return b;
}

struct F { std::string dir, name; Bytes data; };

static void dir_record(Bytes& d, size_t& off, const std::string& name, uint32_t lba, uint32_t size, bool is_dir,
                       bool special = false, uint8_t sp = 0) {
    size_t nl = special ? 1 : name.size();
    size_t len = 33 + nl + ((nl % 2) ? 0 : 1);
    if ((off % 2048) + len > 2048) off = (off / 2048 + 1) * 2048;
    if (d.size() < off + len) d.resize((off + len + 2047) / 2048 * 2048);
    d[off] = uint8_t(len);
    put32(d, off + 2, lba);
    put32(d, off + 10, size);
    d[off + 25] = is_dir ? 2 : 0;
    d[off + 32] = uint8_t(nl);
    if (special) d[off + 33] = sp; else std::memcpy(&d[off + 33], name.data(), nl);
    off += len;
}

// Root holds the files with dir == ""; each other dir name becomes a subfolder of the root.
static Bytes make_iso(const std::vector<F>& files) {
    std::vector<std::string> dirs;
    for (auto& f : files)
        if (!f.dir.empty() && std::find(dirs.begin(), dirs.end(), f.dir) == dirs.end()) dirs.push_back(f.dir);
    // layout: 16 PVD, 17 terminator, 18 root (4 sectors, to exercise many records), then subdirs, then files
    uint32_t root_lba = 18, root_sectors = 2;
    std::vector<uint32_t> dir_lba;
    uint32_t next = root_lba + root_sectors;
    for (size_t i = 0; i < dirs.size(); ++i) dir_lba.push_back(next++);
    std::vector<uint32_t> file_lba;
    for (auto& f : files) { file_lba.push_back(next); next += uint32_t((f.data.size() + 2047) / 2048); }
    Bytes iso(size_t(next) * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    iso[16 * 2048 + 6] = 1;
    {
        Bytes tmp(2048);
        size_t o = 0;
        dir_record(tmp, o, "", root_lba, root_sectors * 2048, true, true, 0);
        std::memcpy(&iso[16 * 2048 + 156], tmp.data(), 34);
    }
    iso[17 * 2048] = 255;
    std::memcpy(&iso[17 * 2048 + 1], "CD001", 5);
    auto write_dir = [&](uint32_t lba, uint32_t self, const std::string& which) {
        Bytes d(2048);
        size_t off = 0;
        dir_record(d, off, "", self, 2048, true, true, 0);
        dir_record(d, off, "", root_lba, 2048, true, true, 1);
        for (size_t i = 0; i < files.size(); ++i)
            if (files[i].dir == which)
                dir_record(d, off, files[i].name + ";1", file_lba[i], uint32_t(files[i].data.size()), false);
        if (which.empty())
            for (size_t i = 0; i < dirs.size(); ++i) dir_record(d, off, dirs[i], dir_lba[i], 2048, true);
        CHECK(d.size() <= (which.empty() ? root_sectors : 1u) * 2048);
        std::memcpy(&iso[size_t(lba) * 2048], d.data(), d.size());
    };
    write_dir(root_lba, root_lba, "");
    for (size_t i = 0; i < dirs.size(); ++i) write_dir(dir_lba[i], dir_lba[i], dirs[i]);
    for (size_t i = 0; i < files.size(); ++i) std::memcpy(&iso[size_t(file_lba[i]) * 2048], files[i].data.data(), files[i].data.size());
    return iso;
}

static void write_file(const fs::path& p, const Bytes& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static Bytes read_file(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(i), {});
}

static Bytes deflate_raw(const Bytes& in, bool gzip) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, gzip ? 16 + MAX_WBITS : -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, uLong(in.size())) + 64);
    z.next_in = const_cast<uint8_t*>(in.data());
    z.avail_in = uInt(in.size());
    z.next_out = out.data();
    z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(out.size() - z.avail_out);
    deflateEnd(&z);
    return out;
}

static void p16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void p32(Bytes& b, uint32_t v) { p16(b, uint16_t(v)); p16(b, uint16_t(v >> 16)); }

static Bytes make_zip(const std::string& name, const Bytes& iso, bool deflated) {
    Bytes comp = deflated ? deflate_raw(iso, false) : iso;
    uint32_t crc = uint32_t(crc32(0, iso.data(), uInt(iso.size())));
    Bytes z;
    // a decoy entry before the iso
    std::string decoy = "readme.txt";
    p32(z, 0x04034b50); p16(z, 20); p16(z, 0); p16(z, 0); p16(z, 0); p16(z, 0); p32(z, 0); p32(z, 4); p32(z, 4);
    p16(z, uint16_t(decoy.size())); p16(z, 0);
    z.insert(z.end(), decoy.begin(), decoy.end());
    z.insert(z.end(), {'h', 'i', '!', '\n'});
    uint32_t lo = uint32_t(z.size());
    p32(z, 0x04034b50); p16(z, 20); p16(z, 0); p16(z, deflated ? 8 : 0); p16(z, 0); p16(z, 0); p32(z, crc);
    p32(z, uint32_t(comp.size())); p32(z, uint32_t(iso.size())); p16(z, uint16_t(name.size())); p16(z, 5);
    z.insert(z.end(), name.begin(), name.end());
    z.insert(z.end(), {1, 2, 3, 4, 5});
    z.insert(z.end(), comp.begin(), comp.end());
    uint32_t cd = uint32_t(z.size());
    p32(z, 0x02014b50); p16(z, 20); p16(z, 20); p16(z, 0); p16(z, 0); p16(z, 0); p16(z, 0); p32(z, 0); p32(z, 4); p32(z, 4);
    p16(z, uint16_t(decoy.size())); p16(z, 0); p16(z, 0); p16(z, 0); p16(z, 0); p32(z, 0); p32(z, 0);
    z.insert(z.end(), decoy.begin(), decoy.end());
    p32(z, 0x02014b50); p16(z, 20); p16(z, 20); p16(z, 0); p16(z, deflated ? 8 : 0); p16(z, 0); p16(z, 0); p32(z, crc);
    p32(z, uint32_t(comp.size())); p32(z, uint32_t(iso.size())); p16(z, uint16_t(name.size())); p16(z, 0); p16(z, 0);
    p16(z, 0); p16(z, 0); p32(z, 0); p32(z, lo);
    z.insert(z.end(), name.begin(), name.end());
    uint32_t cdsize = uint32_t(z.size()) - cd;
    p32(z, 0x06054b50); p16(z, 0); p16(z, 0); p16(z, 2); p16(z, 2); p32(z, cdsize); p32(z, cd); p16(z, 0);
    return z;
}

int main() {
    // known vectors
    CHECK(sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha1_hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    {
        std::string m(1000, 'a');
        CHECK(sha1_hex(m.data(), m.size()) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    }

    fs::path root = fs::temp_directory_path() / "don_setup_test_disc_image";
    fs::remove_all(root);
    fs::create_directories(root);

    Bytes slps = pattern(70000, 1), afs = pattern(300000, 2);
    std::vector<F> files = {
        {"", "SLPS_256.75", slps},
        {"", "DATA.AFS", afs},
        {"", "EMPTY.BIN", {}},
        {"MOVIE", "OP.PSS", pattern(5000, 3)},
        {"MOVIE", "ED.PSS", pattern(2048, 4)},
        {"SND", "A.BIN", pattern(1, 5)},
    };
    for (int i = 0; i < 40; ++i) files.push_back({"", "FILE" + std::to_string(i) + ".DAT", pattern(100 + i, 100 + i)});
    Bytes iso = make_iso(files);

    DiscTable table{sha256_hex(slps.data(), slps.size()),
                    {{"a", "Variant A", "0000000000000000000000000000000000000000"},
                     {"b", "Variant B", sha1_hex(afs.data(), afs.size())}}};

    fs::path plain = root / "disc.iso", gz = root / "disc.iso.gz", zs = root / "stored.zip", zd = root / "deflate.zip";
    write_file(plain, iso);
    write_file(gz, deflate_raw(iso, true));
    write_file(zs, make_zip("DISC.ISO", iso, false));
    write_file(zd, make_zip("disc.iso", iso, true));

    int calls = 0;
    double last = -1;
    bool monotonic = true;
    ProgressFn prog = [&](double f, const std::string&) {
        ++calls;
        if (f < last) monotonic = false;
        last = f;
    };

    int n = 0;
    for (auto& img : {plain, gz, zs, zd}) {
        std::cerr << "image " << img.filename() << "\n";
        calls = 0; last = -1; monotonic = true;
        DiscIdentity id = identify_disc(img, table, prog);
        CHECK(id.known);
        CHECK(id.variant_id == "b");
        CHECK(id.description == "Variant B");
        CHECK(calls > 0 && monotonic && last == 1.0);

        fs::path dest = root / ("out" + std::to_string(n++));
        calls = 0; last = -1; monotonic = true;
        std::string err;
        CHECK(extract_disc(img, dest, prog, err));
        CHECK(err.empty());
        CHECK(monotonic && last == 1.0 && calls > 0);
        for (auto& f : files) {
            fs::path p = dest / f.dir / f.name;
            CHECK(fs::exists(p));
            CHECK(read_file(p) == f.data);
        }
        bool part = false;
        for (auto& e : fs::recursive_directory_iterator(dest)) if (e.path().extension() == ".part") part = true;
        CHECK(!part);
    }

    // wrong table: the ELF hash differs / the AFS hash is unknown
    {
        DiscTable bad = table;
        bad.elf_sha256 = std::string(64, '0');
        DiscIdentity id = identify_disc(plain, bad, nullptr);
        CHECK(!id.known);
        CHECK(id.reason == DiscVerdict::Modified);
        bad = table;
        bad.variants[1].data_afs_sha1 = std::string(40, '1');
        id = identify_disc(gz, bad, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::UnknownData);
    }
    // a valid ISO without the game files
    {
        Bytes other = make_iso({{"", "README.TXT", pattern(10, 9)}});
        write_file(root / "other.iso", other);
        DiscIdentity id = identify_disc(root / "other.iso", table, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::NotDon);
    }
    // T-707: a small file that is not a disc image is E15; E07 only for 0 bytes or a disc-image extension
    {
        write_file(root / "notes.txt", Bytes(24));
        write_file(root / "zero.txt", Bytes());
        write_file(root / "small.iso", Bytes(24));
        CHECK(identify_disc(root / "notes.txt", table, nullptr).error == ErrorId::E15);
        CHECK(identify_disc(root / "zero.txt", table, nullptr).error == ErrorId::E07);
        CHECK(identify_disc(root / "small.iso", table, nullptr).error == ErrorId::E07);
    }
    // not an image at all, missing file, truncated gz, zip without iso
    {
        write_file(root / "junk.iso", pattern(100000, 7));
        DiscIdentity id = identify_disc(root / "junk.iso", table, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::Unreadable);
        id = identify_disc(root / "missing.iso", table, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::Unreadable);
        Bytes g = read_file(gz);
        g.resize(g.size() / 2);
        write_file(root / "trunc.iso.gz", g);
        id = identify_disc(root / "trunc.iso.gz", table, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::Unreadable);
        Bytes z = make_zip("notes.txt", pattern(10, 8), false);
        write_file(root / "noiso.zip", z);
        id = identify_disc(root / "noiso.zip", table, nullptr);
        CHECK(!id.known && id.reason == DiscVerdict::Unreadable);
        std::string err;
        CHECK(!extract_disc(root / "junk.iso", root / "outjunk", nullptr, err));
        CHECK(!err.empty());
        // truncated plain image: extraction fails and leaves no .part file
        Bytes cut(iso.begin(), iso.begin() + long(iso.size() - 150000));
        write_file(root / "cut.iso", cut);
        CHECK(!extract_disc(root / "cut.iso", root / "outcut", nullptr, err));
        bool part = false;
        if (fs::exists(root / "outcut"))
            for (auto& e : fs::recursive_directory_iterator(root / "outcut")) if (e.path().extension() == ".part") part = true;
        CHECK(!part);
    }
    // the built-in table is the one of config/discs.toml
    CHECK(builtin_disc_table().variants.size() == 3);
    CHECK(builtin_disc_table().elf_sha256.size() == 64);

    fs::remove_all(root);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "all ok\n";
    return 0;
}
