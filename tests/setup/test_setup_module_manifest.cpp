// Tests of the one reader of module/<id>/manifest.json (T-683; it moved from src/host, where T-616 tested it through the
// loader). Pure text in, struct out: no game, no host code.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "module_manifest.h"

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void put_text(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}

// Every field of the manifest; `skip` leaves one out, `override_field` replaces the text of one ("name":value).
static std::string manifest(const std::string& skip = "", const std::string& replace_name = "",
                            const std::string& replace_with = "", const std::string& extra = "") {
    const char* names[] = {"program_version", "abi_version", "module_id", "toolchain_hash",
                           "module_sha256", "verified", "variant"};
    const char* values[] = {"\"0.1\"", "2", "\"ab12\"", "\"tc\"", "\"ff\"", "true", "\"jp\""};
    std::string t = "{";
    for (int i = 0; i < 7; ++i) {
        if (names[i] == skip) continue;
        t += std::string("\"") + names[i] + "\":" + (names[i] == replace_name ? replace_with : values[i]) + ",";
    }
    return t + extra + "\"input_hashes\":{\"elf\":\"00\",\"flags\":[1,2.5e1,null]}}";
}

// Reads `text` from a file; returns whether it parsed. `error` is the reader's message.
static bool read(const fs::path& dir, const std::string& text, DonModuleManifest& m, std::string& error) {
    fs::path f = dir / "manifest.json";
    put_text(f, text);
    return don_read_module_manifest(f.string(), m, error);
}

int main() {
    fs::path dir = fs::temp_directory_path() / ("don_manifest_" + std::to_string(getpid()));
    fs::create_directories(dir);
    DonModuleManifest m;
    std::string e;

    CHECK(read(dir, manifest(), m, e));
    CHECK(m.program_version == "0.1" && m.abi_version == 2 && m.module_id == "ab12" && m.toolchain_hash == "tc");
    CHECK(m.module_sha256 == "ff" && m.verified && m.variant == "jp");
    CHECK(read(dir, " \n" + manifest() + "\n ", m, e));  // white space around the object

    // Unknown fields of any shape are skipped; an invalid one is not.
    CHECK(read(dir, manifest("", "", "", "\"x\":{\"a\":[true,false,{}],\"b\":-1.5e3},\"y\":\"\\u0041\","), m, e));
    CHECK(!read(dir, manifest("", "", "", "\"x\":[1,,2],"), m, e) && e.find("bad value of \"x\"") != std::string::npos);

    // Each field is required.
    for (const char* name : {"program_version", "abi_version", "module_id", "toolchain_hash", "module_sha256",
                             "verified", "variant"}) {
        CHECK(!read(dir, manifest(name), m, e) && e.find(std::string("missing field \"") + name + "\"") != std::string::npos);
    }
    // Only the top level counts: a "variant" inside another object is not the field.
    CHECK(!read(dir, "{\"nested\":{\"variant\":\"f\"}}", m, e));

    // Each field has its type.
    CHECK(!read(dir, manifest("", "abi_version", "\"2\""), m, e) && e.find("wrong type") != std::string::npos);
    CHECK(!read(dir, manifest("", "abi_version", "-1"), m, e) && e.find("wrong type") != std::string::npos);
    CHECK(!read(dir, manifest("", "module_id", "7"), m, e) && e.find("wrong type") != std::string::npos);
    CHECK(!read(dir, manifest("", "verified", "\"yes\""), m, e) && e.find("wrong type") != std::string::npos);
    CHECK(!read(dir, manifest("", "variant", "null"), m, e) && e.find("wrong type") != std::string::npos);
    CHECK(read(dir, manifest("", "verified", "false"), m, e) && !m.verified);

    // A field twice, and a file that is not exactly one object.
    CHECK(!read(dir, manifest("", "", "", "\"variant\":\"us\","), m, e) && e.find("duplicate field \"variant\"") != std::string::npos);
    CHECK(!read(dir, manifest() + " x", m, e) && e.find("text after the object") != std::string::npos);
    CHECK(!read(dir, "", m, e) && e.find("not a JSON object") != std::string::npos);
    CHECK(!read(dir, "[]", m, e));
    CHECK(!read(dir, manifest().substr(0, 40), m, e) && e.find("is corrupt") != std::string::npos);
    std::string comma = manifest();
    comma.insert(comma.size() - 1, ",");
    CHECK(!read(dir, comma, m, e));  // trailing comma

    // Missing and oversized files.
    CHECK(!don_read_module_manifest((dir / "none" / "manifest.json").string(), m, e) && e.find("does not exist") != std::string::npos);
    CHECK(!read(dir, manifest("", "", "", "\"pad\":\"" + std::string(kDonModuleManifestMaxBytes, 'a') + "\","), m, e) &&
          e.find("larger than") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
