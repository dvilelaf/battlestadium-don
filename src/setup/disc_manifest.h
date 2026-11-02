// The per-file allow-list of a disc variant and the extraction that obeys it (task T-622).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_image.h"

namespace don_setup {

// The allow-list of one disc variant (config/disc_manifest.toml): the only files extract_disc copies.
struct ManifestFile {
    std::string path;  // relative, '/' separated
    uint64_t size = 0;
    std::string sha256;  // lower-case hex
};

struct DiscManifest {
    std::string variant_id, serial;
    std::vector<ManifestFile> files;
};

// Reads the text of config/disc_manifest.toml. False (with `error`) when it is malformed.
bool parse_disc_manifest(const std::string& toml_text, std::vector<DiscManifest>& out, std::string& error);

// What one extract_disc run did, for resume and for the one-pass check (E21).
struct ExtractStats {
    unsigned copied = 0;    // files written this run
    unsigned skipped = 0;   // files already in dest with the right size and SHA-256
    uint64_t restarts = 0;  // times a gz/zip stream was started over after the first start (0 = one forward pass)
};

// `image` may also be a folder that holds the game's files (any letter case): the same allow-list, SHA-256 check and
// error ids apply, minus the image-only ones (E08, E11).
// Copies only the files listed in `manifest`, in disc (LBA) order, hashing each one as it streams. A file already in
// `dest` with the listed size and SHA-256 is skipped (resume); any other is copied again. On failure *code (when
// given) is set and `detail` carries the facts of the message:
//   E18 a listed file is missing, has another size or another SHA-256 (its name is in `error`; the bad file is not
//       left in `dest`);
//   E08 the image ends early, or a gz/zip fails its CRC or length; detail = "have of want" bytes;
//   E19 a read error (real, or the fault eio@read); detail = the percent read, as digits;
//   E31 a write error (disk full, or the fault enospc@copy);
//   E11 an unusable zip; detail = no-iso | several-isos | password | unsupported-method | split.
bool extract_disc(const std::filesystem::path& image, const std::filesystem::path& dest, const ProgressFn& progress,
                  std::string& error, const DiscManifest& manifest, ErrorId* code = nullptr,
                  std::string* detail = nullptr, ExtractStats* stats = nullptr);

}  // namespace don_setup
