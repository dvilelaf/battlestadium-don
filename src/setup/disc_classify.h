// What kind of file or folder the player picked, decided from its first bytes only (never the extension, except
// the partial-download names of E09, which only fill `detail`). No game data is read beyond one 64 KiB window.
#pragma once

#include <filesystem>
#include <string>

namespace don_setup {

enum class Kind {
    Iso, Gz, Zip, Chd, Cso, Zso, SevenZip, Rar, Ecm, Nrg, Mdf, Cue, Xdelta, Ppf, Ips, Bps, GameCube,
    Folder,             // a folder without SYSTEM.CNF
    DirectoryWithGame,  // a folder holding SYSTEM.CNF
    TooSmall,           // too short to be a disc image and matching no other magic
    Unknown
};

struct Classification {
    Kind kind = Kind::Unknown;
    std::string detail;  // short English hint, e.g. "still downloading" or the reason for Unknown
};

Classification classify(const std::filesystem::path& path);

}  // namespace don_setup
