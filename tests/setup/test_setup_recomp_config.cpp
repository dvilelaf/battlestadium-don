// The C++ render of config/recomp.toml must equal tools/pipeline/freeze_recomp_config.py --render byte for byte.
// No game file is read.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "disc_image.h"
#include "recomp_config.h"

using namespace don_setup;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

// Measured once: `freeze_recomp_config.py --render x.toml --disc /d/SLPS_256.75 --out /o --repo /r` on the frozen
// config/recomp.toml of this commit, then sha256sum x.toml. Update it together with config/recomp.toml.
static const char* kPythonRenderSha256 = "2851974f8547bea8634936cd8e93615034041585ab7fc77866f597903458bb25";

// What the renderer makes of a path: absolute and resolved. On Windows "/d/x" is "<drive>:/d/x" (as Python's
// Path.resolve() gives there), so the expected strings are built the same way instead of being written out.
static std::string res(const char* p) {
    std::error_code ec;
    return fs::weakly_canonical(fs::absolute(p, ec), ec).generic_string();
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string digest(const std::string& s) { return sha256_hex(s.data(), s.size()); }

int main() {
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path().parent_path();
    const std::string frozen = slurp(repo / "config" / "recomp.toml");
    CHECK(!frozen.empty());

    std::string out, err;
    CHECK(render_recomp_config(frozen, "/d/SLPS_256.75", "/o", "/r", false, out, err));
#ifndef _WIN32   // the digest is of the Linux render; on Windows the paths carry a drive letter
    CHECK(digest(out) == kPythonRenderSha256);
#endif
    CHECK(out.find("input = \"" + res("/d/SLPS_256.75") + "\"") != std::string::npos);
    CHECK(out.find("output = \"" + res("/o") + "/\"") != std::string::npos);

    // The module ABI switch adds exactly one line, right after [general].
    std::string abi;
    CHECK(render_recomp_config(frozen, "/d/SLPS_256.75", "/o", "/r", true, abi, err));
    CHECK(abi.size() == out.size() + std::string("emit_module_abi = true\n").size());
    CHECK(abi.find("[general]\nemit_module_abi = true\ninput =") != std::string::npos);

    // A config that already sets the key keeps a single line of it.
    std::string twice;
    CHECK(render_recomp_config("[general]\nemit_module_abi = false\ninput = \"{disc}\"\n", "/d/x", "/o", "/r", true,
                               twice, err));
    CHECK(twice == "[general]\nemit_module_abi = true\ninput = \"" + res("/d/x") + "\"\n");

    // Comments keep their placeholders; a config without [general] is refused only when the switch needs it.
    std::string c;
    CHECK(render_recomp_config("# {disc}\nx = \"{out}\"\n", "/d", "/o", "/r", false, c, err));
    CHECK(c == "# {disc}\nx = \"" + res("/o") + "\"\n");
    CHECK(!render_recomp_config("x = 1\n", "/d", "/o", "/r", true, c, err) && !err.empty());

    std::cerr << (g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
