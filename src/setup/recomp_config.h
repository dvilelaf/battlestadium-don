// The recompiler configuration on the player's PC (task T-691). config/recomp.toml is frozen with placeholders; this
// fills them in exactly like `tools/pipeline/freeze_recomp_config.py --render` (the same bytes for the same paths),
// so no Python is needed at the player's. Placeholders: {disc} the program file of the disc, {out} the folder the
// recompiler writes into, {repo} the install root that holds config/. Lines that start with '#' are left alone.
#pragma once

#include <filesystem>
#include <string>

namespace don_setup {

// Text of the frozen config with the three placeholders replaced by absolute, symlink-resolved, '/'-separated paths.
// With `emit_module_abi` the [general] table also gets `emit_module_abi = true` (patch 0091), right after its header
// line, replacing a line that already sets the key. False with `error` when the text has no [general] table.
bool render_recomp_config(const std::string& frozen, const std::filesystem::path& disc,
                          const std::filesystem::path& out, const std::filesystem::path& install_root,
                          bool emit_module_abi, std::string& rendered, std::string& error);

}  // namespace don_setup
