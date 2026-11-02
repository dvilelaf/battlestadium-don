// classify() on tiny synthetic files: one per kind, a wrong extension and a partial download. No game data.
#include "disc_classify.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace don_setup;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static fs::path g_dir;

static Bytes padded(const std::string& magic, size_t at, size_t total) {
    Bytes b(total, 0x55);
    std::memcpy(b.data() + at, magic.data(), magic.size());
    return b;
}

static fs::path write(const std::string& name, const Bytes& b) {
    fs::path p = g_dir / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return p;
}

static Kind kindOf(const std::string& name, const Bytes& b) { return classify(write(name, b)).kind; }

int main() {
    g_dir = fs::temp_directory_path() / "don_t621_classify";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    Bytes iso(17 * 2048 + 64, 0);
    std::memcpy(iso.data() + 16 * 2048 + 1, "CD001", 5);
    CHECK(kindOf("a.iso", iso) == Kind::Iso);
    Bytes raw(17 * 2352 + 64, 0);
    std::memcpy(raw.data() + 16 * 2352 + 16 + 1, "CD001", 5);
    CHECK(kindOf("raw.bin", raw) == Kind::Iso);

    CHECK(kindOf("a.gz", padded("\x1F\x8B\x08", 0, 64)) == Kind::Gz);
    CHECK(kindOf("a.zip", padded("PK\x03\x04", 0, 64)) == Kind::Zip);
    CHECK(kindOf("a.chd", padded("MComprHD", 0, 64)) == Kind::Chd);
    CHECK(kindOf("a.cso", padded("CISO", 0, 64)) == Kind::Cso);
    CHECK(kindOf("a.zso", padded("ZISO", 0, 64)) == Kind::Zso);
    CHECK(kindOf("a.7z", padded(std::string("7z\xBC\xAF\x27\x1C", 6), 0, 64)) == Kind::SevenZip);
    CHECK(kindOf("a.rar", padded("Rar!\x1A\x07", 0, 64)) == Kind::Rar);
    CHECK(kindOf("a.ecm", padded(std::string("ECM\0", 4), 0, 64)) == Kind::Ecm);
    CHECK(kindOf("a.nrg", padded("NER5", 100 - 8, 100)) == Kind::Nrg);
    CHECK(kindOf("a.mds", padded("MEDIA DESCRIPTOR", 0, 64)) == Kind::Mdf);
    CHECK(kindOf("a.xdelta", padded("\xD6\xC3\xC4", 0, 64)) == Kind::Xdelta);
    CHECK(kindOf("a.ppf", padded("PPF30", 0, 64)) == Kind::Ppf);
    CHECK(kindOf("a.ips", padded("PATCH", 0, 64)) == Kind::Ips);
    CHECK(kindOf("a.bps", padded("BPS1", 0, 64)) == Kind::Bps);
    CHECK(kindOf("a.rvz", padded("RVZ\x01", 0, 64)) == Kind::GameCube);
    CHECK(kindOf("a.gcm", padded("\xC2\x33\x9F\x3D", 0x1C, 64)) == Kind::GameCube);

    const std::string cue = "FILE \"x.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n";
    CHECK(kindOf("a.cue", Bytes(cue.begin(), cue.end())) == Kind::Cue);

    fs::create_directories(g_dir / "plain");
    CHECK(classify(g_dir / "plain").kind == Kind::Folder);
    fs::create_directories(g_dir / "game");
    write("game/SYSTEM.CNF", Bytes(8, 'x'));
    CHECK(classify(g_dir / "game").kind == Kind::DirectoryWithGame);

    CHECK(kindOf("tiny.iso", Bytes(10, 0x20)) == Kind::TooSmall);
    CHECK(kindOf("empty.iso", Bytes()) == Kind::TooSmall);
    CHECK(kindOf("big.bin", Bytes(40000 * 2, 0x20)) == Kind::Unknown);
    CHECK(classify(g_dir / "missing").kind == Kind::Unknown);

    // The extension is never trusted: a zip named .iso is a zip.
    CHECK(kindOf("lie.iso", padded("PK\x03\x04", 0, 64)) == Kind::Zip);

    // E09: the partial-download name sets only the detail; the kind still comes from the bytes.
    Classification part = classify(write("a.iso.crdownload", padded("PK\x03\x04", 0, 64)));
    CHECK(part.kind == Kind::Zip);
    CHECK(part.detail == "still downloading");

    fs::remove_all(g_dir);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "classify: all cases passed\n";
    return 0;
}
