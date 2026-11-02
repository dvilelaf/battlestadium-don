// T-746: the Windows module link passes its ~1900 object paths in a response file, not on the command line, and a
// failed link's error text names the link's own stderr file.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "module_build.h"

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                              \
    do {                                                                                      \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static size_t joined_length(const std::vector<std::string>& argv) {
    size_t n = 0;
    for (const std::string& a : argv) n += a.size() + 1;
    return n;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "don_t746";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // The T-743 run: 1874 objects under a user-folder build tree (realistic length, ~110 characters each).
    const fs::path obj_dir = fs::path("C:/Users/someone/AppData/Local/DON/build/tmp/0123456789abcdef/obj");
    std::vector<fs::path> objects;
    for (int i = 0; i < 1874; ++i) objects.push_back(obj_dir / ("ps2_recompiled_functions_unit_" + std::to_string(i) + ".o"));

    ModulePaths p;
    p.gen = "C:/gen";
    p.sdk = "C:/sdk";
    p.toolchain = "C:/toolchain";
    p.platform = ModulePlatform::Windows;
    const fs::path rsp = dir / "link.rsp";
    std::vector<std::string> argv;
    std::string why;
    CHECK(prepare_module_link(p, objects, "C:/work/don_game.dll", rsp, argv, why));
    // (a) CreateProcessW's lpCommandLine limit is 32767 characters.
    CHECK(joined_length(argv) < 32767);
    CHECK(!argv.empty() && argv.back() == "@" + rsp.string());
    std::ifstream in(rsp, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    size_t lines = 0;
    for (char c : text) lines += c == '\n';
    CHECK(lines == objects.size());
    CHECK(text.find("\"" + objects.front().string() + "\"\n") == 0);
    CHECK(text.find("\"" + objects.back().string() + "\"\n") != std::string::npos);

    // Quoting: a quote is escaped, backslashes before it (and before the closing quote) double.
    CHECK(module_link_response_text({fs::path("a b\\c")}) == "\"a b\\c\"\n");
    CHECK(module_link_response_text({fs::path("x\\\"y")}) == "\"x\\\\\\\"y\"\n");
    CHECK(module_link_response_text({fs::path("dir\\")}) == "\"dir\\\\\"\n");

    // Linux keeps the objects on the command line.
    p.platform = ModulePlatform::Linux;
    std::vector<std::string> lin;
    CHECK(prepare_module_link(p, objects, "/w/libdon_game.so", dir / "never.rsp", lin, why));
    CHECK(lin.size() > objects.size() && lin.back() == objects.back().string() && !fs::exists(dir / "never.rsp"));

    // (b) the error text of a failed link names the link's err file; a compile names its unit's.
    const fs::path work = "/w", so = "/w/don_game.dll";
    CHECK(module_failure_err_path("link", work, so) == fs::path("/w/don_game.dll.err"));
    CHECK(module_failure_err_path("failed: cc:unit_7", work, so) == work / "obj" / "unit_7.o.err");

    fs::remove_all(dir, ec);
    if (g_fail != 0) return 1;
    std::cout << "ok\n";
    return 0;
}
