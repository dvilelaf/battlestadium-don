// The build half of "prepare": a tree emitted with general.emit_module_abi = true becomes module/<id>/ holding
// libdon_game.so (don_game.dll on Windows, see module_platform.h) and manifest.json, with the bundled clang and lld and
// no Python or ninja (task T-690). It is the C++ twin of tools/module/make_ninja.py: same unit grouping, same flags.
//
// The module's abi_version is kDonGameModuleAbiVersion, read from <sdk>/host/game_module.h.
// The `sdk` folder holds the headers the emitted code includes, laid out like the repository:
//   <sdk>/game_module/   a copy of src/game_module/ (don_guest_*.h, iop_shim/, module_entry.cpp)
//   <sdk>/host/game_module.h
//   <sdk>/inc/don_guest_macros.h   the runtime macros header, derived by tools/pipeline/gen_shim_macros.py
// The macros header is shipped in the sdk (derived at release time) rather than generated here: it is made from
// third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime_macros.h by regex substitutions, and the player's PC has
// neither that header nor a reason to run the substitutions; deriving it once from the pinned runtime is reproducible.
// The toolchain folder is build/toolchain/linux/ (bin/clang++, bin/ld.lld, lib/clang/<n>/include, TOOLCHAIN.sha256).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "module_platform.h"
#include "setup_types.h"

namespace don_setup {

constexpr uintmax_t kModuleBigBytes = 200000;  // a source this big is its own translation unit (generate.sh's split)
constexpr size_t kModuleBatch = 32;            // the smaller sources go this many to one unity file

struct ModuleUnit {
    std::string name;                   // "batch_00000" or "big_<stem>"
    std::vector<std::filesystem::path> files;
    uintmax_t bytes = 0;
};

// The units of the tree in gen/: sorted *.cpp, big ones alone, the rest in batches; batches first, then big ones
// (the order of make_ninja.py). Empty with `error` set when gen holds no .cpp.
std::vector<ModuleUnit> plan_module_units(const std::filesystem::path& gen, std::string& error);
// Writes <dir>/<unit.name>.cpp: one #include "<file>" line per source. Returns its path (empty on failure).
std::filesystem::path write_unity_file(const ModuleUnit& unit, const std::filesystem::path& dir);

struct ModulePaths {
    std::filesystem::path gen, sdk, toolchain;
    ModulePlatform platform = kHostModulePlatform;  // the module is built for this platform (module_platform.h)
};
// <toolchain>/lib/clang/<n>/include, or empty when absent.
std::filesystem::path module_resource_include(const std::filesystem::path& toolchain);
// argv[0] is <toolchain>/bin/clang++ (clang++.exe for a Windows module); the flags are those of make_ninja.py FLAGS.
std::vector<std::string> module_compile_argv(const ModulePaths& p, const std::filesystem::path& source,
                                             const std::filesystem::path& object);
// argv[0] is <toolchain>/bin/ld.lld (lld-link.exe for a Windows module).
std::vector<std::string> module_link_argv(const ModulePaths& p, const std::vector<std::filesystem::path>& objects,
                                          const std::filesystem::path& output);
// The link step as the build runs it. On Windows the object list goes to the response file `rsp` (module_link_response_text)
// and argv ends with "@<rsp>": CreateProcessW takes at most 32767 characters of command line
// (https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw, lpCommandLine)
// and the real module has ~1900 objects. On Linux argv lists the objects (their ~250 KB are far under ARG_MAX, 2 MiB) and
// `rsp` is not touched. Returns false with `why` when the file cannot be written.
bool prepare_module_link(const ModulePaths& p, const std::vector<std::filesystem::path>& objects,
                         const std::filesystem::path& output, const std::filesystem::path& rsp,
                         std::vector<std::string>& argv, std::string& why);
// One path per line, each in double quotes with `"` as \" and backslashes before a quote doubled: the Windows command line
// rules lld's COFF driver tokenizes response files with (https://lld.llvm.org/windows_support.html, "@file"; LLVM's
// cl::TokenizeWindowsCommandLine). UTF-8.
std::string module_link_response_text(const std::vector<std::filesystem::path>& objects);
// The file that holds the stderr of the failed job named in `detail` ("cc:<unit>" or "link"): <work>/obj/<unit>.o.err
// for a compile, <module_file>.err (the job's output plus ".err") for the link.
std::filesystem::path module_failure_err_path(const std::string& detail, const std::filesystem::path& work,
                                              const std::filesystem::path& module_file);

// First 16 hex of SHA-256 over the text "<tree hash>\n<toolchain hash>\n<abi version>\n".
std::string module_id_of(const std::string& tree_hash, const std::string& toolchain_hash, uint32_t abi_version);

struct ModuleBuildOptions {
    ModulePaths paths;
    std::filesystem::path root;  // bulk root: module/<id>/ is made here, the work in build/tmp/<id>/
    std::string variant;         // game/<variant>/ the module is for (manifest field)
    std::string program_version;
    size_t max_jobs = 0;         // cap on parallel compiles; 0 = logical cores. Never more than the logical cores.
    std::function<void(const Progress&)> on_progress;
    // Raised: no new unit starts, the running compilers are killed (their process groups) and the journal keeps the
    // finished units; build_module() on the same root then continues.
    const std::atomic<bool>* stop = nullptr;
};

struct ModuleBuildResult {
    bool ok = false;
    bool stopped = false;        // `stop` ended the build before it was done
    size_t skipped = 0;          // units the journal proved done (an earlier run finished them)
    size_t compiled = 0;         // units that ran to the end in this call
    std::string module_id;
    std::filesystem::path module_dir;
    std::string error;           // names the failed unit and the file holding its stderr
};

// Builds (or resumes) the module. A module/<id>/ with a valid manifest is left as it is.
ModuleBuildResult build_module(const ModuleBuildOptions& o);

}  // namespace don_setup
