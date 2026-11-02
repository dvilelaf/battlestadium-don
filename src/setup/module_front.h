// Front half of "prepare" (task T-691): the player's disc image -> checked game files -> the generated C++ tree that the
// module build (T-690) compiles, with no Python. One function per stage so the engine can journal each of them:
//   1. identify_stage   the image is the game (existing error ids otherwise);
//   2. extract_stage    the allow-listed files go to <root>/game/<variant>/;
//   3. config_stage     config/recomp.toml with its placeholders filled in and general.emit_module_abi = true;
//   4. recompile_stage  the bundled ps2_recomp writes <root>/build/tmp/gen (exit codes of patch 0089 -> E51);
//   4b. iop_native_stage  (T-694) the IOP sound module cri_adxi of the extracted files is translated to
//                       <gen>/iop_native/adxi_native.cpp (port of tools/iop_recomp/make_native.py; skipped, and
//                       said so, when the module is not on the disc; E51 when it cannot be translated);
//   5. verify_stage     tree_hash of that folder (the IOP file included) against [generated] module_abi_sha256 of
//                       config/module_reference.toml (E52 on a mismatch).
// Nothing is thrown; every stage answers with a StageResult. `root` is the user's bulk folder (storage.h layout),
// `install_root` the folder that holds config/.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "disc_image.h"
#include "setup_types.h"

namespace don_setup {

struct StageResult {
    bool ok = true;
    ErrorId error = ErrorId::None;
    std::string detail;  // what went wrong, for the report; never game data
};

struct FrontPaths {
    std::filesystem::path root;          // user folder: game/, build/tmp/
    std::filesystem::path install_root;  // holds config/
    std::filesystem::path recomp;        // the bundled ps2_recomp
};

struct FrontOutput {
    DiscIdentity identity;
    std::filesystem::path game_dir;    // <root>/game/<variant>
    std::filesystem::path config_file; // rendered recompiler config
    std::filesystem::path gen_dir;     // <root>/build/tmp/gen
    std::string tree_hash;             // of gen_dir, once recompiled
    int recomp_exit = -1;              // exit code of ps2_recomp (-1: it did not run to an exit)
    std::string iop_native_note;       // what iop_native_stage did: counts, or why it skipped (never game data)
};

// Folder the stages use: <root>/build/tmp/gen, and the rendered config next to it.
std::filesystem::path front_gen_dir(const std::filesystem::path& root);

StageResult identify_stage(const std::filesystem::path& image, const ProgressFn& progress, FrontOutput& out);
StageResult extract_stage(const std::filesystem::path& image, const FrontPaths& paths, const ProgressFn& progress,
                          FrontOutput& out);
StageResult config_stage(const FrontPaths& paths, FrontOutput& out);
// `timeout_ms` bounds the recompiler (0: none). Progress is parsed from the "[progress] <done>/<total>" lines.
StageResult recompile_stage(const FrontPaths& paths, long timeout_ms, const ProgressFn& progress, FrontOutput& out);
StageResult iop_native_stage(const FrontPaths& paths, FrontOutput& out);
StageResult verify_stage(const FrontPaths& paths, FrontOutput& out);

// The IRX of cri_adxi under <game_dir>/MODULES: the entry of 1000.FPK, else the loose CRI_ADXI.IRX. `found` false and
// `ok` true: it is not on the disc. False: the pack entry could not be read.
bool find_adxi_irx(const std::filesystem::path& game_dir, std::vector<uint8_t>& irx, bool& found);

// Stages 1 to 5; stops at the first failure.
StageResult run_front(const std::filesystem::path& image, const FrontPaths& paths, long recomp_timeout_ms,
                      const ProgressFn& progress, FrontOutput& out);

// "[progress] 3000/14000" -> 3000, 14000. False for any other line.
bool parse_progress_line(const std::string& line, unsigned long& done, unsigned long& total);
// [generated] module_abi_sha256 of config/module_reference.toml's text; empty when absent.
std::string parse_module_reference(const std::string& toml_text);

}  // namespace don_setup
