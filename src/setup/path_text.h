// UTF-8 text of a path and back (task T-749). On Windows std::filesystem::path::string() converts through the ANSI code
// page and throws ("No Unicode translation") for a name that page cannot hold (Japanese or Cyrillic on a Western
// install, most accented names on other pages). Every path that becomes a log line, a message, a manifest field or a
// process argument goes through path_text(); a std::string that carries such text and becomes a path again goes
// through path_from_text(). Paths handed to the OS stay std::filesystem::path (wide on Windows).
// On POSIX a path already is bytes and both functions are the identity.
#pragma once

#include <filesystem>
#include <string>

namespace don_setup {

inline std::string path_text(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.string();
#endif
}

inline std::filesystem::path path_from_text(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::path(utf8);
#endif
}

}  // namespace don_setup
