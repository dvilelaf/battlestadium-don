// Storage actions behind the Settings rows (S6): sizes, FREE UP SPACE, REMOVE GAME DATA, the installed variants and
// the REPAIR plan. Layout of the bulk root (docs/tasks/M6-public-release.md, A2):
//   game/<variant>/   cache/obj/   module/<id>/manifest.json   build/tmp/   memcards/
// `memcards/` is never touched by any cleanup. A symlink is never followed: it is counted as zero bytes and removed
// as a link, and no path is ever resolved through a link: if a folder on the way (cache, build, game, module) is a
// symlink, nothing beneath it is measured or removed and it is reported (`blocked`, or a LinkedFolder repair item).
// A module is matched to a variant by its manifest: module/<id>/manifest.json (<id> is a hash) carries a string field
// "variant"; game/<variant>/ is installed when some manifest names it, or a record names it with a module whose
// manifest is valid (editions, below).
// Errors come back as an ErrorId (E32 when a part cannot be read or removed); nothing is thrown.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "setup_types.h"

namespace don_setup {

struct StorageSizes {
    std::vector<std::pair<std::string, uint64_t>> game;   // bytes per variant, sorted by name
    uint64_t object_cache = 0;                            // cache/obj/
    uint64_t modules = 0;                                 // module/
    uint64_t build_tmp = 0;                               // build/tmp/
    uint64_t memcards = 0;                                // memcards/
    uint64_t total = 0;                                   // sum of every part above
    ErrorId error = ErrorId::None;
};

struct FreedBytes {
    uint64_t bytes = 0;   // bytes that are gone, also when an error stopped the work part-way
    std::vector<std::string> blocked;   // parts not touched because a folder on the way is a symlink, e.g. "cache"
    ErrorId error = ErrorId::None;
};

enum class RepairKind {
    VariantWithoutModule,   // game/<name>/ is named by no manifest
    StaleModuleTemp,        // module/<name>.tmp/ left behind by an interrupted build
    BadManifest,            // module/<name>/manifest.json is missing, unreadable or has no valid "variant"
    LinkedFolder,           // game, module, cache or build is a symlink: left alone
};

struct RepairItem {
    RepairKind kind;
    std::string name;   // variant, or the folder name including ".tmp"
};

struct RepairPlan {
    std::vector<RepairItem> items;   // empty: nothing to repair
    ErrorId error = ErrorId::None;
};

struct VariantList {
    std::vector<std::string> variants;   // sorted
    ErrorId error = ErrorId::None;
};

// The module the game starts from (T-692): module/<id>/ whose manifest is valid, `verified`, and names a variant whose
// game/<variant>/ folder exists and whose abi_version equals the program's (a module of another ABI is skipped, and
// reported through abi_mismatch when nothing else matches). Several: the newest manifest wins. Never a .tmp folder, never through a symlink.
struct InstalledModule {
    bool found = false;
    std::string variant;
    std::string module_id;
    std::filesystem::path module_dir;   // <root>/module/<id>
    std::filesystem::path game_dir;     // <root>/game/<variant>
    bool abi_mismatch = false;          // not found, but a verified module exists for another ABI version
    ErrorId error = ErrorId::None;
};

InstalledModule find_installed_module(const std::filesystem::path& root, uint32_t abi_version);

// Editions (T-693). Every edition of the game (jp, en-jpvoice, en-envoice: config/discs.toml) carries the same program
// file (SLPS_256.75, one SHA-256), so they generate the same tree and one module serves all of them: the module id
// is a hash of that tree, the toolchain and the ABI. Preparing a second edition extracts its game/<variant>/ and
// reuses the module; nothing is recompiled. The manifest's "variant" field is the edition the module was first built
// for: it pairs a root only when game/prepared.txt does not exist (roots made before records). Once that file
// exists, even empty, its records are the only truth.
// `<root>/game/prepared.txt` has one line "<variant> <module id>" per prepared edition, oldest first; preparing an
// edition again moves its line to the end. The last line whose game/<variant>/ exists and whose module is verified
// and of the program's ABI is the edition that plays (find_installed_module). A variant named by a record whose module
// has a valid manifest counts as installed (installed_variants, repair_plan). The file lives in game/ so that
// removing the game data removes it. False when a name is not a plain name or the file cannot be written.
bool record_prepared_edition(const std::filesystem::path& root, const std::string& variant, const std::string& module_id);

// Removes the record of one edition (the other lines stay). Called before the edition's game/<variant>/ is extracted
// again, so that an interrupted extract never leaves a half-copied edition marked as playable. True when the record is
// gone (also when there was none; the file stays, empty when no line is left, so the manifest fallback stays off); false for a name that is not a plain name or when the file cannot be rewritten.
bool drop_prepared_edition(const std::filesystem::path& root, const std::string& variant);

StorageSizes storage_sizes(const std::filesystem::path& root);
FreedBytes free_up_space(const std::filesystem::path& root);     // removes cache/obj and build/tmp only
FreedBytes remove_game_data(const std::filesystem::path& root);  // removes game, module, cache, build
VariantList installed_variants(const std::filesystem::path& root);
RepairPlan repair_plan(const std::filesystem::path& root);       // read only

}  // namespace don_setup
