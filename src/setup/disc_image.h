// First-run disc handling (task B-01): identify a player's own disc image and copy its files out.
// Accepts a plain ISO9660 image (2048-byte sectors), the same gzip-compressed, or a .zip holding one .iso.
// No game data is embedded here, only the hashes in config/discs.toml.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "setup_types.h"

namespace don_setup {

using ProgressFn = std::function<void(double fraction, const std::string& detail)>;

struct DiscVariant {
    std::string id, description, data_afs_sha1;
};

struct DiscTable {
    std::string elf_sha256;
    std::vector<DiscVariant> variants;
};

// The table built into the program (values of config/discs.toml).
const DiscTable& builtin_disc_table();

DiscIdentity identify_disc(const std::filesystem::path& image, const ProgressFn& progress);
// Same, against another table (tests).
DiscIdentity identify_disc(const std::filesystem::path& image, const DiscTable& table, const ProgressFn& progress);

bool extract_disc(const std::filesystem::path& image, const std::filesystem::path& dest, const ProgressFn& progress,
                  std::string& error);

// Lower-case hex digests of a buffer.
std::string sha1_hex(const void* data, size_t size);
std::string sha256_hex(const void* data, size_t size);

}  // namespace don_setup

#include "disc_manifest.h"
