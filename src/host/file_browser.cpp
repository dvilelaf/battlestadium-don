#include "file_browser.h"

#include <algorithm>
#include <cstdint>
#include <fstream>

#include "../setup/disc_classify.h"
#include "setup_flow_model.h"  // don_setup::sanitize_utf8

namespace fs = std::filesystem;

namespace don_host {
namespace {

constexpr uint64_t kSector = 2048;

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// True when the ISO 9660 primary volume descriptor (sector 16) claims more volume space than the file holds, or
// when the file ends before it. The space size is a 32-bit little-endian field at byte 80 (ECMA-119 8.4.8), the
// same read as src/setup/disc_image.cpp. Only plain 2048-byte sectors are judged: a raw 2352- or 2448-byte image
// has other data at that offset, so it is not judged here (false). A descriptor that cannot be read is not judged.
bool truncated_iso(const fs::path& p) {
    std::error_code ec;
    uint64_t size = fs::file_size(p, ec);
    if (ec) return false;
    std::ifstream f(p, std::ios::binary);
    char id[5];
    if (!f.seekg(16 * kSector + 1) || !f.read(id, 5) || std::string(id, 5) != "CD001") return false;
    if (size < 17 * kSector) return true;
    uint8_t pvd[kSector];
    if (!f.seekg(16 * kSector) || !f.read(reinterpret_cast<char*>(pvd), kSector)) return false;
    uint64_t sectors = uint64_t(pvd[80]) | uint64_t(pvd[81]) << 8 | uint64_t(pvd[82]) << 16 | uint64_t(pvd[83]) << 24;
    return sectors * kSector > size;
}

const char* label_for(don_setup::Kind k) {
    using K = don_setup::Kind;
    switch (k) {
        case K::Iso: return "Disc image";
        case K::Gz: return "GZ archive";
        case K::Zip: return "ZIP archive";
        case K::Chd: return "CHD image";
        case K::Cso: return "CSO image";
        case K::Zso: return "ZSO image";
        case K::SevenZip: return "7z archive";
        case K::Rar: return "RAR archive";
        case K::Ecm: return "ECM image";
        case K::Nrg: return "NRG image";
        case K::Mdf: return "MDF image";
        case K::Cue: return "CUE sheet";
        case K::Xdelta: case K::Ppf: case K::Ips: case K::Bps: return "Patch file";
        case K::GameCube: return "GameCube disc";
        case K::Folder: return "";
        case K::DirectoryWithGame: return "Game folder";
        case K::TooSmall: return "Too small";
        case K::Unknown: return "Unknown file";
    }
    return "Unknown file";
}

}  // namespace

FileBrowserModel::FileBrowserModel(std::vector<fs::path> starts) : starts_(std::move(starts)) {
    fs::path root = "/";
    if (std::find(starts_.begin(), starts_.end(), root) == starts_.end()) starts_.push_back(root);
}

FileBrowserModel FileBrowserModel::from_home(const fs::path& home, const std::string& user) {
    return FileBrowserModel(don_setup::discovery_roots(home, user));
}

void FileBrowserModel::show_starts() {
    at_starts_ = true;
    entries_.clear();
    error_.clear();
    verdicts_.clear();
    truncated_.clear();
    sel_ = 0;
    first_ = 0;
}

void FileBrowserModel::open_dir(fs::path dir) {
    at_starts_ = false;
    dir_ = dir;
    entries_.clear();
    error_.clear();
    verdicts_.clear();
    truncated_.clear();
    sel_ = 0;
    first_ = 0;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec) { error_ = "This folder can't be read."; return; }
    for (; it != end; it.increment(ec)) {
        if (ec) { error_ = "This folder can't be read."; break; }
        std::error_code e2;
        bool folder = fs::is_directory(it->path(), e2);  // follows symlinks; a broken link is a file
        entries_.push_back({it->path(), don_setup::sanitize_utf8(it->path().filename().string()), folder});
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_folder != b.is_folder) return a.is_folder;
        std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    if (entries_.empty() && error_.empty()) error_ = "This folder is empty.";
}

don_setup::Kind FileBrowserModel::kind_for(const fs::path& p) {
    auto key = p.string();
    auto it = verdicts_.find(key);
    if (it != verdicts_.end()) return it->second;
    return verdicts_[key] = don_setup::classify(p).kind;  // classify reads at most its 64 KiB window
}

bool FileBrowserModel::is_truncated(const fs::path& p) {
    auto key = p.string();
    auto it = truncated_.find(key);
    if (it != truncated_.end()) return it->second;
    return truncated_[key] = truncated_iso(p);
}

std::string FileBrowserModel::verdict_for(const fs::path& p) {
    auto kind = kind_for(p);
    // A cut-short image would fail with E-FILE-4 when chosen: the list must not promise it is a complete disc image.
    if (kind == don_setup::Kind::Iso && is_truncated(p)) return "Incomplete disc image";
    return label_fn_ ? label_fn_(kind) : label_for(kind);
}

void FileBrowserModel::navigate(don_setup::Nav nav) {
    using N = don_setup::Nav;
    size_t n = count();
    switch (nav) {
        case N::Up: if (sel_ > 0) --sel_; break;           // stops at the ends: no wrap, so the end of a list is felt
        case N::Down: if (sel_ + 1 < n) ++sel_; break;
        case N::Left: case N::Right: break;
        case N::Back:
            if (at_starts_) break;
            if (std::find(starts_.begin(), starts_.end(), dir_) != starts_.end() || !dir_.has_parent_path() ||
                dir_.parent_path() == dir_)
                show_starts();
            else
                open_dir(dir_.parent_path());
            break;
        case N::Confirm: {
            if (sel_ >= n) break;
            if (at_starts_) { open_dir(starts_[sel_]); break; }
            const Entry& e = entries_[sel_];
            if (!e.is_folder) { choice_ = e.path.string(); break; }
            if (kind_for(e.path) == don_setup::Kind::DirectoryWithGame) choice_ = e.path.string();  // the setup model applies E03
            else if (!would_loop(e.path)) open_dir(e.path);  // a loop is not entered: the folder shown stays the same
            break;
        }
    }
    follow();
}

void FileBrowserModel::follow() {
    if (sel_ < first_) first_ = sel_;
    else if (sel_ >= first_ + visible_) first_ = sel_ + 1 - visible_;
}

void FileBrowserModel::select_visible(size_t row) {
    follow();
    if (row < visible_ && first_ + row < count()) sel_ = first_ + row;
}

bool FileBrowserModel::would_loop(const fs::path& target) const {
    for (fs::path here = dir_;;) {
        std::error_code ec;
        if (fs::equivalent(target, here, ec)) return true;  // equivalent() is false when it fails
        if (!here.has_parent_path() || here.parent_path() == here) return false;
        here = here.parent_path();
    }
}

BrowserView FileBrowserModel::view() {
    BrowserView v;
    size_t n = count();
    follow();
    size_t first = first_;
    size_t last = std::min(n, first + visible_);
    if (at_starts_) {
        v.title = "Places";
        for (size_t i = first; i < last; ++i) v.rows.push_back({starts_[i].string(), "", true, i == sel_});
        return v;
    }
    v.title = dir_.string();
    v.empty_reason = entries_.empty() ? error_ : "";
    for (size_t i = first; i < last; ++i) {
        const Entry& e = entries_[i];
        bool game = e.is_folder && kind_for(e.path) == don_setup::Kind::DirectoryWithGame;
        bool loop = e.is_folder && !game && would_loop(e.path);
        v.rows.push_back({e.name, verdict_for(e.path), e.is_folder, i == sel_, !loop});
    }
    return v;
}

std::optional<std::string> FileBrowserModel::take_choice() {
    auto c = std::move(choice_);
    choice_.reset();
    return c;
}

}  // namespace don_host
