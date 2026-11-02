// T-623b: extract_disc resumes per file, reads gz/zip in one forward pass, and maps failures to E08, E11, E19 and the
// disk-full fault. Images are synthetic and built here (a few KB); no game data is read.
#include "disc_manifest.h"
#include "faults.h"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
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
static void app16(Bytes& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
static void app32(Bytes& b, uint32_t v) { app16(b, v & 0xFFFF); app16(b, v >> 16); }
static void app(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }
static void app(Bytes& b, const std::string& x) { b.insert(b.end(), x.begin(), x.end()); }

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (auto& c : b) { x = x * 1664525u + 1013904223u; c = uint8_t(x >> 28); }  // low entropy: it compresses
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

// Same layout as test_setup_extract_manifest.cpp: PVD, root, subdirs, then the files in the order given.
static Bytes make_iso(const std::vector<F>& files) {
    std::vector<std::string> dirs;
    for (auto& f : files)
        if (!f.dir.empty() && std::find(dirs.begin(), dirs.end(), f.dir) == dirs.end()) dirs.push_back(f.dir);
    uint32_t root_lba = 18, next = root_lba + 1;
    std::vector<uint32_t> dir_lba, file_lba;
    for (size_t i = 0; i < dirs.size(); ++i) dir_lba.push_back(next++);
    for (auto& f : files) { file_lba.push_back(next); next += uint32_t((f.data.size() + 2047) / 2048); }
    Bytes iso(size_t(next) * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    iso[16 * 2048 + 6] = 1;
    Bytes tmp(2048);
    size_t o = 0;
    dir_record(tmp, o, "", root_lba, 2048, true, true, 0);
    std::memcpy(&iso[16 * 2048 + 156], tmp.data(), 34);
    iso[17 * 2048] = 255;
    std::memcpy(&iso[17 * 2048 + 1], "CD001", 5);
    auto write_dir = [&](uint32_t lba, const std::string& which) {
        Bytes d(2048);
        size_t off = 0;
        dir_record(d, off, "", lba, 2048, true, true, 0);
        dir_record(d, off, "", root_lba, 2048, true, true, 1);
        for (size_t i = 0; i < files.size(); ++i)
            if (files[i].dir == which)
                dir_record(d, off, files[i].name + ";1", file_lba[i], uint32_t(files[i].data.size()), false);
        if (which.empty())
            for (size_t i = 0; i < dirs.size(); ++i) dir_record(d, off, dirs[i], dir_lba[i], 2048, true);
        std::memcpy(&iso[size_t(lba) * 2048], d.data(), d.size());
    };
    write_dir(root_lba, "");
    for (size_t i = 0; i < dirs.size(); ++i) write_dir(dir_lba[i], dirs[i]);
    for (size_t i = 0; i < files.size(); ++i) std::memcpy(&iso[size_t(file_lba[i]) * 2048], files[i].data.data(), files[i].data.size());
    return iso;
}

static Bytes deflate_bytes(const Bytes& in, bool gzip) {
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

struct ZipEntry { std::string name; Bytes data; uint16_t method = 8, flags = 0; int bad_crc = 0; };

static Bytes make_zip(const std::vector<ZipEntry>& es, uint16_t eocd_disk = 0) {
    Bytes z, cd;
    for (auto& e : es) {
        Bytes body = e.method == 8 ? deflate_bytes(e.data, false) : e.data;
        uint32_t crc = uint32_t(crc32(0, e.data.data(), uInt(e.data.size()))) + uint32_t(e.bad_crc);
        uint32_t lo = uint32_t(z.size());
        app32(z, 0x04034b50); app16(z, 20); app16(z, e.flags); app16(z, e.method); app32(z, 0); app32(z, crc);
        app32(z, uint32_t(body.size())); app32(z, uint32_t(e.data.size())); app16(z, uint32_t(e.name.size())); app16(z, 0);
        app(z, e.name); app(z, body);
        app32(cd, 0x02014b50); app16(cd, 20); app16(cd, 20); app16(cd, e.flags); app16(cd, e.method); app32(cd, 0);
        app32(cd, crc); app32(cd, uint32_t(body.size())); app32(cd, uint32_t(e.data.size()));
        app16(cd, uint32_t(e.name.size())); app16(cd, 0); app16(cd, 0); app16(cd, 0); app16(cd, 0); app32(cd, 0);
        app32(cd, lo); app(cd, e.name);
    }
    uint32_t cd_off = uint32_t(z.size());
    app(z, cd);
    app32(z, 0x06054b50); app16(z, eocd_disk); app16(z, eocd_disk); app16(z, uint32_t(es.size()));
    app16(z, uint32_t(es.size())); app32(z, uint32_t(cd.size())); app32(z, cd_off); app16(z, 0);
    return z;
}

static void write_file(const fs::path& p, const Bytes& b) {
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
static void set_fault(const char* v) {
    if (v) setenv("DON_TEST_FAULT", v, 1); else unsetenv("DON_TEST_FAULT");
    faults_reset_for_tests();
}

struct Run { bool ok; ErrorId code; std::string error, detail; ExtractStats st; };
static Run run(const fs::path& img, const fs::path& dest, const DiscManifest& m) {
    Run r{false, ErrorId::None, "", "", {}};
    r.ok = extract_disc(img, dest, nullptr, r.error, m, &r.code, &r.detail, &r.st);
    return r;
}

int main() {
    fs::path root = fs::temp_directory_path() / "don_t623b_resume";
    fs::remove_all(root);
    fs::create_directories(root);
    set_fault(nullptr);

    // The manifest lists the files in reverse disc order; the files on disc are in a different order again.
    Bytes sys = pattern(57, 1), elf = pattern(6000, 2), irx = pattern(3000, 3), big = pattern(5 << 19, 4);  // 2.5 MiB: several read chunks, so a percent fault fires inside it
    std::vector<F> files = {{"", "SYSTEM.CNF", sys}, {"MODULES", "SDRDRV.IRX", irx}, {"", "SLPS_256.75", elf}, {"DATA", "BIG.BIN", big}};
    Bytes iso = make_iso(files);
    DiscManifest m{"test", "SLPS-00000", {entry("DATA/BIG.BIN", big), entry("SLPS_256.75", elf), entry("MODULES/SDRDRV.IRX", irx), entry("SYSTEM.CNF", sys)}};
    fs::path img = root / "disc.iso";
    write_file(img, iso);
    auto check_tree = [&](const fs::path& d) {
        return read_file(d / "SYSTEM.CNF") == sys && read_file(d / "SLPS_256.75") == elf &&
               read_file(d / "MODULES" / "SDRDRV.IRX") == irx && read_file(d / "DATA" / "BIG.BIN") == big;
    };

    // 1. resume: a second run skips verified files; a wrong size or wrong hash is copied again; a stale .part goes
    {
        fs::path d = root / "resume";
        Run a = run(img, d, m);
        CHECK(a.ok && a.st.copied == 4 && a.st.skipped == 0 && check_tree(d));
        Run b = run(img, d, m);
        CHECK(b.ok && b.st.copied == 0 && b.st.skipped == 4);
        Bytes wrong_hash = irx; wrong_hash[10] ^= 0xFF;
        write_file(d / "MODULES" / "SDRDRV.IRX", wrong_hash);
        write_file(d / "DATA" / "BIG.BIN", Bytes(100, 7));
        write_file(d / "SLPS_256.75.part", Bytes(10, 1));
        Run c = run(img, d, m);
        CHECK(c.ok && c.st.copied == 2 && c.st.skipped == 2 && check_tree(d));
        CHECK(!fs::exists(d / "SLPS_256.75.part"));
    }
    // 2. gz and zip (deflated and stored) images: same bytes, and the compressed stream only moves forward
    {
        write_file(root / "disc.iso.gz", deflate_bytes(iso, true));
        write_file(root / "disc.zip", make_zip({{"DON.ISO", iso}}));
        write_file(root / "disc_stored.zip", make_zip({{"don.iso", iso, 0}}));
        for (const char* n : {"disc.iso.gz", "disc.zip", "disc_stored.zip"}) {
            fs::path d = root / (std::string("out_") + n);
            Run r = run(root / n, d, m);
            CHECK(r.ok && check_tree(d));
            CHECK(r.st.restarts == 0);
            if (!r.ok) std::cerr << n << ": " << r.error << "\n";
        }
        // resume on a compressed image skips forward too
        fs::path d = root / "out_disc.iso.gz";
        Run r = run(root / "disc.iso.gz", d, m);
        CHECK(r.ok && r.st.skipped == 4 && r.st.restarts == 0);
    }
    // 3. eio@read:50% -> E19 with the percent read; no .part and no half file left behind
    {
        set_fault("eio@read:50%");
        fs::path d = root / "eio";
        Run r = run(img, d, m);
        CHECK(!r.ok && r.code == ErrorId::E19 && r.code == fault_error(FaultKind::Eio));
        int pct = std::atoi(r.detail.c_str());
        CHECK(pct >= 50 && pct < 100);
        size_t parts = 0;
        if (fs::exists(d)) for (auto& e : fs::recursive_directory_iterator(d)) if (e.path().extension() == ".part") ++parts;
        CHECK(parts == 0);
        set_fault(nullptr);
        Run again = run(img, d, m);  // resume after the read error
        CHECK(again.ok && again.st.skipped > 0 && check_tree(d));
    }
    // 4. enospc@copy -> the catalogue error of the fault (E31)
    {
        set_fault("enospc@copy:50%");
        Run r = run(img, root / "enospc", m);
        CHECK(!r.ok && r.code == ErrorId::E31 && r.code == fault_error(FaultKind::Enospc));
        CHECK(!fs::exists(root / "enospc" / "DATA" / "BIG.BIN.part"));
        set_fault(nullptr);
    }
    // 5. E08: gz with a broken CRC, a cut-off gz, a cut-off raw image, a zip whose CRC is wrong
    {
        Bytes gz = deflate_bytes(iso, true);
        Bytes badcrc = gz;
        badcrc[badcrc.size() - 6] ^= 0x55;  // inside the CRC32 of the trailer
        write_file(root / "badcrc.gz", badcrc);
        Run a = run(root / "badcrc.gz", root / "o1", m);
        CHECK(!a.ok && a.code == ErrorId::E08);

        Bytes cut(gz.begin(), gz.begin() + std::ptrdiff_t(gz.size() * 6 / 10));
        write_file(root / "cut.gz", cut);
        Run b = run(root / "cut.gz", root / "o2", m);
        CHECK(!b.ok && b.code == ErrorId::E08 && b.detail.find(" of ") != std::string::npos);

        Bytes rawcut(iso.begin(), iso.begin() + std::ptrdiff_t(iso.size() * 8 / 10));
        write_file(root / "cut.iso", rawcut);
        Run c = run(root / "cut.iso", root / "o3", m);
        CHECK(!c.ok && c.code == ErrorId::E08 && c.detail.find(" of ") != std::string::npos);

        write_file(root / "badcrc.zip", make_zip({{"DON.ISO", iso, 8, 0, 1}}));
        Run d = run(root / "badcrc.zip", root / "o4", m);
        CHECK(!d.ok && d.code == ErrorId::E08);
    }
    // 6. E11, each zip case with its own detail
    {
        struct Case { const char* name; Bytes zip; const char* detail; };
        std::vector<Case> cases = {
            {"noiso", make_zip({{"README.TXT", sys}}), "no-iso"},
            {"two", make_zip({{"A.ISO", iso}, {"B.ISO", iso}}), "several-isos"},
            {"pass", make_zip({{"DON.ISO", iso, 8, 1}}), "password"},
            {"method", make_zip({{"DON.ISO", iso, 12}}), "unsupported-method"},
            {"split", make_zip({{"DON.ISO", iso}}, 1), "split"},
        };
        for (auto& c : cases) {
            write_file(root / (std::string(c.name) + ".zip"), c.zip);
            Run r = run(root / (std::string(c.name) + ".zip"), root / ("e11_" + std::string(c.name)), m);
            CHECK(!r.ok && r.code == ErrorId::E11 && r.detail == c.detail);
            if (r.detail != c.detail) std::cerr << c.name << ": got '" << r.detail << "'\n";
        }
    }
    fs::remove_all(root);
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
