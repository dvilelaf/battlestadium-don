// Finding the game disc in the optical drives (T-647) over synthetic "devices": plain files in a temporary folder,
// listed through DON_TEST_DISC_DEVICES. No /dev/sr* device is ever opened here.
#include "disc_drive.h"

#include "discover.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

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

// A flat ISO9660 image: PVD at 16, root directory at 18, SYSTEM.CNF at 19 (same layout as the discovery tests).
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

// Lists the given devices in DON_TEST_DISC_DEVICES and looks for the game disc.
static std::optional<fs::path> find_in(const std::vector<fs::path>& devices) {
    std::string list;
    for (const fs::path& d : devices) {
        if (!list.empty()) list += kDeviceListSeparator;
        list += d.string();
    }
    setenv("DON_TEST_DISC_DEVICES", list.c_str(), 1);
    auto found = find_game_disc();
    unsetenv("DON_TEST_DISC_DEVICES");
    return found;
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_disc_drive_" + std::to_string(getpid()));
    {   // a left-over read-only file of an earlier run would stop the clean-up on Windows
        std::error_code ec;
        fs::permissions(tmp / "dev" / "unreadable.iso", fs::perms::owner_all, ec);
    }
    fs::remove_all(tmp);
    fs::path dev = tmp / "dev";

    write_file(dev / "game.iso", make_iso("SLPS_256.75"));
    write_file(dev / "other_game.iso", make_iso("SLUS_123.45"));
    write_file(dev / "zeros.iso", Bytes(20 * 2048, 0));
    write_file(dev / "unreadable.iso", make_iso("SLPS_256.75"));
    fs::permissions(dev / "unreadable.iso", fs::perms::none);   // (Windows: only the read-only attribute)

    // The check is bounded: a root subdirectory whose extent lies far past the end of the image is never read.
    {
        Bytes iso = make_iso("SLPS_256.75");
        Bytes root(iso.begin() + 18 * 2048, iso.begin() + 19 * 2048);
        size_t sub = 0;
        while (root[sub]) sub += root[sub];
        size_t o = sub;
        dir_record(root, o, "SUBDIR", 0x7fffff00u, 2048);
        root[sub + 25] = 2;
        std::memcpy(&iso[18 * 2048], root.data(), 2048);
        write_file(dev / "subdir.iso", iso);
        CHECK(is_this_game(dev / "subdir.iso"));
        CHECK(find_in({dev / "subdir.iso"}).has_value());
    }

    // The game ISO is found.
    auto a = find_in({dev / "game.iso"});
    CHECK(a.has_value() && *a == dev / "game.iso");

    // Another game gives nothing.
    CHECK(!find_in({dev / "other_game.iso"}).has_value());

    // Zeros (blank or unformatted disc) give nothing.
    CHECK(!find_in({dev / "zeros.iso"}).has_value());

    // A missing device gives nothing and does not throw.
    CHECK(!find_in({dev / "missing.iso"}).has_value());

    // An unreadable device gives nothing. Root can read a mode-000 file, so the check is skipped then.
#ifndef _WIN32   // permission bits do not make a file unreadable on Windows
    if (::geteuid() != 0) CHECK(!find_in({dev / "unreadable.iso"}).has_value());
#endif

    // Two devices, the second holds the game: the second is returned.
    auto b = find_in({dev / "other_game.iso", dev / "game.iso"});
    CHECK(b.has_value() && *b == dev / "game.iso");

    fs::permissions(dev / "unreadable.iso", fs::perms::owner_all);   // Windows will not delete a read-only file
    fs::remove_all(tmp);
    if (g_fail) {
        std::cerr << g_fail << " check(s) failed\n";
        return 1;
    }
    std::cout << "test_setup_disc_drive: all checks passed\n";
    return 0;
}
