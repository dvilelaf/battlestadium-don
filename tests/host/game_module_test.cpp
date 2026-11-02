// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test of the POSIX game module loader (src/host/game_module_posix.cpp, T-610). No game and no disc: the modules
// are the fixtures of game_module_fixture.cpp, built by CMake into DON_FIXTURE_DIR, one per failure.
// Exit code 0 when every check passes.
#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "game_module.h"
#include "prepare_engine.h"
#include "ps2_runtime.h"

#ifndef DON_FIXTURE_DIR
#error "DON_FIXTURE_DIR must name the folder of the fixture modules (tests/host/CMakeLists.txt)"
#endif

namespace
{
int g_failures = 0;

std::string fixture(const char *name)
{
    return std::string(DON_FIXTURE_DIR) + "/" + name;
}

void report(bool ok, const std::string &what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

// The load must fail, and the error must say why (`needle` is a part of that message).
void expectRefused(const char *what, const std::string &path, const std::string &needle)
{
    std::string error;
    const bool loaded = don_load_game_module(path, error);
    const bool said = error.find(needle) != std::string::npos;
    report(!loaded && said, std::string(what) + ": " + (loaded ? "loaded" : error));
}

std::string abiNeedle(uint32_t abi, size_t entry_size)
{
    return "has ABI version " + std::to_string(abi) + " (entry size " + std::to_string(entry_size) + ")";
}

namespace fs = std::filesystem;

std::string manifestText(const std::string &id, const std::string &sha, uint32_t abi, const char *verified)
{
    return "{\"program_version\":\"0.0-test\",\"abi_version\":" + std::to_string(abi) + ",\"module_id\":\"" + id +
           "\",\"input_hashes\":{\"elf\":\"00\",\"flags\":[1,2.5e1,null]},\"toolchain_hash\":\"tc\","
           "\"module_sha256\":\"" + sha + "\",\"verified\":" + verified + ",\"variant\":\"jp\"}";
}

// module/<id>/ under `root` with the ok fixture as libdon_game.so; `manifest` is written as is unless empty.
fs::path makeFolder(const fs::path &root, const std::string &id, const std::string &manifest)
{
    const fs::path dir = root / id;
    fs::create_directories(dir);
    fs::copy_file(fixture("libfx_ok.so"), dir / kDonModuleLibrary, fs::copy_options::overwrite_existing);
    if (!manifest.empty())
        std::ofstream(dir / kDonModuleManifest, std::ios::binary) << manifest;
    return dir;
}

// T-648a: the IOP descriptor of ABI 2. Nothing is loaded when the descriptor is bad.
void iopTests()
{
    std::string error;
    report(don_load_game_module(fixture("libfx_iop.so"), error) && error.empty(), "module with an IOP descriptor loads");
    don_unload_game_module();
    report(don_load_game_module(fixture("libfx_ok.so"), error), "module without an IOP descriptor still loads");
    don_unload_game_module();
    expectRefused("IOP descriptor with the wrong RAM size", fixture("libfx_iopram.so"), "IOP descriptor");
    expectRefused("IOP descriptor with an entry pc outside its code", fixture("libfx_ioppc.so"), "outside its code range");
}

void folderTests()
{
    const fs::path root = fs::temp_directory_path() / ("don_game_module_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    const std::string sha = don_setup::sha256_of_file(fixture("libfx_ok.so"));
    report(sha.size() == 64, "fixture SHA-256 is computed");

    expectRefused("folder without manifest", makeFolder(root, "nomanifest", "").string(), "E58");
    expectRefused("folder without manifest says why", (root / "nomanifest").string(), "does not exist");
    expectRefused("corrupt manifest", makeFolder(root, "corrupt", "{\"module_id\": \"corrupt\", ").string(), "is corrupt");
    expectRefused("manifest with a wrong type", makeFolder(root, "badtype", manifestText("badtype", sha, kDonGameModuleAbiVersion, "\"yes\"")).string(),
                  "wrong type");
    expectRefused("not verified", makeFolder(root, "unverified", manifestText("unverified", sha, kDonGameModuleAbiVersion, "false")).string(),
                  "not verified");
    expectRefused("wrong ABI in the manifest",
                  makeFolder(root, "badabi", manifestText("badabi", sha, kDonGameModuleAbiVersion + 1, "true")).string(),
                  "ABI version " + std::to_string(kDonGameModuleAbiVersion + 1));
    expectRefused("wrong SHA-256", makeFolder(root, "badsha", manifestText("badsha", std::string(64, '0'), kDonGameModuleAbiVersion, "true")).string(),
                  "has SHA-256 " + sha);
    expectRefused("id differs from the folder", makeFolder(root, "otherdir", manifestText("someid", sha, kDonGameModuleAbiVersion, "true")).string(),
                  "names module someid but the folder is otherdir");
    report(!fs::exists(root / "badsha" / kDonModuleRunningMarker), "a refused folder gets no crash marker");

    // A good folder (with and without a trailing slash) loads; the crash marker follows the run.
    const fs::path good = makeFolder(root, "goodid", manifestText("goodid", sha, kDonGameModuleAbiVersion, "true"));
    std::string error;
    bool crashed = true;
    report(don_load_game_module(good.string() + "/", error, &crashed) && !crashed, "good folder loads with no E59: " + error);
    report(!fs::exists(good / kDonModuleRunningMarker), "no marker before the run starts");
    report(don_game_module_begin_run() && fs::exists(good / kDonModuleRunningMarker), "marker written when the run starts");
    don_game_module_end_run();
    report(!fs::exists(good / kDonModuleRunningMarker), "marker removed on a clean exit");
    report(fs::exists(good / kDonModuleManifest) && fs::exists(good / kDonModuleLibrary), "nothing else was deleted");
    don_unload_game_module();

    // E80 (T-683): a successful load is a use, so the manifest's last write time becomes now; a refused one is not.
    const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24 * 40);
    const fs::path manifest_file = good / kDonModuleManifest;
    fs::last_write_time(manifest_file, stamp);
    report(don_load_game_module(good.string(), error), "good folder loads again: " + error);
    const auto age = fs::file_time_type::clock::now() - fs::last_write_time(manifest_file);
    report(age < std::chrono::minutes(1), "a successful load sets the manifest's last write time to now");
    don_unload_game_module();
    const fs::path refused = makeFolder(root, "refusedid", manifestText("refusedid", std::string(64, '0'), kDonGameModuleAbiVersion, "true"));
    fs::last_write_time(refused / kDonModuleManifest, stamp);
    std::string refused_error;
    report(!don_load_game_module(refused.string(), refused_error) &&
               fs::last_write_time(refused / kDonModuleManifest) == stamp,
           "a refused load leaves the manifest's last write time alone");

    // A marker left by a crashed run: the load reports E59 (the [don] line is on stderr) and goes on; the marker stays
    // for the setup screens.
    std::ofstream(good / kDonModuleRunningMarker) << "running\n";
    crashed = false;
    report(don_load_game_module(good.string(), error, &crashed) && crashed,
           "folder with a stale marker still loads and reports E59: " + error);
    report(fs::exists(good / kDonModuleRunningMarker), "stale marker is left for the setup screens");
    don_game_module_end_run();
    don_unload_game_module();
    fs::remove_all(root);
}
} // namespace

int main()
{
    expectRefused("missing file", fixture("libfx_no_such_module.so"), "cannot load");
    expectRefused("missing symbol", fixture("libfx_nosym.so"), "does not export " + std::string(kDonGameModuleEntrySymbol));
    expectRefused("wrong ABI version", fixture("libfx_abi.so"),
                  abiNeedle(kDonGameModuleAbiVersion + 1, sizeof(DonGameModuleEntry)));
    expectRefused("wrong entry size", fixture("libfx_size.so"),
                  abiNeedle(kDonGameModuleAbiVersion, sizeof(DonGameModuleEntry) + 8));
    expectRefused("null bind", fixture("libfx_nobind.so"), "null bind");
    expectRefused("empty table", fixture("libfx_empty.so"), "empty function table");

    // The good module: the table the runtime will read is the module's, and the bind slot is set only by the bind.
    const std::string good = fixture("libfx_ok.so");
    std::string error;
    report(don_load_game_module(good, error) && error.empty(), "good module loads: " + error);
    const DonGameModule &table = don_game_module();
    report(table.count == 2 && table.entries != nullptr && table.entries[0].address == 0x00100000u &&
               table.entries[1].address == 0x00100008u,
           "good module's table is the one handed to the runtime");
    report(!don_game_module_bound(), "good module is not bound before the bind");

    void *handle = dlopen(good.c_str(), RTLD_NOW | RTLD_LOCAL);
    using EntryFn = const DonGameModuleEntry *(*)();
    const DonGameModuleEntry *entry = handle != nullptr && dlsym(handle, kDonGameModuleEntrySymbol) != nullptr
                                          ? reinterpret_cast<EntryFn>(dlsym(handle, kDonGameModuleEntrySymbol))()
                                          : nullptr;
    report(entry != nullptr && entry->bind != nullptr && *entry->bind == nullptr,
           "bind slot of the good module is empty before the bind");

    int runtime_marker = 0;
    don_bind_game_module(&runtime_marker);
    report(entry != nullptr && entry->bind != nullptr && *entry->bind == &runtime_marker,
           "bind writes the runtime into the module's slot");
    report(don_game_module_bound(), "the bind is seen as done after don_bind_game_module");

    don_unload_game_module();
    iopTests();
    folderTests();

    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
