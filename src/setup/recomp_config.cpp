#include "recomp_config.h"

#include <system_error>
#include <vector>

namespace don_setup {

namespace {

// Python's Path(p).resolve().as_posix(): absolute, links followed as far as the path exists.
std::string resolved(const std::filesystem::path& p) {
    std::error_code ec;
    auto r = std::filesystem::weakly_canonical(std::filesystem::absolute(p, ec), ec);
    return (ec ? p : r).generic_string();
}

void replace_all(std::string& s, const std::string& key, const std::string& value) {
    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size()))
        s.replace(at, key.size(), value);
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

bool render_recomp_config(const std::string& frozen, const std::filesystem::path& disc,
                          const std::filesystem::path& out, const std::filesystem::path& install_root,
                          bool emit_module_abi, std::string& rendered, std::string& error) {
    const std::string d = resolved(disc), o = resolved(out), r = resolved(install_root);
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < frozen.size()) {
        size_t nl = frozen.find('\n', at);
        if (nl == std::string::npos) nl = frozen.size();
        std::string line = frozen.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = nl + 1;
        if (!starts_with(line, "#")) {
            replace_all(line, "{disc}", d);
            replace_all(line, "{out}", o);
            replace_all(line, "{repo}", r);
        }
        lines.push_back(std::move(line));
    }
    bool general = false;
    if (emit_module_abi) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] == "[general]") {
                general = true;
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i) + 1, "emit_module_abi = true");
                break;
            }
        }
        if (!general) {
            error = "the recompiler config has no [general] table";
            return false;
        }
        bool seen = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!starts_with(lines[i], "emit_module_abi")) continue;
            if (!seen) { seen = true; continue; }  // the line just inserted
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    rendered.clear();
    for (const auto& l : lines) rendered += l + "\n";
    return true;
}

}  // namespace don_setup
