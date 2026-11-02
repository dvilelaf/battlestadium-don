// identify_disc on a folder, on a device-like file, with unknown game data (E17), and the built-in table against
// config/discs.toml. All contents are made up here; no game file is read.
#include "disc_image.h"

#include <cstdint>
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

static Bytes pattern(size_t n, uint32_t seed) {
    Bytes b(n);
    uint32_t x = seed;
    for (auto& c : b) { x = x * 1664525u + 1013904223u; c = uint8_t(x >> 24); }
    return b;
}
static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
static void write_file(const fs::path& p, const Bytes& d) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}

static const Bytes kCnf = [] { std::string s = "BOOT2 = cdrom0:\\SLPS_256.75;1\n"; return Bytes(s.begin(), s.end()); }();
static const Bytes kElf = pattern(5000, 1), kAfs = pattern(9000, 2);

static DiscTable fake_table() {
    return {sha256_hex(kElf.data(), kElf.size()), {{"jp", "Fake", sha1_hex(kAfs.data(), kAfs.size())}}};
}

// A flat ISO9660 image: PVD at sector 16, root directory at 18, the files after it.
static Bytes make_iso(const std::vector<std::pair<std::string, Bytes>>& files) {
    uint32_t next = 19;
    std::vector<uint32_t> lba;
    for (auto& f : files) { lba.push_back(next); next += uint32_t((f.second.size() + 2047) / 2048); }
    Bytes iso(size_t(next) * 2048, 0);
    auto record = [&](size_t off, const std::string& name, uint32_t at, uint32_t size, bool dir, bool special) {
        size_t nl = special ? 1 : name.size(), len = 33 + nl + ((nl % 2) ? 0 : 1);
        iso[off] = uint8_t(len);
        put32(iso, off + 2, at);
        put32(iso, off + 10, size);
        iso[off + 25] = dir ? 2 : 0;
        iso[off + 32] = uint8_t(nl);
        if (!special) std::memcpy(&iso[off + 33], name.data(), nl);
        return len;
    };
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    iso[16 * 2048 + 6] = 1;
    put32(iso, 16 * 2048 + 80, next);
    record(16 * 2048 + 156, "", 18, 2048, true, true);
    size_t off = 18 * 2048;
    off += record(off, "", 18, 2048, true, true);
    off += record(off, "", 18, 2048, true, true);
    for (size_t i = 0; i < files.size(); ++i)
        off += record(off, files[i].first + ";1", lba[i], uint32_t(files[i].second.size()), false, false);
    for (size_t i = 0; i < files.size(); ++i) std::memcpy(&iso[size_t(lba[i]) * 2048], files[i].second.data(), files[i].second.size());
    return iso;
}

static fs::path scratch(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("don_t623c_") + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

static void folder_cases() {
    const auto t = fake_table();
    fs::path d = scratch("good");
    write_file(d / "SYSTEM.CNF", kCnf);
    write_file(d / "SLPS_256.75", kElf);
    write_file(d / "DATA.AFS", kAfs);
    auto id = identify_disc(d, t, nullptr);
    CHECK(id.known && id.variant_id == "jp");

    fs::path lower = scratch("lower");  // names in another case still count
    write_file(lower / "system.cnf", kCnf);
    write_file(lower / "slps_256.75", kElf);
    write_file(lower / "data.afs", kAfs);
    CHECK(identify_disc(lower, t, nullptr).known);

    fs::path none = scratch("empty");
    write_file(none / "readme.txt", kCnf);
    id = identify_disc(none, t, nullptr);
    CHECK(!id.known && id.error == ErrorId::E03);

    fs::path half = scratch("half");  // SYSTEM.CNF without the program file
    write_file(half / "SYSTEM.CNF", kCnf);
    id = identify_disc(half, t, nullptr);
    CHECK(!id.known && id.error == ErrorId::E03);

    fs::path odd = scratch("odd");  // known program file, unknown data
    Bytes other = pattern(9000, 77);
    write_file(odd / "SYSTEM.CNF", kCnf);
    write_file(odd / "SLPS_256.75", kElf);
    write_file(odd / "DATA.AFS", other);
    id = identify_disc(odd, t, nullptr);
    CHECK(!id.known && id.error == ErrorId::E17);
    CHECK(id.variant_id == "custom-" + sha1_hex(other.data(), other.size()).substr(0, 8));

    fs::path bad = scratch("badelf");
    write_file(bad / "SYSTEM.CNF", kCnf);
    write_file(bad / "SLPS_256.75", pattern(5000, 9));
    write_file(bad / "DATA.AFS", kAfs);
    id = identify_disc(bad, t, nullptr);
    CHECK(!id.known && id.error == ErrorId::E16);
}

static void image_cases() {
    const auto t = fake_table();
    fs::path d = scratch("img");
    Bytes other = pattern(9000, 5);
    write_file(d / "odd.iso", make_iso({{"SYSTEM.CNF", kCnf}, {"SLPS_256.75", kElf}, {"DATA.AFS", other}}));
    auto id = identify_disc(d / "odd.iso", t, nullptr);
    CHECK(!id.known && id.error == ErrorId::E17);
    CHECK(id.variant_id == "custom-" + sha1_hex(other.data(), other.size()).substr(0, 8));

    // A block device is opened read-only and read as a raw image; a regular file takes the same code path
    // (no real device was tried).
    write_file(d / "dev.iso", make_iso({{"SYSTEM.CNF", kCnf}, {"SLPS_256.75", kElf}, {"DATA.AFS", kAfs}}));
    id = identify_disc(d / "dev.iso", t, nullptr);
    CHECK(id.known && id.variant_id == "jp");
}

// Reads `key = "value"` lines of config/discs.toml: the [elf] hash and each [[variant]].
static void table_matches_toml() {
    fs::path toml = fs::path(__FILE__).parent_path().parent_path().parent_path() / "config" / "discs.toml";
    std::ifstream in(toml);
    CHECK(bool(in));
    DiscTable want;
    std::string line;
    while (std::getline(in, line)) {
        if (line == "[[variant]]") { want.variants.emplace_back(); continue; }
        size_t eq = line.find(" = \""), end = line.rfind('"');
        if (eq == std::string::npos || end <= eq + 3) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 4, end - eq - 4);
        if (key == "sha256") want.elf_sha256 = val;
        else if (!want.variants.empty() && key == "id") want.variants.back().id = val;
        else if (!want.variants.empty() && key == "description") want.variants.back().description = val;
        else if (!want.variants.empty() && key == "data_afs_sha1") want.variants.back().data_afs_sha1 = val;
    }
    const DiscTable& got = builtin_disc_table();
    CHECK(!want.elf_sha256.empty() && got.elf_sha256 == want.elf_sha256);
    CHECK(want.variants.size() == 3 && got.variants.size() == want.variants.size());
    for (size_t i = 0; i < want.variants.size() && i < got.variants.size(); ++i) {
        CHECK(got.variants[i].id == want.variants[i].id);
        CHECK(got.variants[i].description == want.variants[i].description);
        CHECK(got.variants[i].data_afs_sha1 == want.variants[i].data_afs_sha1);
    }
}

int main() {
    folder_cases();
    image_cases();
    table_matches_toml();
    if (g_fail) { std::cerr << g_fail << " failures\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
