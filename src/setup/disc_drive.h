// Finding the game disc in the optical drives (A3 S1, task T-647): READ FROM DISC DRIVE is offered only when this
// finds one. Read-only: a drive is opened with O_RDONLY, the tray is never ejected or locked.
#pragma once

#include <filesystem>
#include <optional>

namespace don_setup {

// Separates the paths of DON_TEST_DISC_DEVICES: ':' where paths hold no colon, ';' on Windows ("C:\\x.iso").
#ifdef _WIN32
constexpr char kDeviceListSeparator = ';';
#else
constexpr char kDeviceListSeparator = ':';
#endif

// The first optical drive (/dev/sr0 ... /dev/sr15 on Linux) holding a readable ISO 9660 image whose SYSTEM.CNF names
// SLPS_256.75. Empty, unreadable, blank, or another game's disc gives nothing; it never raises an error.
// DON_TEST_DISC_DEVICES (paths separated by kDeviceListSeparator, like PATH) replaces the device list; for tests only.
std::optional<std::filesystem::path> find_game_disc();

}  // namespace don_setup
