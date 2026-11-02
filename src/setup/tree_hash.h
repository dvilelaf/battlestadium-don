// SHA-256 of a folder tree: tells whether the code generated on the player's PC is exactly the reference tree
// (same files, same bytes) before it is compiled (task T-617).
//
// Byte format of the hashed stream (a second implementation, for example in Python, must produce the same hex):
//
//   1. The regular files under the root are listed by their path relative to the root, written with '/' as
//      separator (also on Windows), and sorted by their bytes (byte-wise, unsigned, so the order is the same
//      on every OS).
//   2. For each file, in that order, the stream receives:
//        a. the relative path, as UTF-8 bytes, without terminator;
//        b. one byte 0x00 (a path cannot contain it, so the path ends there);
//        c. the size of the file in bytes, as an unsigned 64-bit integer, 8 bytes little-endian;
//        d. the contents of the file, exactly as stored.
//   3. The result is the SHA-256 of the whole stream, as 64 lowercase hexadecimal characters.
//
// Rules: symbolic links are an error and are never followed; empty folders do not count (they hold no file);
// any other entry that is neither a regular file nor a folder (device, pipe, socket) is an error. The files
// are streamed in chunks, so a large tree does not have to fit in memory.
#pragma once

#include <filesystem>
#include <string>

namespace don_setup {

// Hash of the tree under `root` in the format above. On failure returns an empty string and sets `error`
// to a message that names the offending path.
std::string tree_hash(const std::filesystem::path& root, std::string& error);

}  // namespace don_setup
