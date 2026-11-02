// T-752: the optimisation level of the module recipe. Every game unit is compiled at the release level; only the unit
// that holds register_functions.cpp (a registration table that runs no game instruction) stays at -O1. And a module
// prepared by an older recipe must not be mistaken for the current one: the recipe is part of the module id.
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include "disc_image.h"
#include "module_build.h"

using namespace don_setup;
namespace fs = std::filesystem;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; ++g_fail; } \
    } while (0)
static int g_fail = 0;

static bool has(const std::vector<std::string>& a, const std::string& s) { return std::find(a.begin(), a.end(), s) != a.end(); }
static bool any_opt(const std::vector<std::string>& a) {
    return std::any_of(a.begin(), a.end(), [](const std::string& s) { return s.size() == 3 && s.compare(0, 2, "-O") == 0; });
}

int main() {
    const fs::path tc = fs::temp_directory_path() / "don_test_module_flags_tc";
    fs::create_directories(tc / "lib/clang/17/include");
    ModulePaths p{"/g", "/s", tc, ModulePlatform::Linux};

    const auto reg = module_compile_argv(p, "/u/unity/big_register_functions.cpp", "/o/big_register_functions.o");
    CHECK(has(reg, "-O1") && !has(reg, "-O2") && !has(reg, "-O3"));

    for (const char* unit : {"/u/unity/batch_00000.cpp", "/u/unity/big_FUN_00100350_0x100350.cpp", "/s/game_module/module_entry.cpp"}) {
        const auto a = module_compile_argv(p, unit, "/o/x.o");
        CHECK(has(a, "-O2") && !has(a, "-O1") && !has(a, "-O3"));
        CHECK(has(a, "-ffp-contract=off") && has(a, "-march=x86-64-v3") && !has(a, "-ffast-math"));
    }
    CHECK(any_opt(reg));

    // The id used before the recipe was part of it: a module prepared that way has to be seen as built for another version.
    const std::string legacy_text = std::string("aa") + "\n" + "bb" + "\n" + std::to_string(1) + "\n";
    const std::string legacy = sha256_hex(legacy_text.data(), legacy_text.size()).substr(0, 16);
    CHECK(module_id_of("aa", "bb", 1) != legacy);

    fs::remove_all(tc);
    if (g_fail == 0) std::cout << "ok\n";
    return g_fail == 0 ? 0 : 1;
}
