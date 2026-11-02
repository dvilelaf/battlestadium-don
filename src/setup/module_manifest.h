// SPDX-License-Identifier: GPL-3.0-or-later
// module/<id>/manifest.json (docs/tasks/M6-public-release.md, A2) and its one reader (T-683). The loader
// (src/host/game_module_posix.cpp), the storage scan (storage.cpp) and the startup check (startup.cpp) all use it.
// Pure standard C++: no host code, no platform call.
#pragma once

#include <stdint.h>

#include <filesystem>
#include <string>

// The input hashes of the manifest are skipped: they are inputs of the module id, checked when the module is built.
struct DonModuleManifest
{
    std::string program_version;
    uint32_t abi_version = 0;
    std::string module_id;
    std::string toolchain_hash;
    std::string module_sha256;  // of libdon_game.so, lower-case hex
    bool verified = false;
    std::string variant;        // the game/<variant>/ the module was built for (storage.cpp pairs on it)
};

// A manifest is read only up to this size; a larger file is corrupt.
constexpr uintmax_t kDonModuleManifestMaxBytes = 1u << 20;

// Strict reader: one JSON object holding every field above with its type, no duplicate among them. Unknown fields are
// skipped (but must be valid JSON). On failure `error` says what is wrong.
bool don_read_module_manifest(const std::string &file, DonModuleManifest &out, std::string &error);
// Same, for a path that may hold characters outside the ANSI code page (Windows, T-749). The std::string overloads treat
// the text as a native path (ANSI on Windows); setup code passes a path.
bool don_read_module_manifest(const std::filesystem::path &file, DonModuleManifest &out, std::string &error);

// Writes the seven fields above as one JSON object into `file` (flushed and closed; the caller renames the folder
// into place). False, with `error` set, when the file cannot be written. don_read_module_manifest reads it back.
bool don_write_module_manifest(const std::string &file, const DonModuleManifest &m, std::string &error);
bool don_write_module_manifest(const std::filesystem::path &file, const DonModuleManifest &m, std::string &error);
