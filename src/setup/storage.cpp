#include "storage.h"
#include "path_text.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <sstream>

#include "module_manifest.h"

namespace fs = std::filesystem;

namespace don_setup {
namespace {

// Bytes of regular files under `p`, never following a symlink (a link counts as zero). `ec` gets the first failure.
uint64_t tree_bytes(const fs::path& p, std::error_code& ec) {
    std::error_code e;
    fs::file_status st = fs::symlink_status(p, e);
    if (e) { if (e != std::errc::no_such_file_or_directory) ec = e; return 0; }
    if (fs::is_regular_file(st)) {
        uint64_t n = fs::file_size(p, e);
        if (e) { ec = e; return 0; }
        return n;
    }
    if (!fs::is_directory(st)) return 0;   // symlink or special file
    uint64_t total = 0;
    fs::directory_iterator it(p, e), end;
    if (e) { ec = e; return 0; }
    for (; it != end; it.increment(e)) {
        if (e) break;
        total += tree_bytes(it->path(), ec);
    }
    if (e) ec = e;
    return total;
}

uint64_t part_bytes(const fs::path& p, ErrorId& err) {
    std::error_code ec;
    uint64_t n = tree_bytes(p, ec);
    if (ec) err = ErrorId::E32;
    return n;
}

// Names of the sub-folders of `dir` (symlinks are not folders here), sorted. Missing `dir` gives an empty list.
std::vector<std::string> subfolders(const fs::path& dir, ErrorId& err) {
    std::vector<std::string> names;
    std::error_code e;
    fs::directory_iterator it(dir, e), end;
    if (e) { if (e != std::errc::no_such_file_or_directory) err = ErrorId::E32; return names; }
    for (; it != end; it.increment(e)) {
        if (e) break;
        std::error_code se;
        if (fs::is_directory(it->symlink_status(se))) names.push_back(path_text(it->path().filename()));
    }
    if (e) err = ErrorId::E32;
    std::sort(names.begin(), names.end());
    return names;
}

// True when `rel` (below `root`) is reached through a symlink at any level above its last component. A missing
// level is not a link. The last component itself may be a link: it is then handled as a link, never followed.
bool via_link(const fs::path& root, const fs::path& rel) {
    fs::path cur = root;
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        if (std::next(it) == rel.end()) break;
        cur /= *it;
        std::error_code e;
        if (fs::is_symlink(fs::symlink_status(cur, e))) return true;
    }
    return false;
}

// Bytes of root/rel, zero (and no error) when a level on the way is a link.
uint64_t safe_part_bytes(const fs::path& root, const fs::path& rel, ErrorId& err) {
    return via_link(root, rel) ? 0 : part_bytes(root / rel, err);
}

// Sub-folders of root/rel unless a level on the way, or `rel` itself, is a link.
std::vector<std::string> safe_subfolders(const fs::path& root, const fs::path& rel, ErrorId& err) {
    std::error_code e;
    if (via_link(root, rel) || fs::is_symlink(fs::symlink_status(root / rel, e))) return {};
    return subfolders(root / rel, err);
}

// Removes `p` (a symlink is removed as a link). Adds what disappeared to `freed`.
void remove_part(const fs::path& p, FreedBytes& freed) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) return;
    ErrorId measure = ErrorId::None;
    uint64_t before = part_bytes(p, measure);
    fs::remove_all(p, ec);
    uint64_t after = 0;
    if (ec) {
        freed.error = ErrorId::E32;
        after = part_bytes(p, measure);
    }
    freed.bytes += before > after ? before - after : 0;
}

FreedBytes remove_parts(const fs::path& root, std::initializer_list<const char*> parts) {
    FreedBytes freed;
    for (const char* part : parts) {
        fs::path rel = part;
        if (via_link(root, rel)) {
            freed.blocked.push_back(rel.begin()->string());
            continue;
        }
        remove_part(root / rel, freed);
    }
    return freed;
}

bool plain_name(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\") == std::string::npos &&
           n.find('\0') == std::string::npos;
}

// The variant a manifest names, or an empty string when it cannot be read (module_manifest.h, the one reader) or does
// not name one validly. A manifest that is a link is not read.
std::string variant_of_module(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec))) return {};
    DonModuleManifest manifest;
    std::string why;
    if (!don_read_module_manifest(file, manifest, why) || !plain_name(manifest.variant)) return {};
    return manifest.variant;
}

bool is_tmp(const std::string& name) {
    const std::string suffix = ".tmp";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// game/prepared.txt (storage.h): "<variant> <module id>" per line, oldest first. Lines that do not fit are skipped.
struct Record {
    std::string variant, module_id;
};

std::vector<Record> read_records(const fs::path& root) {
    std::vector<Record> out;
    const fs::path file = root / "game" / "prepared.txt";
    std::error_code ec;
    if (via_link(root, "game/prepared.txt") || !fs::is_regular_file(fs::symlink_status(file, ec))) return out;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        Record r;
        std::string extra;
        if ((ls >> r.variant >> r.module_id) && !(ls >> extra) && plain_name(r.variant) && plain_name(r.module_id))
            out.push_back(r);
    }
    return out;
}

struct Modules {
    std::set<std::string> ids;        // module folders with a valid manifest
    std::set<std::string> variants;   // named by a valid manifest, or by a record of one of those modules
    std::vector<std::string> bad;     // module folders (not .tmp) whose manifest is missing or invalid
    std::vector<std::string> temps;   // module/<x>.tmp
};

Modules scan_modules(const fs::path& root, ErrorId& err) {
    Modules m;
    for (const std::string& id : safe_subfolders(root, "module", err)) {
        if (is_tmp(id)) { m.temps.push_back(id); continue; }
        std::string v = variant_of_module(root / "module" / id / "manifest.json");
        if (v.empty()) m.bad.push_back(id);
        else { m.variants.insert(v); m.ids.insert(id); }
    }
    for (const Record& r : read_records(root))
        if (m.ids.count(r.module_id)) m.variants.insert(r.variant);
    return m;
}

}  // namespace

StorageSizes storage_sizes(const fs::path& root) {
    StorageSizes s;
    for (const std::string& v : safe_subfolders(root, "game", s.error))
        s.game.emplace_back(v, part_bytes(root / "game" / v, s.error));
    s.object_cache = safe_part_bytes(root, "cache/obj", s.error);
    s.modules = safe_part_bytes(root, "module", s.error);
    s.build_tmp = safe_part_bytes(root, "build/tmp", s.error);
    s.memcards = safe_part_bytes(root, "memcards", s.error);
    s.total = s.object_cache + s.modules + s.build_tmp + s.memcards;
    for (const auto& g : s.game) s.total += g.second;
    return s;
}

FreedBytes free_up_space(const fs::path& root) {
    return remove_parts(root, {"cache/obj", "build/tmp"});
}

FreedBytes remove_game_data(const fs::path& root) {
    return remove_parts(root, {"game", "module", "cache", "build"});
}

VariantList installed_variants(const fs::path& root) {
    VariantList out;
    Modules m = scan_modules(root, out.error);
    for (const std::string& v : safe_subfolders(root, "game", out.error))
        if (m.variants.count(v)) out.variants.push_back(v);
    return out;
}

namespace {

// Fills `best` from module/<id>/ paired with game/<variant>/ when that module is verified and of `abi_version`.
// A verified module of another ABI sets best.abi_mismatch.
bool pair_module(const fs::path& root, const std::string& id, const std::string& variant, uint32_t abi_version,
                 InstalledModule& best, fs::file_time_type& when) {
    const fs::path manifest_file = root / "module" / id / "manifest.json";
    std::error_code ec;
    if (via_link(root, "module/x/manifest.json") || !fs::is_regular_file(fs::symlink_status(manifest_file, ec))) return false;
    DonModuleManifest manifest;
    std::string why;
    if (!don_read_module_manifest(manifest_file, manifest, why) || !manifest.verified) return false;
    const fs::path game = root / "game" / variant;
    if (!fs::is_directory(fs::symlink_status(game, ec))) return false;
    if (manifest.abi_version != abi_version) { best.abi_mismatch = true; return false; }
    const fs::file_time_type t = fs::last_write_time(manifest_file, ec);
    if (ec) return false;
    // The newest manifest wins; a tie goes to the larger id so the choice never depends on directory order.
    if (best.found && (t < when || (t == when && id < best.module_id))) return false;
    best.found = true;
    best.variant = variant;
    best.module_id = id;
    best.module_dir = root / "module" / id;
    best.game_dir = game;
    when = t;
    return true;
}

}  // namespace

InstalledModule find_installed_module(const fs::path& root, uint32_t abi_version) {
    InstalledModule best;
    fs::file_time_type when{};
    // The most recently prepared edition whose game folder and module are usable plays.
    const std::vector<Record> records = read_records(root);
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        if (via_link(root, "module/x") || fs::is_symlink(fs::symlink_status(root / "module" / it->module_id))) continue;
        if (pair_module(root, it->module_id, it->variant, abi_version, best, when)) {
            best.abi_mismatch = false;
            return best;
        }
    }
    // game/prepared.txt, when it exists, is the only truth (storage.h): a dropped record is never paired again by the
    // manifest. The fallback is for roots made before records.
    std::error_code ec;
    if (fs::exists(fs::symlink_status(root / "game" / "prepared.txt", ec))) return best;
    for (const std::string& id : safe_subfolders(root, "module", best.error)) {
        if (is_tmp(id)) continue;
        DonModuleManifest manifest;
        std::string why;
        if (!don_read_module_manifest((root / "module" / id / "manifest.json"), manifest, why) ||
            !plain_name(manifest.variant))
            continue;
        pair_module(root, id, manifest.variant, abi_version, best, when);
    }
    if (best.found) best.abi_mismatch = false;
    return best;
}

bool record_prepared_edition(const fs::path& root, const std::string& variant, const std::string& module_id) {
    if (!plain_name(variant) || !plain_name(module_id) || variant.find_first_of(" \t\r\n") != std::string::npos ||
        module_id.find_first_of(" \t\r\n") != std::string::npos)
        return false;
    std::error_code ec;
    if (via_link(root, "game/prepared.txt") || fs::is_symlink(fs::symlink_status(root / "game", ec))) return false;
    std::vector<Record> records = read_records(root);
    records.erase(std::remove_if(records.begin(), records.end(), [&](const Record& r) { return r.variant == variant; }),
                  records.end());
    records.push_back({variant, module_id});
    fs::create_directories(root / "game", ec);
    const fs::path tmp = root / "game" / "prepared.txt.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const Record& r : records) out << r.variant << ' ' << r.module_id << '\n';
        out.close();
        if (!out) return false;
    }
    fs::rename(tmp, root / "game" / "prepared.txt", ec);
    return !ec;
}

bool drop_prepared_edition(const fs::path& root, const std::string& variant) {
    if (!plain_name(variant)) return false;
    std::error_code ec;
    if (via_link(root, "game/prepared.txt") || fs::is_symlink(fs::symlink_status(root / "game", ec))) return false;
    std::vector<Record> records = read_records(root);
    const size_t before = records.size();
    records.erase(std::remove_if(records.begin(), records.end(), [&](const Record& r) { return r.variant == variant; }),
                  records.end());
    if (records.size() == before) return true;
    const fs::path tmp = root / "game" / "prepared.txt.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const Record& r : records) out << r.variant << ' ' << r.module_id << '\n';
        out.close();
        if (!out) return false;
    }
    fs::rename(tmp, root / "game" / "prepared.txt", ec);
    return !ec;
}

RepairPlan repair_plan(const fs::path& root) {
    RepairPlan plan;
    for (const char* name : {"game", "module", "cache", "build"}) {
        std::error_code e;
        if (fs::is_symlink(fs::symlink_status(root / name, e))) plan.items.push_back({RepairKind::LinkedFolder, name});
    }
    Modules m = scan_modules(root, plan.error);
    for (const std::string& v : safe_subfolders(root, "game", plan.error))
        if (!m.variants.count(v)) plan.items.push_back({RepairKind::VariantWithoutModule, v});
    for (const std::string& id : m.bad) plan.items.push_back({RepairKind::BadManifest, id});
    for (const std::string& t : m.temps) plan.items.push_back({RepairKind::StaleModuleTemp, t});
    return plan;
}

}  // namespace don_setup
