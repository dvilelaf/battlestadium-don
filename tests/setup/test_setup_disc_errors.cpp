// identify_disc names what is wrong with a file as an error-catalogue ID (T-623a). Synthetic files only.
#include "disc_image.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#ifdef __unix__
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }

struct F { std::string name; Bytes data; };

static void dir_record(Bytes& d, size_t& off, const std::string& name, uint32_t lba, uint32_t size, bool special = false,
                       uint8_t sp = 0) {
    size_t nl = special ? 1 : name.size();
    size_t len = 33 + nl + ((nl % 2) ? 0 : 1);
    d[off] = uint8_t(len);
    put32(d, off + 2, lba);
    put32(d, off + 10, size);
    d[off + 25] = special ? 2 : 0;
    d[off + 32] = uint8_t(nl);
    if (special) d[off + 33] = sp; else std::memcpy(&d[off + 33], name.data(), nl);
    off += len;
}

// A flat ISO9660 image: PVD at 16, root directory at 18, the files after it. The volume size field stays 0.
static Bytes make_iso(const std::vector<F>& files) {
    uint32_t next = 19;
    std::vector<uint32_t> lba;
    for (auto& f : files) { lba.push_back(next); next += uint32_t((f.data.size() + 2047) / 2048); }
    Bytes iso(size_t(next) * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    Bytes root(2048);
    size_t o = 0;
    dir_record(root, o, "", 18, 2048, true, 0);
    std::memcpy(&iso[16 * 2048 + 156], root.data(), 34);
    dir_record(root, o, "", 18, 2048, true, 1);
    for (size_t i = 0; i < files.size(); ++i) dir_record(root, o, files[i].name + ";1", lba[i], uint32_t(files[i].data.size()));
    std::memcpy(&iso[18 * 2048], root.data(), 2048);
    for (size_t i = 0; i < files.size(); ++i) std::memcpy(&iso[size_t(lba[i]) * 2048], files[i].data.data(), files[i].data.size());
    return iso;
}

static Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Bytes cnf(const std::string& boot) { return text("BOOT2 = cdrom0:\\" + boot + ";1\r\nVER = 1.00\r\n"); }

static void write_file(const fs::path& p, const Bytes& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static DiscIdentity run(const fs::path& p) { return identify_disc(p, nullptr); }

static std::string between(const std::string& s, const std::string& open, const std::string& close) {
    size_t a = s.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    return s.substr(a, s.find(close, a) - a);
}

// The built-in table must equal config/discs.toml (only the fields the table has).
static void check_table() {
    fs::path toml = fs::path(__FILE__).parent_path() / ".." / ".." / "config" / "discs.toml";
    std::ifstream in(toml);
    CHECK(bool(in));
    std::string sha256;
    std::vector<DiscVariant> v;
    bool in_elf = false;
    for (std::string line; std::getline(in, line);) {
        if (line == "[elf]") in_elf = true;
        else if (line == "[[variant]]") { in_elf = false; v.emplace_back(); }
        else if (in_elf && line.rfind("sha256", 0) == 0) sha256 = between(line, "\"", "\"");
        else if (!v.empty() && line.rfind("id =", 0) == 0) v.back().id = between(line, "\"", "\"");
        else if (!v.empty() && line.rfind("description =", 0) == 0) v.back().description = between(line, "\"", "\"");
        else if (!v.empty() && line.rfind("data_afs_sha1 =", 0) == 0) v.back().data_afs_sha1 = between(line, "\"", "\"");
    }
    const DiscTable& t = builtin_disc_table();
    CHECK(t.elf_sha256 == sha256);
    CHECK(t.variants.size() == v.size());
    for (size_t i = 0; i < v.size() && i < t.variants.size(); ++i) {
        CHECK(t.variants[i].id == v[i].id);
        CHECK(t.variants[i].description == v[i].description);
        CHECK(t.variants[i].data_afs_sha1 == v[i].data_afs_sha1);
    }
}

int main() {
    fs::path root = fs::temp_directory_path() / ("don_setup_disc_errors_" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root);
    const Bytes elf_data = text("not the real program file");
    const Bytes afs_data = text("not the real data file");

    // E05: no read permission (a root process can read anything, so it is skipped there)
#ifdef __unix__
    if (geteuid() != 0) {
        write_file(root / "locked.iso", Bytes(100));
        chmod((root / "locked.iso").c_str(), 0);
        DiscIdentity id = run(root / "locked.iso");
        CHECK(!id.known && id.error == ErrorId::E05);
        chmod((root / "locked.iso").c_str(), 0600);
    }
#endif
    // E06: the file is gone
    {
        DiscIdentity id = run(root / "gone.iso");
        CHECK(!id.known && id.error == ErrorId::E06 && id.reason == DiscVerdict::Unreadable);
    }
    // E07: empty and tiny; E09: the same with a download suffix
    write_file(root / "empty.iso", Bytes());
    write_file(root / "tiny.iso", Bytes(100));
    CHECK(run(root / "empty.iso").error == ErrorId::E07);
    CHECK(run(root / "tiny.iso").error == ErrorId::E07);
    write_file(root / "tiny.iso.crdownload", Bytes(100));
    CHECK(run(root / "tiny.iso.crdownload").error == ErrorId::E09);
    // E08: the volume says more sectors than the file has; E09 when it carries a download suffix
    {
        Bytes iso = make_iso({{"SLPS_256.75", elf_data}, {"DATA.AFS", afs_data}});
        put32(iso, 16 * 2048 + 80, 1000);
        write_file(root / "short.iso", iso);
        DiscIdentity id = run(root / "short.iso");
        CHECK(!id.known && id.error == ErrorId::E08);
        CHECK(id.detail == std::to_string(iso.size()) + " of " + std::to_string(1000 * 2048));
        write_file(root / "short.iso.part", iso);
        CHECK(run(root / "short.iso.part").error == ErrorId::E09);
    }
    // E10: containers by their magic bytes
    {
        struct { const char* file; Bytes head; const char* name; } cases[] = {
            {"a.chd", text("MComprHD"), "CHD"}, {"a.cso", text("CISO"), "CSO"}, {"a.zso", text("ZISO"), "ZSO"},
            {"a.7z", {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}, "7z"}, {"a.rar", text("Rar!\x1A\x07"), "RAR"},
            {"a.ecm", Bytes{'E', 'C', 'M', 0}, "ECM"}, {"a.mdf", text("MEDIA DESCRIPTOR"), "MDF"},
            {"a.cue", text("FILE \"a.bin\" BINARY\n TRACK 01 MODE2/2352\n  INDEX 01 00:00:00\n"), "CUE"}};
        for (auto& c : cases) {
            Bytes b = c.head;
            if (std::strcmp(c.name, "CUE") != 0) b.resize(std::max<size_t>(b.size(), 64), 0);  // a cue sheet is text
            write_file(root / c.file, b);
            DiscIdentity id = run(root / c.file);
            CHECK(id.error == ErrorId::E10 && id.detail == c.name);
        }
        Bytes nrg(40000, 0);
        std::memcpy(&nrg[nrg.size() - 12], "NERO", 4);
        write_file(root / "a.nrg", nrg);
        CHECK(run(root / "a.nrg").error == ErrorId::E10);
    }
    // E12: patches
    {
        struct { const char* file; Bytes head; } cases[] = {{"a.xdelta", {0xD6, 0xC3, 0xC4, 0}}, {"a.ppf", text("PPF30")},
                                                            {"a.ips", text("PATCH")}, {"a.bps", text("BPS1")}};
        for (auto& c : cases) {
            Bytes b = c.head;
            b.resize(64, 0);
            write_file(root / c.file, b);
            CHECK(run(root / c.file).error == ErrorId::E12);
        }
    }
    // E14: the GameCube disc header magic at 0x1C
    {
        Bytes gc(64, 0);
        gc[0x1C] = 0xC2; gc[0x1D] = 0x33; gc[0x1E] = 0x9F; gc[0x1F] = 0x3D;
        write_file(root / "gc.iso", gc);
        CHECK(run(root / "gc.iso").error == ErrorId::E14);
    }
    // E15: big enough, no signature
    write_file(root / "zeros.bin", Bytes(40000, 0));
    CHECK(run(root / "zeros.bin").error == ErrorId::E15);
    // E13: another PS2 game, with its serial
    {
        write_file(root / "other.iso", make_iso({{"SYSTEM.CNF", cnf("SLUS_123.45")}, {"SLUS_123.45", elf_data}}));
        DiscIdentity id = run(root / "other.iso");
        CHECK(!id.known && id.error == ErrorId::E13 && id.detail == "SLUS-12345" && id.reason == DiscVerdict::NotDon);
    }
    // E16: the right serial, a program file that does not match
    {
        write_file(root / "patched.iso",
                   make_iso({{"SYSTEM.CNF", cnf("SLPS_256.75")}, {"SLPS_256.75", elf_data}, {"DATA.AFS", afs_data}}));
        DiscIdentity id = run(root / "patched.iso");
        CHECK(!id.known && id.error == ErrorId::E16 && id.detail == "SLPS-25675" && id.reason == DiscVerdict::Modified);
    }
    // E19: the file ends after the directory (the volume size field is 0, so E08 does not apply)
    {
        Bytes iso = make_iso({{"SYSTEM.CNF", cnf("SLPS_256.75")}, {"SLPS_256.75", Bytes(3 * 2048, 1)}, {"DATA.AFS", Bytes(4 * 2048, 2)}});
        iso.resize(iso.size() - 2 * 2048);
        write_file(root / "cut.iso", iso);
        DiscIdentity id = run(root / "cut.iso");
        CHECK(!id.known && id.error == ErrorId::E19 && id.reason == DiscVerdict::Unreadable);
        CHECK(!id.detail.empty() && std::stoi(id.detail) > 0 && std::stoi(id.detail) < 100);
    }
    // A good verdict carries no error
    {
        DiscTable t{sha256_hex(elf_data.data(), elf_data.size()), {{"x", "X", sha1_hex(afs_data.data(), afs_data.size())}}};
        write_file(root / "ok.iso", make_iso({{"SLPS_256.75", elf_data}, {"DATA.AFS", afs_data}}));
        DiscIdentity id = identify_disc(root / "ok.iso", t, nullptr);
        CHECK(id.known && id.error == ErrorId::None && id.variant_id == "x");
    }
    check_table();

    fs::remove_all(root);
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    std::cout << "all disc error checks passed\n";
    return 0;
}
