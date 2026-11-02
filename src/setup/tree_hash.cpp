// SHA-256 of a folder tree, in the byte format documented in tree_hash.h (task T-617).
#include "tree_hash.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "iso_reader.h"

namespace fs = std::filesystem;

namespace don_setup {
namespace {

// Chunk size used to stream the contents of the files: 64 KiB.
constexpr size_t kChunkSize = 64 * 1024;

// A relative path as UTF-8 with '/' separators, the same on every OS.
std::string relative_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

}  // namespace

std::string tree_hash(const fs::path& root, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = "not a folder: " + relative_utf8(root);
        return {};
    }

    // 1. Collect the regular files, with their relative paths.
    std::vector<std::pair<std::string, fs::path>> files;
    fs::recursive_directory_iterator it(root, ec), end;
    if (ec) {
        error = "cannot read " + relative_utf8(root) + ": " + ec.message();
        return {};
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot read " + relative_utf8(root) + ": " + ec.message();
            return {};
        }
        const fs::path rel = fs::relative(it->path(), root, ec);
        const std::string rel_text = relative_utf8(rel);
        const fs::file_status st = it->symlink_status(ec);
        if (ec) {
            error = "cannot read the status of " + rel_text + ": " + ec.message();
            return {};
        }
        if (fs::is_symlink(st)) {
            error = "symbolic link not allowed: " + rel_text;
            return {};
        }
        if (fs::is_directory(st)) continue;  // folders hold no file of their own
        if (!fs::is_regular_file(st)) {
            error = "not a regular file: " + rel_text;
            return {};
        }
        files.emplace_back(rel_text, it->path());
    }

    // 2. Byte-wise sort of the relative paths: std::string compares as unsigned bytes.
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 3. Stream each file: path, 0x00, size (8 bytes little-endian), contents.
    Sha256 hash;
    std::vector<uint8_t> chunk(kChunkSize);
    for (const auto& [rel, path] : files) {
        std::error_code size_ec;
        const uintmax_t size = fs::file_size(path, size_ec);
        if (size_ec) {
            error = "cannot read the size of " + rel + ": " + size_ec.message();
            return {};
        }
        hash.update(reinterpret_cast<const uint8_t*>(rel.data()), rel.size());
        const uint8_t terminator = 0;
        hash.update(&terminator, 1);
        uint8_t size_le[8];
        for (int i = 0; i < 8; ++i) size_le[i] = uint8_t(uint64_t(size) >> (8 * i));
        hash.update(size_le, sizeof size_le);

        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + rel;
            return {};
        }
        uint64_t left = size;
        while (left > 0) {
            const size_t take = size_t(std::min<uint64_t>(left, chunk.size()));
            in.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(take));
            if (size_t(in.gcount()) != take) {
                error = "file changed while it was read: " + rel;
                return {};
            }
            hash.update(chunk.data(), take);
            left -= take;
        }
    }
    return hash.finish();
}

}  // namespace don_setup
