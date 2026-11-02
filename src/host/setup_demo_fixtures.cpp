// SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_demo_fixtures.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "file_browser_view.h"

namespace
{
// The in-app browser's fake disk: placeholder files only, never a real disc image. The iso placeholder is zeros
// with the ISO 9660 marker where the classifier looks, so its verdict reads "Disc image".
const std::filesystem::path kBrowserRoot = "/tmp/t628b-demo";

void put(const std::filesystem::path &p, std::size_t size = 0, std::size_t at = 0, const std::string &magic = {})
{
    std::filesystem::create_directories(p.parent_path());
    std::string bytes(std::max(size, at + magic.size()), '\0');
    bytes.replace(at, magic.size(), magic);
    std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
} // namespace

namespace don_host {

void setup_demo_make_browser_tree()
{
    std::error_code ec;
    std::filesystem::remove_all(kBrowserRoot, ec);
    const auto dl = kBrowserRoot / "home" / "Downloads", games = kBrowserRoot / "home" / "Games";
    std::filesystem::create_directories(dl / "Photos");
    std::filesystem::create_directories(dl / "Old downloads");
    put(dl / "BSD-extracted" / "SYSTEM.CNF");
    put(dl / "archive.gz", 64, 0, "\x1f\x8b");
    put(dl / "battle-stadium-don.iso", 17 * 2048 + 8, 16 * 2048 + 1, "CD001");
    put(dl / "notes.txt", 40);
    for (int i = 1; i <= 14; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "part-%02d.bin", i);
        put(dl / name, 100);
    }
    put(games / "Some Other Game (USA)" / "SYSTEM.CNF");
    put(games / "a-very-long-file-name-that-does-not-fit-on-one-line-of-the-list-at-small-sizes.iso", 17 * 2048 + 8,
        16 * 2048 + 1, "CD001");
    file_browser_set_start_places({dl, games});
}

} // namespace don_host

don_setup::PowerState setup_demo_fake_battery_state()
{
    const std::filesystem::path root = "/tmp/t644-demo-sysfs";
    const auto ps = root / "sys/class/power_supply";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(ps / "BAT0");
    std::filesystem::create_directories(ps / "AC");
    std::ofstream(ps / "BAT0" / "type") << "Battery\n";
    std::ofstream(ps / "BAT0" / "status") << "Discharging\n";
    std::ofstream(ps / "BAT0" / "capacity") << "63\n";
    std::ofstream(ps / "AC" / "type") << "Mains\n";
    std::ofstream(ps / "AC" / "online") << "0\n";
    return don_setup::read_power(root.string());
}
