#include "discover.h"
#include "path_text.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string>

#include "disc_classify.h"
#include "iso_reader.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")  // FOLDERID_*
#endif

namespace fs = std::filesystem;

namespace don_setup {
namespace {

constexpr int kMaxDepth = 2;
constexpr const char* kBootName = "SLPS_256.75";  // SYSTEM.CNF BOOT2 of this game

std::string lower_ext(const fs::path& p) {
    std::string e = path_text(p.extension());
    for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool candidate_extension(const std::string& e) {
    static const char* const k[] = {".iso", ".zip", ".gz", ".7z", ".rar", ".chd", ".cso", ".zso", ".ecm"};
    return std::any_of(std::begin(k), std::end(k), [&](const char* x) { return e == x; });
}

}  // namespace

// Descriptor, root directory and SYSTEM.CNF only; no file data beyond SYSTEM.CNF is read.
bool is_this_game(const fs::path& iso) {
    ImageReader img;
    std::string err;
    std::string t;
    constexpr uint64_t kMaxSystemCnf = 4096;
    if (!img.open(iso, err) || !read_root_file(img, "SYSTEM.CNF", kMaxSystemCnf, t)) return false;
    size_t b = t.find("BOOT2");
    if (b == std::string::npos) return false;
    size_t s = t.find('\\', b);
    if (s == std::string::npos) return false;
    size_t e = t.find_first_of(";\r\n \t", s + 1);
    return ieq(t.substr(s + 1, e == std::string::npos ? e : e - s - 1), kBootName);
}

namespace {

bool inside(const fs::path& root, const fs::path& p) {
    auto r = root.begin(), rend = root.end();
    auto q = p.begin(), qend = p.end();
    for (; r != rend; ++r, ++q)
        if (q == qend || *q != *r) return false;
    return true;
}

class Scan {
public:
    Scan(std::chrono::milliseconds budget, DiscoverResult& out)
        : deadline_(std::chrono::steady_clock::now() + budget), out_(out) {}

    void root(const fs::path& r) {
        std::error_code ec;
        fs::path canon = fs::canonical(r, ec);
        if (ec || !fs::is_directory(canon, ec)) return;
        walk(canon, canon, 0);
    }

private:
    bool expired() {
        if (out_.timed_out) return true;
        if (std::chrono::steady_clock::now() >= deadline_) out_.timed_out = true;
        return out_.timed_out;
    }

    void walk(const fs::path& root, const fs::path& dir, int depth) {
        if (expired() || !visited_.insert(path_text(dir)).second) return;  // a loop or an already scanned folder
        if (is_network_filesystem(dir)) return;
        std::error_code ec;
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec))
            entries.push_back(it->path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path& e : entries) {
            if (expired()) return;
            // A symlink counts only when its final target is still inside this root.
            fs::path target = e;
            if (fs::is_symlink(fs::symlink_status(e, ec))) {
                target = fs::canonical(e, ec);
                if (ec || !inside(root, target)) continue;
            }
            if (fs::is_directory(target, ec)) {
                if (depth < kMaxDepth) walk(root, target, depth + 1);
            } else if (fs::is_regular_file(target, ec) && candidate_extension(lower_ext(e))) {
                file(e, target);
            }
        }
    }

    void file(const fs::path& shown, const fs::path& real) {
        switch (classify(real).kind) {
            case Kind::Iso:
                if (is_this_game(real)) out_.found.push_back(shown);
                break;
            case Kind::Gz: case Kind::Zip: case Kind::SevenZip: case Kind::Rar: case Kind::Chd: case Kind::Cso:
            case Kind::Zso: case Kind::Ecm:
                out_.possible.push_back(shown);
                break;
            default:
                break;
        }
    }

    std::chrono::steady_clock::time_point deadline_;
    DiscoverResult& out_;
    std::set<std::string> visited_;
};

// PCSX2.ini keeps game-list folders as Paths0=... / RecursivePaths0=... under [GameList].
void pcsx2_folders(const fs::path& ini, std::vector<fs::path>& out) {
    std::ifstream in(ini);
    bool section = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { section = line == "[GameList]"; continue; }
        size_t eq = line.find('=');
        if (!section || eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        if (key.rfind("Paths", 0) == 0 || key.rfind("RecursivePaths", 0) == 0) out.push_back(path_from_text(line.substr(eq + 1)));
    }
}

#ifdef _WIN32
// A Windows known folder (it may be redirected, e.g. to OneDrive, so it is asked for and never built from the home).
// Empty when the shell does not know it.
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR w = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &w)) && w) p = fs::path(std::wstring(w));
    CoTaskMemFree(w);
    return p;
}
#endif

}  // namespace

// `home` and `user` shape the Linux roots; on Windows the roots are the user's known folders (Downloads, Desktop,
// Documents) and the game-list folders of PCSX2's ini at Documents\PCSX2\inis\PCSX2.ini. Disc drives are out of
// scope there (an image file only).
std::vector<fs::path> discovery_roots(const fs::path& home, const std::string& user) {
    std::vector<fs::path> all;
#ifdef _WIN32
    (void)home;
    (void)user;
    const fs::path documents = known_folder(FOLDERID_Documents);
    for (const fs::path& p : {known_folder(FOLDERID_Downloads), known_folder(FOLDERID_Desktop), documents})
        if (!p.empty()) all.push_back(p);
    if (!documents.empty()) pcsx2_folders(documents / "PCSX2" / "inis" / "PCSX2.ini", all);
#else
    for (const char* sub : {"Downloads", "Desktop", "Documents", "Games", "Emulation/roms/ps2"}) all.push_back(home / sub);
    pcsx2_folders(home / ".config" / "PCSX2" / "inis" / "PCSX2.ini", all);
    if (!user.empty()) {
        all.push_back(fs::path("/media") / user);
        all.push_back(fs::path("/run/media") / user);
    }
#endif
    std::vector<fs::path> roots;
    std::error_code ec;
    for (const fs::path& p : all)
        if (fs::is_directory(p, ec) && std::find(roots.begin(), roots.end(), p) == roots.end()) roots.push_back(p);
    return roots;
}

DiscoverResult discover(const std::vector<fs::path>& roots, std::chrono::milliseconds budget) {
    DiscoverResult out;
    Scan scan(budget, out);
    for (const fs::path& r : roots) scan.root(r);
    std::sort(out.found.begin(), out.found.end());
    std::sort(out.possible.begin(), out.possible.end());
    return out;
}

}  // namespace don_setup
