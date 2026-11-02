// Discovery (T-627) over a fake home tree built in a temporary folder. Synthetic images only; the real home and
// disc/ are never scanned.
#include "discover.h"

#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                              \
    do {                                                                                      \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put32(Bytes& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }

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

// A flat ISO9660 image: PVD at 16, root directory at 18, SYSTEM.CNF at 19 (a few sectors in all).
static Bytes make_iso(const std::string& boot) {
    std::string cnf = "BOOT2 = cdrom0:\\" + boot + ";1\r\nVER = 1.00\r\n";
    Bytes iso(20 * 2048, 0);
    iso[16 * 2048] = 1;
    std::memcpy(&iso[16 * 2048 + 1], "CD001", 5);
    Bytes root(2048);
    size_t o = 0;
    dir_record(root, o, "", 18, 2048, true, 0);
    std::memcpy(&iso[16 * 2048 + 156], root.data(), 34);
    dir_record(root, o, "", 18, 2048, true, 1);
    dir_record(root, o, "SYSTEM.CNF;1", 19, uint32_t(cnf.size()));
    std::memcpy(&iso[18 * 2048], root.data(), 2048);
    std::memcpy(&iso[19 * 2048], cnf.data(), cnf.size());
    return iso;
}

static void write_file(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_discover_" + std::to_string(getpid()));
    fs::remove_all(tmp);
    fs::path home = tmp / "home", outside = tmp / "outside";

    write_file(home / "Downloads" / "good.iso", make_iso("SLPS_256.75"));
    write_file(home / "Games" / "a" / "b" / "c" / "too_deep.iso", make_iso("SLPS_256.75"));  // below depth 2: not reached
    write_file(home / "Documents" / "other.iso", make_iso("SLUS_123.45"));
    write_file(home / "Desktop" / "game.zip", Bytes{'P', 'K', 3, 4, 0, 0, 0, 0});
    write_file(home / "Desktop" / "notes.txt", Bytes{'h', 'i'});
    write_file(outside / "escaped.iso", make_iso("SLPS_256.75"));
    fs::create_symlink(outside, home / "Downloads" / "escape");                 // leaves the root
    fs::create_symlink(outside / "escaped.iso", home / "Downloads" / "link.iso");  // file outside the root
    fs::create_directory_symlink(home / "Documents", home / "Documents" / "loop");  // loop onto itself

#ifdef _WIN32
    // Windows asks the shell for its known folders and ignores `home` (its roots are tested by
    // test_startup_discover_win32); the scan itself is run on the folders this test made.
    std::vector<fs::path> roots = {home / "Downloads", home / "Desktop", home / "Documents", home / "Games"};
#else
    auto roots = discovery_roots(home, "nobody");
    CHECK(roots.size() == 4);  // Downloads, Desktop, Documents, Games exist; the rest do not
#endif

    auto t0 = std::chrono::steady_clock::now();
    DiscoverResult r = discover(roots, std::chrono::milliseconds(2000));
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took < std::chrono::milliseconds(2000));
    CHECK(!r.timed_out);
    CHECK(r.found.size() == 1 && r.found[0] == fs::canonical(home) / "Downloads" / "good.iso");
    CHECK(r.possible.size() == 1 && r.possible[0] == fs::canonical(home) / "Desktop" / "game.zip");

    // A zero budget stops before reading anything.
    DiscoverResult z = discover(roots, std::chrono::milliseconds(0));
    CHECK(z.timed_out && z.found.empty() && z.possible.empty());

    // A PCSX2 game-list folder is added as a root.
    write_file(home / ".config/PCSX2/inis/PCSX2.ini", Bytes{});
    fs::create_directories(tmp / "roms");
    std::string ini = "[GameList]\nRecursivePaths0=" + (tmp / "roms").string() + "\n";
    write_file(home / ".config/PCSX2/inis/PCSX2.ini", Bytes(ini.begin(), ini.end()));
#ifndef _WIN32   // Windows reads PCSX2.ini from the Documents known folder, not from `home`
    CHECK(discovery_roots(home, "nobody").size() == 5);
#endif

    CHECK(!is_network_filesystem(tmp));  // a temporary folder is local

    fs::remove_all(tmp);
    if (g_fail) { std::cerr << g_fail << " check(s) failed\n"; return 1; }
    std::cout << "discover: ok\n";
    return 0;
}
