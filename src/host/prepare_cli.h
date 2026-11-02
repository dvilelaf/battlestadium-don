// SPDX-License-Identifier: GPL-3.0-or-later
// `don --prepare <image>` (task T-692): from the player's disc image to a prepared game module, on the terminal.
// The front half (src/setup/module_front.*) and the build half (src/setup/module_build.*) run behind the setup
// engine's journal in the user folder, so a killed run resumes where it stopped and a finished one ends at once.
//
// Install layout, everything relative to the folder of the `don` executable (no absolute path anywhere):
//   don[.exe]            the program, built without linked game code (-DDON_LINK_GENERATED=OFF)
//   gs/ vu/ spu2/        the PCSX2 libraries (found by the usual RUNPATH / --gs-dir rules)
//   bin/ps2_recomp[.exe] the recompiler
//   config/              recomp.toml, disc_manifest.toml, module_reference.toml, ... (the folder the recompiler config
//                        placeholder {repo} points to)
//   sdk/                 what the generated code includes: game_module/ (don_guest_*.h, iop_shim/, module_entry.cpp),
//                        host/game_module.h, inc/don_guest_macros.h (see src/setup/module_build.h)
//   toolchain/           clang++ and lld: bin/clang++, bin/ld.lld, lib/clang/<n>/include, TOOLCHAIN.sha256
//
// Environment: DON_PREPARE_JOBS=<n> caps the parallel compile jobs of the build half (default 0: all logical
// cores); it exists so a developer can keep a busy machine responsive. There is no --until:
// --prepare always runs every stage.
// tools/module/stage_install.py lays this out from a build.
//
// Editions (T-693): the three editions of config/discs.toml carry one program file, so --prepare <image> of a second
// edition extracts its game/<variant>/ and reuses the installed module (no recompile, no build): any verified module of
// this ABI, this program version and this install's toolchain serves it (the tree is the same by construction).
// Each finished prepare is recorded in game/prepared.txt (src/setup/storage.h); the edition
// prepared last is the one `don` plays. A run whose image differs from the last one (path, size, modification time:
// build/tmp/front/image.key) starts over instead of resuming.
#pragma once

#include <filesystem>
#include <string>

struct DonHostOptions;

namespace don_host {

struct InstallLayout {
    std::filesystem::path root;       // folder of the executable
    std::filesystem::path recomp;     // bin/ps2_recomp
    std::filesystem::path config;     // config/
    std::filesystem::path sdk;        // sdk/
    std::filesystem::path toolchain;  // toolchain/
};

InstallLayout install_layout(const std::filesystem::path& program_folder);
// The folder of the running executable (SDL_GetBasePath, else the current directory).
std::filesystem::path program_folder_of_executable();
// Names of the layout's missing parts ("bin/ps2_recomp", ...); empty when the install is complete.
std::string missing_install_parts(const InstallLayout& layout);

// Runs --prepare. Exit code: 0 prepared, 1 the preparation failed (error id and text printed), 2 the install or the
// user folder is unusable.
int run_prepare(const std::string& image, const InstallLayout& layout);

}  // namespace don_host
