// Module platform names (T-730): the file and tool names per platform, and the argv the recipe builds for each.
#include "module_platform.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "module_build.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

// Argv paths use the host's separators; the expectations are written with '/'.
static std::vector<std::string> fwd(std::vector<std::string> v) {
    for (std::string& s : v) std::replace(s.begin(), s.end(), '\\', '/');
    return v;
}

static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

int main() {
    CHECK(std::string(module_file_name(ModulePlatform::Linux)) == "libdon_game.so");
    CHECK(std::string(module_file_name(ModulePlatform::Windows)) == "don_game.dll");
    CHECK(std::string(compiler_file_name(ModulePlatform::Linux)) == "clang++");
    CHECK(std::string(compiler_file_name(ModulePlatform::Windows)) == "clang++.exe");
    CHECK(std::string(linker_file_name(ModulePlatform::Linux)) == "ld.lld");
    CHECK(std::string(linker_file_name(ModulePlatform::Windows)) == "lld-link.exe");
    CHECK(std::string(module_target_triple(ModulePlatform::Windows)) == "x86_64-pc-windows-msvc");
    CHECK(std::string(module_target_triple(ModulePlatform::Linux)).empty());
    CHECK(std::string(kModuleEntrySymbol) == "don_game_module_v1");
#ifdef _WIN32
    CHECK(kHostModulePlatform == ModulePlatform::Windows);
#else
    CHECK(kHostModulePlatform == ModulePlatform::Linux);
#endif

    // Linux argv keeps its shape: no target flag, ELF linker flags.
    ModulePaths lin{"/g", "/sdk", "/tc", ModulePlatform::Linux};
    auto lc = fwd(module_compile_argv(lin, "/u/x.cpp", "/o/x.o"));
    CHECK(lc[0] == "/tc/bin/clang++");
    CHECK(std::none_of(lc.begin(), lc.end(), [](const std::string& s) { return s.rfind("--target=", 0) == 0; }));
    auto ll = fwd(module_link_argv(lin, {"/o/a.o"}, "/o/out.so"));
    CHECK(ll[0] == "/tc/bin/ld.lld");
    CHECK(has(ll, "-shared"));
    CHECK(has(ll, "-o"));

    // Windows argv: the MSVC target, a DLL with no entry point, no default libraries, nothing imported.
    ModulePaths win{"/g", "/sdk", "/tc", ModulePlatform::Windows};
    auto wc = fwd(module_compile_argv(win, "/u/x.cpp", "/o/x.o"));
    CHECK(wc[0] == "/tc/bin/clang++.exe");
    CHECK(wc[1] == "--target=x86_64-pc-windows-msvc");
    CHECK(has(wc, "-ffreestanding"));
    CHECK(has(wc, "-ffp-contract=off"));
    auto wl = fwd(module_link_argv(win, {"/o/a.o", "/o/b.o"}, "/o/don_game.dll"));
    CHECK(wl[0] == "/tc/bin/lld-link.exe");
    CHECK(has(wl, "/DLL"));
    CHECK(has(wl, "/NOENTRY"));
    CHECK(has(wl, "/NODEFAULTLIB"));
    CHECK(has(wl, "/MACHINE:X64"));
    CHECK(has(wl, "/OUT:/o/don_game.dll"));
    CHECK(has(wl, "/o/a.o") && has(wl, "/o/b.o"));
    CHECK(!has(wl, "-shared"));

    if (g_fail == 0) std::cout << "test_module_platform: ok\n";
    return g_fail == 0 ? 0 : 1;
}
