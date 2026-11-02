// Tests of the storage actions (T-643). Everything happens inside a fresh folder under the temp directory.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "storage.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put(const fs::path& p, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(n, 'x');
}

static void put_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}

// The manifest fields the reader requires (module_manifest.h), around `variant_json` (the JSON text of "variant").
static std::string full_manifest(const std::string& variant_json, const std::string& before = "") {
    return "{" + before + "\"program_version\":\"0\",\"abi_version\":2,\"module_id\":\"m\",\"toolchain_hash\":\"t\","
           "\"module_sha256\":\"s\",\"verified\":true,\"variant\":" + variant_json + "}";
}

constexpr size_t kManifestBytes = 300;

// A manifest of exactly kManifestBytes bytes naming `variant` (padding with spaces keeps the JSON valid).
static void put_manifest(const fs::path& p, const std::string& variant) {
    std::string t = full_manifest("\"" + variant + "\"");
    put_text(p, t + std::string(kManifestBytes - t.size(), ' '));
}

// A full root: sizes are 1000/2000 (game), 300 (obj), kManifestBytes+5 (modules), 60 (tmp), 700 (memcards), plus root files.
// `outside` is a folder outside the root; links inside point to it.
static void make_root(const fs::path& root, const fs::path& outside) {
    put(root / "game/us/a.bin", 600);
    put(root / "game/us/sub/b.bin", 400);
    put(root / "game/jp/c.bin", 2000);
    put(root / "cache/obj/x.o", 300);
    put_manifest(root / "module/a1b2/manifest.json", "us");
    put(root / "module/jp.tmp/part.o", 5);
    put(root / "build/tmp/t.txt", 60);
    put(root / "memcards/card1.ps2", 700);
    put(root / "don.ini", 11);
    put(root / "setup.json", 22);
    put(outside / "precious.txt", 5000);
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "cache/obj/link", ec);
    fs::create_directory_symlink(outside, root / "game/us/outlink", ec);
}

// True when a folder link can be made and is seen as one. Not so on Windows without the symlink privilege (or Developer
// Mode), nor under Wine, which creates the link without error but reports an ordinary entry: the tests that need a
// link are skipped there.
static bool can_link(const fs::path& base) {
    std::error_code ec;
    fs::create_directories(base / "can_link_target", ec);
    fs::remove(base / "can_link", ec);
    fs::create_directory_symlink(base / "can_link_target", base / "can_link", ec);
    const bool ok = !ec && fs::is_symlink(base / "can_link", ec) && !ec;
    fs::remove(base / "can_link", ec);
    if (!ok) std::cerr << "storage: link tests skipped, the system does not make usable folder links\n";
    return ok;
}

static void test_sizes(const fs::path& base) {
    fs::path root = base / "sizes", out = base / "sizes_out";
    make_root(root, out);
    StorageSizes s = storage_sizes(root);
    CHECK(s.error == ErrorId::None);
    CHECK(s.game.size() == 2);
    CHECK(s.game.size() == 2 && s.game[0].first == "jp" && s.game[0].second == 2000);
    CHECK(s.game.size() == 2 && s.game[1].first == "us" && s.game[1].second == 1000);
    CHECK(s.object_cache == 300);
    CHECK(s.modules == kManifestBytes + 5);
    CHECK(s.build_tmp == 60);
    CHECK(s.memcards == 700);
    CHECK(s.total == 2000 + 1000 + 300 + kManifestBytes + 5 + 60 + 700);
    CHECK(storage_sizes(base / "nothing").total == 0);
    CHECK(storage_sizes(base / "nothing").error == ErrorId::None);
}

static void test_free_up_space(const fs::path& base) {
    fs::path root = base / "free", out = base / "free_out";
    make_root(root, out);
    FreedBytes f = free_up_space(root);
    CHECK(f.error == ErrorId::None);
    CHECK(f.bytes == 360);
    CHECK(!fs::exists(root / "cache/obj"));
    CHECK(!fs::exists(root / "build/tmp"));
    CHECK(fs::exists(root / "game/us/a.bin"));
    CHECK(fs::exists(root / "module/a1b2/manifest.json"));
    CHECK(fs::file_size(root / "memcards/card1.ps2") == 700);
    CHECK(fs::exists(root / "don.ini") && fs::exists(root / "setup.json"));
    CHECK(fs::file_size(out / "precious.txt") == 5000);
    CHECK(free_up_space(root).bytes == 0);   // second run: nothing left, no error
}

static void test_remove_game_data(const fs::path& base) {
    fs::path root = base / "remove", out = base / "remove_out";
    make_root(root, out);
    FreedBytes f = remove_game_data(root);
    CHECK(f.error == ErrorId::None);
    CHECK(f.bytes == 3000 + 300 + kManifestBytes + 5 + 60);
    CHECK(!fs::exists(root / "game") && !fs::exists(root / "module"));
    CHECK(!fs::exists(root / "cache") && !fs::exists(root / "build"));
    CHECK(fs::file_size(root / "memcards/card1.ps2") == 700);
    CHECK(fs::file_size(root / "don.ini") == 11 && fs::file_size(root / "setup.json") == 22);
    CHECK(fs::file_size(out / "precious.txt") == 5000);   // the link target survived
}

static void test_symlinked_part(const fs::path& base) {
    if (!can_link(base)) return;
    fs::path root = base / "linkpart", out = base / "linkpart_out";
    put(out / "obj/keep.o", 77);
    fs::create_directories(root / "cache");
    std::error_code ec;
    fs::create_directory_symlink(out / "obj", root / "cache/obj", ec);
    CHECK(storage_sizes(root).object_cache == 0);
    CHECK(free_up_space(root).error == ErrorId::None);
    CHECK(!fs::is_symlink(root / "cache/obj") && fs::file_size(out / "obj/keep.o") == 77);
}

static void test_installed_and_repair(const fs::path& base) {
    fs::path root = base / "repair", out = base / "repair_out";
    make_root(root, out);
    put(root / "game/eu/d.bin", 1);
    put(root / "module/c3d4/other.o", 1);   // module folder without manifest.json: a broken module
    put_manifest(root / "module/e5f6/manifest.json", "us");   // second module for "us": listed once
    put_manifest(root / "module/0a0a/manifest.json", "xx");   // names a variant that has no game folder
    VariantList v = installed_variants(root);
    CHECK(v.error == ErrorId::None);
    CHECK(v.variants == std::vector<std::string>{"us"});
    RepairPlan p = repair_plan(root);
    CHECK(p.error == ErrorId::None);
    CHECK(p.items.size() == 4);
    bool eu = false, jp = false, tmp = false, bad = false;
    for (const RepairItem& i : p.items) {
        eu |= i.kind == RepairKind::VariantWithoutModule && i.name == "eu";
        jp |= i.kind == RepairKind::VariantWithoutModule && i.name == "jp";
        tmp |= i.kind == RepairKind::StaleModuleTemp && i.name == "jp.tmp";
        bad |= i.kind == RepairKind::BadManifest && i.name == "c3d4";
    }
    CHECK(eu && jp && tmp && bad);
    CHECK(fs::exists(root / "module/jp.tmp/part.o") && fs::exists(root / "game/eu/d.bin"));   // nothing changed
    CHECK(repair_plan(base / "nothing").items.empty());
    CHECK(installed_variants(base / "nothing").variants.empty());
}

// T-692: the module the game starts from: verified, with its game folder, the newest first.
constexpr uint32_t kAbi = 2;   // the abi_version full_manifest writes

static void put_manifest_at(const fs::path& root, const std::string& id, const std::string& variant, bool verified,
                            int age_seconds, int abi = kAbi) {
    std::string t = full_manifest("\"" + variant + "\"");
    t.replace(t.find("\"abi_version\":2"), 15, "\"abi_version\":" + std::to_string(abi));
    if (!verified) t.replace(t.find("\"verified\":true"), 15, "\"verified\":false");
    const fs::path f = root / "module" / id / "manifest.json";
    put_text(f, t);
    std::error_code ec;
    fs::last_write_time(f, fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds), ec);
}

static void test_find_installed_module(const fs::path& base) {
    const fs::path root = base / "find";
    CHECK(!find_installed_module(root, kAbi).found);                       // nothing at all
    fs::create_directories(root / "game/us");
    CHECK(!find_installed_module(root, kAbi).found);                       // a game folder without a module
    put_manifest_at(root, "aaaa", "us", true, 10);
    InstalledModule one = find_installed_module(root, kAbi);
    CHECK(one.found && one.variant == "us" && one.module_id == "aaaa");
    CHECK(one.module_dir == root / "module/aaaa" && one.game_dir == root / "game/us");
    put_manifest_at(root, "bbbb", "us", true, 5);                    // newer
    put_manifest_at(root, "cccc", "us", false, 1);                   // newest, but not verified: ignored
    CHECK(find_installed_module(root, kAbi).module_id == "bbbb");
    put_manifest_at(root, "dddd", "jp", true, 0);                    // newest, but no game/jp/ folder: ignored
    CHECK(find_installed_module(root, kAbi).module_id == "bbbb");
    fs::create_directories(root / "module/eeee.tmp");                // an interrupted build is never chosen
    CHECK(find_installed_module(root, kAbi).module_id == "bbbb");

    // Another ABI: a newer verified module built for it never wins, the older matching one is chosen.
    put_manifest_at(root, "ffff", "us", true, 0, kAbi + 1);
    InstalledModule matching = find_installed_module(root, kAbi);
    CHECK(matching.found && matching.module_id == "bbbb" && !matching.abi_mismatch);

    // Only mismatched modules: not found, and the caller is told why.
    const fs::path other = base / "find_abi";
    fs::create_directories(other / "game/us");
    put_manifest_at(other, "aaaa", "us", true, 3, kAbi + 1);
    InstalledModule stale = find_installed_module(other, kAbi);
    CHECK(!stale.found && stale.abi_mismatch);
    CHECK(find_installed_module(other, kAbi + 1).found);
}

// T-697: once game/prepared.txt exists its records are the only truth; the manifest fallback is for roots without it.
static void test_records_are_authoritative(const fs::path& base) {
    const fs::path root = base / "authoritative";
    fs::create_directories(root / "game/jp");
    put_manifest_at(root, "aaaa", "jp", true, 10);
    CHECK(find_installed_module(root, kAbi).found);   // no file: the manifest's variant pairs
    CHECK(record_prepared_edition(root, "jp", "aaaa"));
    CHECK(find_installed_module(root, kAbi).found);
    // An interrupted re-extract: the record was dropped, the folder is half copied. It must not pair again.
    CHECK(drop_prepared_edition(root, "jp"));
    CHECK(fs::is_regular_file(root / "game/prepared.txt"));   // left in place, empty
    CHECK(!find_installed_module(root, kAbi).found);
}

// T-693: one module serves every edition; the edition that plays is the most recently prepared one.
static void test_editions_share_a_module(const fs::path& base) {
    const fs::path root = base / "editions";
    fs::create_directories(root / "game/jp");
    put_manifest_at(root, "aaaa", "jp", true, 10);   // built while preparing "jp"
    CHECK(find_installed_module(root, kAbi).variant == "jp");   // no record yet: the manifest's variant

    fs::create_directories(root / "game/en-jpvoice");
    CHECK(record_prepared_edition(root, "jp", "aaaa"));
    CHECK(record_prepared_edition(root, "en-jpvoice", "aaaa"));   // reused, not rebuilt
    InstalledModule m = find_installed_module(root, kAbi);
    CHECK(m.found && m.module_id == "aaaa" && m.variant == "en-jpvoice" && m.game_dir == root / "game/en-jpvoice");
    // Both editions are installed and neither is offered for repair.
    CHECK((installed_variants(root).variants == std::vector<std::string>{"en-jpvoice", "jp"}));
    CHECK(repair_plan(root).items.empty());

    CHECK(record_prepared_edition(root, "jp", "aaaa"));   // prepared again: jp is the most recent now
    CHECK(find_installed_module(root, kAbi).variant == "jp");
    CHECK(record_prepared_edition(root, "jp", "aaaa"));   // the same edition twice leaves one record
    {   // closed before the next record: Windows does not replace or delete a file that is open
        std::ifstream in(root / "game/prepared.txt");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text == "en-jpvoice aaaa\njp aaaa\n");
    }

    // The recorded edition lost its game folder, or its module is not usable: the next most recent one plays.
    fs::remove_all(root / "game/jp");
    CHECK(find_installed_module(root, kAbi).variant == "en-jpvoice");
    CHECK(!find_installed_module(root, kAbi + 1).found);   // another ABI: no module, whatever the records say

    // Re-preparing drops the record first, so an interrupted extract leaves no half-copied edition marked playable.
    CHECK(record_prepared_edition(root, "jp", "aaaa"));
    fs::create_directories(root / "game/jp");
    CHECK(drop_prepared_edition(root, "jp"));
    {
        std::ifstream in2(root / "game/prepared.txt");
        std::string text2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        CHECK(text2 == "en-jpvoice aaaa\n");
    }
    CHECK(drop_prepared_edition(root, "never-recorded"));   // nothing to drop is not a failure
    CHECK(!drop_prepared_edition(root, "../x"));

    // A bad name is not recorded.
    CHECK(!record_prepared_edition(root, "../x", "aaaa") && !record_prepared_edition(root, "jp", "a b"));
    // The records go with the game data.
    remove_game_data(root);
    CHECK(!fs::exists(root / "game"));
}

static void test_manifest_parsing(const fs::path& base) {
    fs::path root = base / "manifest";
    for (const char* v : {"a", "b", "c", "d", "e", "f"}) put(root / "game" / v / "f", 1);
    put_text(root / "module/m1/manifest.json", full_manifest("\"a\"", R"("id":"m1","files":[1,2,{"x":"y"}],"n":-1.5e3,"t":true,)"));
    put_text(root / "module/m2/manifest.json", full_manifest(R"("b\u0063")"));     // escape: decodes to "bc", no game/bc
    std::string trailing_comma = full_manifest("\"c\"");
    trailing_comma.insert(trailing_comma.size() - 1, ",");
    put_text(root / "module/m3/manifest.json", trailing_comma);   // trailing comma: strict, rejected
    put_text(root / "module/m4/manifest.json", full_manifest("\"d\"") + " trailing");   // garbage after the object
    put_text(root / "module/m5/manifest.json", full_manifest("\"../e\""));   // not a plain name
    put_text(root / "module/m6/manifest.json", full_manifest("7"));   // not a string
    put_text(root / "module/m7/manifest.json", "");                              // empty
    put_text(root / "module/m8/manifest.json", R"({"nested":{"variant":"f"}})"); // only the top level counts
    VariantList v = installed_variants(root);
    CHECK(v.variants == std::vector<std::string>{"a"});
    RepairPlan p = repair_plan(root);
    int bad = 0, nomod = 0;
    for (const RepairItem& i : p.items) {
        bad += i.kind == RepairKind::BadManifest;
        nomod += i.kind == RepairKind::VariantWithoutModule;
    }
    CHECK(bad == 6);     // m3 m4 m5 m6 m7 m8
    CHECK(nomod == 5);   // b c d e f
}

static void test_linked_parent(const fs::path& base, const char* parent, const char* leaf) {
    if (!can_link(base)) return;
    fs::path root = base / (std::string("lp_") + parent), out = base / (std::string("lp_out_") + parent);
    put(out / leaf / "x", 99);
    put(root / "memcards/card1.ps2", 700);
    std::error_code ec;
    fs::create_directories(root);
    fs::create_directory_symlink(out, root / parent, ec);
    StorageSizes s = storage_sizes(root);
    CHECK(s.object_cache == 0 && s.build_tmp == 0 && s.total == 700);
    FreedBytes f = free_up_space(root);
    CHECK(f.error == ErrorId::None && f.bytes == 0);
    CHECK(f.blocked.size() == 1 && f.blocked[0] == parent);
    CHECK(fs::is_symlink(root / parent));
    CHECK(fs::file_size(out / leaf / "x") == 99);
    RepairPlan p = repair_plan(root);
    CHECK(p.items.size() == 1 && p.items[0].kind == RepairKind::LinkedFolder && p.items[0].name == parent);
    FreedBytes r = remove_game_data(root);
    CHECK(r.error == ErrorId::None);
    CHECK(!fs::is_symlink(root / parent) && !fs::exists(root / parent));   // the link itself is gone
    CHECK(fs::file_size(out / leaf / "x") == 99);                            // its target is not
    CHECK(fs::file_size(root / "memcards/card1.ps2") == 700);
}

static void test_linked_game_and_module(const fs::path& base) {
    if (!can_link(base)) return;
    for (const char* parent : {"game", "module"}) {
        fs::path root = base / (std::string("lg_") + parent), out = base / (std::string("lg_out_") + parent);
        put(out / "us/x", 50);
        put_manifest(out / "m/manifest.json", "us");
        std::error_code ec;
        fs::create_directories(root);
        fs::create_directory_symlink(out, root / parent, ec);
        CHECK(storage_sizes(root).game.empty() && storage_sizes(root).modules == 0);
        CHECK(installed_variants(root).variants.empty());
        remove_game_data(root);
        CHECK(!fs::exists(root / parent) && fs::file_size(out / "us/x") == 50);
    }
}

static void test_read_only(const fs::path& base) {
    fs::path root = base / "ro", out = base / "ro_out";
    make_root(root, out);
#ifdef _WIN32
    return;   // skipped on Windows: the read-only attribute of a directory does not stop deleting the files in it
#endif
    if (geteuid() == 0) return;   // root ignores permissions
    fs::permissions(root / "build/tmp", fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                                            fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
    FreedBytes f = free_up_space(root);
    CHECK(f.error == ErrorId::E32);
    CHECK(fs::exists(root / "build/tmp/t.txt"));
    CHECK(fs::file_size(root / "memcards/card1.ps2") == 700);
    FreedBytes r = remove_game_data(root);
    CHECK(r.error == ErrorId::E32);
    fs::permissions(root / "build/tmp", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                            fs::perms::others_read | fs::perms::others_exec);
    CHECK(remove_game_data(root).error == ErrorId::None);
    CHECK(fs::file_size(root / "memcards/card1.ps2") == 700);
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_storage_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    test_sizes(tmp);
    test_free_up_space(tmp);
    test_remove_game_data(tmp);
    test_symlinked_part(tmp);
    test_installed_and_repair(tmp);
    test_manifest_parsing(tmp);
    test_find_installed_module(tmp);
    test_editions_share_a_module(tmp);
    test_records_are_authoritative(tmp);
    test_linked_parent(tmp, "cache", "obj");
    test_linked_parent(tmp, "build", "tmp");
    test_linked_game_and_module(tmp);
    test_read_only(tmp);
    std::error_code ec;
    fs::permissions(tmp / "ro/build/tmp", fs::perms::owner_all, ec);
    fs::remove_all(tmp, ec);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
