// SPDX-License-Identifier: GPL-3.0-or-later
// The one door between the program and a separately built game module (docs/tasks/M6-public-release.md, A1).
// Shared by both sides: the module (src/game_module/module_entry.cpp, built with DON_GAME_MODULE_BUILD defined and no
// C++ library) and the loader (src/host/game_module_posix.cpp).
//
// The module exports one extern "C" function, kDonGameModuleEntrySymbol. It is not called "don_game_module": inside the
// module the generated register_functions.cpp already defines the C++ function don_game_module() (patch 0088), and a
// C-linkage and a C++-linkage function of the same name and parameters cannot be declared in one translation unit.
#pragma once

#include <stdint.h>

// Defined by ps2_runtime.h on the host side; the entry only carries a pointer to it.
struct DonGameModule;
// src/game_module/don_iop_abi.h: the translated IOP code of the module, when it has one.
struct DonIopDescriptor;

// Version 2 (T-648a) adds `iop` to the entry. A module of version 1 is refused.
constexpr uint32_t kDonGameModuleAbiVersion = 2;
constexpr const char *kDonGameModuleEntrySymbol = "don_game_module_v1";

struct DonGameModuleEntry
{
    uint32_t abi_version;  // kDonGameModuleAbiVersion the module was built with
    uint32_t struct_size;  // sizeof(DonGameModuleEntry) in the module, a sanity check by the loader
    const DonGameModule *module;
    // Address of the module's one global slot (`don_rt` in src/game_module/don_guest_abi.h, a `const DonRuntime *`).
    // The host stores the address of its DonRuntime there, once, before the first guest instruction.
    const void **bind;
    // Nullable. The translated IOP sound module (patch 0085); the loader registers it with the IOP emulator through
    // src/host/iop_native_bridge.cpp and refuses a module whose descriptor is malformed.
    const DonIopDescriptor *iop;
};

using DonGameModuleEntryFn = const DonGameModuleEntry *(*)();

#ifndef DON_GAME_MODULE_BUILD
#include <string>

#include "../setup/module_manifest.h"  // DonModuleManifest and its one reader (relative: every includer finds it)

// T-731: the module's file name per platform. A local constant: src/setup/module_platform.h (t-730) is not on main yet.
#ifdef _WIN32
constexpr const char *kDonModuleLibrary = "don_game.dll";
#else
constexpr const char *kDonModuleLibrary = "libdon_game.so";
#endif
constexpr const char *kDonModuleManifest = "manifest.json";
constexpr const char *kDonModuleRunningMarker = "running";

// Loads the module at `path`: a .so (developer use) or a module folder module/<id>/ holding libdon_game.so and
// manifest.json. A folder is checked in this order, any failure is an "E58: ..." error: the manifest exists and
// parses; verified; the ABI version is the program's; the .so's SHA-256 is the manifest's; the id is the folder name.
// Then the .so is loaded (dlopen RTLD_NOW|RTLD_LOCAL), its entry looked up and the ABI version and table checked.
// On failure returns false and `error` says why. The library stays loaded until don_unload_game_module.
// A folder that still holds the "running" marker was left by a crash: a "[don] E59" line is printed, `previous_crash`
// (when given) is set to true, and loading goes on.
bool don_load_game_module(const std::string &path, std::string &error, bool *previous_crash = nullptr);

// Writes module/<id>/running (a folder module only); call it just before the first guest instruction.
bool don_game_module_begin_run();
// Removes that marker (and nothing else); call it on a clean exit.
void don_game_module_end_run();
// Registers a module's IOP descriptor with the IOP emulator (src/host/iop_native_bridge.cpp). False with `error` set
// when the descriptor is malformed; nullptr unregisters. Called by the loader before any emulator exists.
bool don_register_iop_descriptor(const DonIopDescriptor *descriptor, std::string &error);

// Closes the module. The runtime must not use its table afterwards (tests only).
void don_unload_game_module();

// Hands the loaded module the host's DonRuntime (src/host/don_runtime_bridge.cpp). Called once, when the runtime
// exists and before any guest code runs. Prints a [don] message and aborts when no module is loaded.
void don_bind_game_module(const void *runtime);

// True once the module's bind slot holds the host's runtime. The bind runs from loadELF (an override descriptor), so
// it is checked after loadELF: a null slot means that descriptor did not run and the guest would crash silently.
bool don_game_module_bound();

// The loaded folder module's id (module/<id>/); empty when no module is loaded or it is a bare .so.
std::string don_game_module_id();
#endif
