// First-run discovery (A3 S0, task T-627): finds the player's disc image in the usual folders without any prompt.
// Depth <= 2 (the root is depth 0, so root/a/b/file is reached), no symlink leaves its root, no network filesystem,
// and a time budget. A raw ISO is "found" only when its SYSTEM.CNF BOOT2 names this game; compressed files cannot be
// checked cheaply and are only "possible".
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace don_setup {

struct DiscoverResult {
    std::vector<std::filesystem::path> found;     // raw ISOs of this game, sorted
    std::vector<std::filesystem::path> possible;  // compressed images (zip, gz, 7z...), unchecked, sorted
    bool timed_out = false;                       // the budget ended the scan
};

// The S0 folders under `home`: Downloads, Desktop, Documents, Games, Emulation/roms/ps2, the game-list folders of
// PCSX2's PCSX2.ini ([GameList] Paths*/RecursivePaths*), /media/<user> and /run/media/<user>. Only folders that exist.
std::vector<std::filesystem::path> discovery_roots(const std::filesystem::path& home, const std::string& user);

DiscoverResult discover(const std::vector<std::filesystem::path>& roots, std::chrono::milliseconds budget);

// True when `iso` is a readable ISO 9660 image whose SYSTEM.CNF names this game (SLPS_256.75). Reads only the
// descriptor, the root directory and SYSTEM.CNF; never raises an error, a file that cannot be read gives false.
bool is_this_game(const std::filesystem::path& iso);

// Platform part (platform/fs_linux.cpp): true when `dir` is on a network filesystem (NFS, SMB/CIFS, AFS, Ceph...).
bool is_network_filesystem(const std::filesystem::path& dir);

}  // namespace don_setup
