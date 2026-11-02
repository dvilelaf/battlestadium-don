// extract_disc against an allow-list manifest: copy ok, hash mismatch is E18 with the name, extras are not copied.
// The image is synthetic (a few bytes per file); no game data is read.
#include "disc_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

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
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
static Bytes read_file(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(i), {});
}

static ManifestFile entry(const std::string& path, const Bytes& data) {
    return {path, data.size(), sha256_hex(data.data(), data.size())};
}

int main() {
    fs::path root = fs::temp_directory_path() / "don_t622_manifest";
    fs::remove_all(root);
    fs::create_directories(root);

    Bytes sys = pattern(57, 1), elf = pattern(5000, 2), irx = pattern(3000, 3), extra = pattern(100, 4);
    std::vector<F> files = {{"", "SYSTEM.CNF", sys}, {"", "SLPS_256.75", elf}, {"MODULES", "SDRDRV.IRX", irx},
                            {"", "SAVE.BIN", extra}};
    fs::path img = root / "disc.iso";
    Bytes iso = make_iso(files);
    write_file(img, iso);

    DiscManifest good{"test", "SLPS-00000", {entry("SYSTEM.CNF", sys), entry("SLPS_256.75", elf), entry("MODULES/SDRDRV.IRX", irx)}};

    // 1. copy ok, and the file outside the allow-list is not copied
    {
        fs::path dest = root / "ok";
        std::string err;
        ErrorId code = ErrorId::E22;
        double last = -1;
        CHECK(extract_disc(img, dest, [&](double f, const std::string&) { last = f; }, err, good, &code));
        CHECK(err.empty());
        CHECK(code == ErrorId::E22);
        CHECK(last == 1.0);
        CHECK(read_file(dest / "SYSTEM.CNF") == sys);
        CHECK(read_file(dest / "SLPS_256.75") == elf);
        CHECK(read_file(dest / "MODULES" / "SDRDRV.IRX") == irx);
        CHECK(!fs::exists(dest / "SAVE.BIN"));
        size_t n = 0;
        for (auto& e : fs::recursive_directory_iterator(dest)) if (e.is_regular_file()) ++n;
        CHECK(n == 3);
    }
    // 2. hash mismatch: E18 naming the file, nothing left behind for it
    {
        DiscManifest bad = good;
        bad.files[2].sha256 = sha256_hex("other", 5);
        fs::path dest = root / "bad";
        std::string err;
        ErrorId code = ErrorId::E22;
        CHECK(!extract_disc(img, dest, nullptr, err, bad, &code));
        CHECK(code == ErrorId::E18);
        CHECK(err.find("MODULES/SDRDRV.IRX") != std::string::npos);
        CHECK(!fs::exists(dest / "MODULES" / "SDRDRV.IRX"));
        CHECK(!fs::exists(dest / "MODULES" / "SDRDRV.IRX.part"));
    }
    // 3. wrong size and a listed file that is absent are E18 too, before anything is written
    {
        DiscManifest sz = good;
        sz.files[1].size += 1;
        std::string err;
        ErrorId code = ErrorId::E22;
        CHECK(!extract_disc(img, root / "sz", nullptr, err, sz, &code));
        CHECK(code == ErrorId::E18 && err.find("SLPS_256.75") != std::string::npos);
        CHECK(!fs::exists(root / "sz" / "SYSTEM.CNF"));

        DiscManifest gone = good;
        gone.files.push_back(entry("DATA.AFS", extra));
        err.clear(); code = ErrorId::E22;
        CHECK(!extract_disc(img, root / "gone", nullptr, err, gone, &code));
        CHECK(code == ErrorId::E18 && err.find("DATA.AFS") != std::string::npos);
    }
    // 4. the manifest text round-trips through the parser
    {
        std::string text = "# c\n[[variant]]\nid = \"v\"\nserial = \"SLPS-25675\"\n\n[[variant.file]]\npath = \"MODULES/A.IRX\"\n"
                           "size = 12\nsha256 = \"ab\"\n\n[[variant]]\nid = \"w\"\nserial = \"SLPS-25675\"\n";
        std::vector<DiscManifest> m;
        std::string err;
        CHECK(parse_disc_manifest(text, m, err));
        CHECK(m.size() == 2 && m[0].variant_id == "v" && m[0].files.size() == 1);
        CHECK(m[0].files[0].path == "MODULES/A.IRX" && m[0].files[0].size == 12 && m[0].files[0].sha256 == "ab");
        CHECK(m[1].variant_id == "w" && m[1].files.empty());
        CHECK(!parse_disc_manifest("[[variant]]\nsize = x\n", m, err));
    }
    // 6. folder input: same allow-list, size, hash and error ids as the image
    {
        fs::path src = root / "folder";
        write_file(src / "system.cnf", sys);  // other letter case than the allow-list
        write_file(src / "SLPS_256.75", elf);
        write_file(src / "MODULES" / "SDRDRV.IRX", irx);
        write_file(src / "SAVE.BIN", extra);
        fs::path dest = root / "fok";
        std::string err;
        ErrorId code = ErrorId::E22;
        double last = -1;
        CHECK(extract_disc(src, dest, [&](double f, const std::string&) { last = f; }, err, good, &code));
        CHECK(last == 1.0);
        CHECK(read_file(dest / "SYSTEM.CNF") == sys && read_file(dest / "MODULES/SDRDRV.IRX") == irx);
        CHECK(!fs::exists(dest / "SAVE.BIN"));
        ExtractStats stats;
        CHECK(extract_disc(src, dest, nullptr, err, good, &code, nullptr, &stats));
        CHECK(stats.copied == 0 && stats.skipped == 3);

        // wrong hash (same size): E18 with the name, nothing left behind
        Bytes bad = elf;
        bad[10] ^= 0xFF;
        write_file(src / "SLPS_256.75", bad);
        fs::path d2 = root / "fbad";
        CHECK(!extract_disc(src, d2, nullptr, err, good, &code));
        CHECK(code == ErrorId::E18 && err.find("SLPS_256.75") != std::string::npos);
        CHECK(!fs::exists(d2 / "SLPS_256.75") && !fs::exists(d2 / "SLPS_256.75.part"));
        // wrong size
        write_file(src / "SLPS_256.75", pattern(4999, 2));
        CHECK(!extract_disc(src, root / "fsize", nullptr, err, good, &code));
        CHECK(code == ErrorId::E18 && err.find("SLPS_256.75") != std::string::npos);
        // missing file
        fs::remove(src / "SLPS_256.75");
        CHECK(!extract_disc(src, root / "fmiss", nullptr, err, good, &code));
        CHECK(code == ErrorId::E18 && err.find("SLPS_256.75") != std::string::npos);
    }
    // 6. T-693: config/disc_manifest.toml lists the three editions, each with the same program file (one SHA-256,
    //    so one module serves them all) and a DATA.AFS of its own.
    {
        std::ifstream f(fs::path(__FILE__).parent_path() / ".." / ".." / "config" / "disc_manifest.toml");
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<DiscManifest> all;
        std::string perr;
        CHECK(parse_disc_manifest(text, all, perr));
        CHECK(all.size() == 3);
        std::string elf, afs[3];
        for (size_t i = 0; i < all.size(); ++i) {
            CHECK(all[i].variant_id == (i == 0 ? "jp" : i == 1 ? "en-jpvoice" : "en-envoice"));
            for (const ManifestFile& mf : all[i].files) {
                if (mf.path == "SLPS_256.75") { CHECK(elf.empty() || elf == mf.sha256); elf = mf.sha256; }
                if (mf.path == "DATA.AFS") afs[i] = mf.sha256;
            }
        }
        CHECK(elf == "994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1");
        CHECK(!afs[0].empty() && afs[0] != afs[1] && afs[1] != afs[2] && afs[0] != afs[2]);
    }
    fs::remove_all(root);
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
