// SPDX-License-Identifier: GPL-3.0-or-later
// POSIX loader of the game module (T-610). Only built with DON_LINK_GENERATED=OFF: it supplies the don_game_module()
// the PS2Runtime constructor calls (patch 0088), returning the loaded module's table instead of the empty one.
#include "game_module.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "prepare_engine.h"  // sha256_of_file
#include "ps2_runtime.h"

namespace
{
const DonGameModule *g_loaded = nullptr;
const void **g_bind = nullptr;
void *g_handle = nullptr;
std::string g_module_id;  // the folder module's id, empty for a bare .so
std::filesystem::path g_marker;  // module/<id>/running of a folder module, empty for a bare .so

bool loadLibrary(const std::string &path, std::string &error)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        const char *why = dlerror();
        error = "cannot load " + path + ": " + (why != nullptr ? why : "unknown error");
        return false;
    }
    void *symbol = dlsym(handle, kDonGameModuleEntrySymbol);
    if (symbol == nullptr)
    {
        error = path + " does not export " + kDonGameModuleEntrySymbol;
        dlclose(handle);
        return false;
    }
    const DonGameModuleEntry *entry = reinterpret_cast<DonGameModuleEntryFn>(symbol)();
    if (entry == nullptr || entry->module == nullptr)
    {
        error = path + " returned no module entry";
        dlclose(handle);
        return false;
    }
    if (entry->abi_version != kDonGameModuleAbiVersion || entry->struct_size != sizeof(DonGameModuleEntry))
    {
        error = path + " has ABI version " + std::to_string(entry->abi_version) + " (entry size " +
                std::to_string(entry->struct_size) + "), this program needs " +
                std::to_string(kDonGameModuleAbiVersion) + " (entry size " +
                std::to_string(sizeof(DonGameModuleEntry)) + ")";
        dlclose(handle);
        return false;
    }
    if (entry->bind == nullptr)
    {
        error = path + " has no runtime slot to bind (null bind)";
        dlclose(handle);
        return false;
    }
    if (entry->module->entries == nullptr || entry->module->count == 0)
    {
        error = path + " has an empty function table";
        dlclose(handle);
        return false;
    }
    if (entry->iop != nullptr)
    {
        std::string why;
        if (!don_register_iop_descriptor(entry->iop, why))
        {
            error = path + ": " + why;
            dlclose(handle);
            return false;
        }
    }
    g_loaded = entry->module;
    g_bind = entry->bind;
    g_handle = handle;
    std::fprintf(stderr, "[don] game module %s: %u functions%s\n", path.c_str(), entry->module->count,
                 entry->iop != nullptr ? ", IOP native code" : "");
    return true;
}

// E58: the installed module cannot be used; the setup prepares it again.
bool refuse(std::string &error, const std::string &why)
{
    error = "E58 the game module is not usable: " + why;
    return false;
}

bool loadFolder(const std::filesystem::path &folder, std::string &error, bool &previous_crash)
{
    namespace fs = std::filesystem;
    DonModuleManifest manifest;
    std::string why;
    const fs::path manifest_file = folder / kDonModuleManifest;
    if (!don_read_module_manifest(manifest_file, manifest, why))
        return refuse(error, why);
    if (!manifest.verified)
        return refuse(error, "its manifest says it is not verified");
    if (manifest.abi_version != kDonGameModuleAbiVersion)
        return refuse(error, "its manifest has ABI version " + std::to_string(manifest.abi_version) +
                                 ", this program needs " + std::to_string(kDonGameModuleAbiVersion));
    const fs::path library = folder / kDonModuleLibrary;
    const std::string actual = don_setup::sha256_of_file(library);
    if (actual.empty())
        return refuse(error, library.string() + " does not exist or cannot be read");
    if (actual != manifest.module_sha256)
        return refuse(error, library.string() + " has SHA-256 " + actual + ", its manifest says " + manifest.module_sha256);
    const std::string folder_name = folder.filename().string();
    if (manifest.module_id != folder_name)
        return refuse(error, "its manifest names module " + manifest.module_id + " but the folder is " + folder_name);

    const fs::path marker = folder / kDonModuleRunningMarker;
    std::error_code ec;
    previous_crash = fs::exists(marker, ec);
    if (previous_crash)
        std::fprintf(stderr, "[don] E59 the previous run of module %s did not exit cleanly (crash marker %s)\n",
                     folder_name.c_str(), marker.string().c_str());
    if (!loadLibrary(library.string(), error))
        return false;
    g_marker = marker;
    g_module_id = folder_name;
    // E80 takes the manifest's last write time as the module's last use (src/setup/startup.cpp): this load is a use.
    fs::last_write_time(manifest_file, fs::file_time_type::clock::now(), ec);
    if (ec)
        std::fprintf(stderr, "[don] cannot mark module %s as used (%s): %s\n", folder_name.c_str(),
                     manifest_file.string().c_str(), ec.message().c_str());
    return true;
}
} // namespace

bool don_load_game_module(const std::string &path, std::string &error, bool *previous_crash)
{
    if (previous_crash != nullptr)
        *previous_crash = false;
    if (g_loaded != nullptr)
    {
        error = "a game module is already loaded";
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
    {
        // "module/<id>/" and "module/<id>" name the same folder.
        std::filesystem::path folder(path);
        if (!folder.has_filename())
            folder = folder.parent_path();
        bool crashed = false;
        const bool loaded = loadFolder(folder, error, crashed);
        if (previous_crash != nullptr)
            *previous_crash = crashed;
        return loaded;
    }
    return loadLibrary(path, error);
}

bool don_game_module_begin_run()
{
    if (g_marker.empty())
        return true;
    std::ofstream out(g_marker, std::ios::binary | std::ios::trunc);
    out << "running\n";
    out.flush();
    if (!out)
    {
        std::fprintf(stderr, "[don] cannot write the crash marker %s\n", g_marker.string().c_str());
        return false;
    }
    return true;
}

void don_game_module_end_run()
{
    if (g_marker.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(g_marker, ec);
}

void don_unload_game_module()
{
    std::string ignored;
    don_register_iop_descriptor(nullptr, ignored);  // the descriptor points into the library
    if (g_handle != nullptr)
        dlclose(g_handle);
    g_handle = nullptr;
    g_loaded = nullptr;
    g_bind = nullptr;
    g_marker.clear();
    g_module_id.clear();
}

// Called by the PS2Runtime constructor. main() loads the module before any runtime exists; reaching this without one
// is a program error, and an empty table would only fail later, far from the cause.
const DonGameModule &don_game_module()
{
    if (g_loaded == nullptr)
    {
        std::fprintf(stderr, "[don] no game module loaded: this build has no game code (pass --game-module <path>)\n");
        std::fflush(stderr);
        std::abort();
    }
    return *g_loaded;
}

void don_bind_game_module(const void *runtime)
{
    if (g_bind == nullptr)
    {
        std::fprintf(stderr, "[don] no game module loaded: cannot bind the runtime (pass --game-module <path>)\n");
        std::fflush(stderr);
        std::abort();
    }
    *g_bind = runtime;
}

std::string don_game_module_id()
{
    return g_module_id;
}

bool don_game_module_bound()
{
    return g_bind != nullptr && *g_bind != nullptr;
}
