#include "disc_classify.h"
#include "path_text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace don_setup {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr size_t kWindow = 64 * 1024;  // the only part of the file that is read (plus the NRG trailer)

// Does `b` hold `magic` at `at`? Bounds-checked, so short files simply do not match.
bool has(const Bytes& b, size_t at, const char* magic, size_t n) {
    return at + n <= b.size() && std::memcmp(b.data() + at, magic, n) == 0;
}
bool hasBytes(const Bytes& b, size_t at, std::initializer_list<uint8_t> m) {
    return at + m.size() <= b.size() && std::equal(m.begin(), m.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
}
#define HAS(b, at, lit) has(b, at, lit, sizeof(lit) - 1)

// ISO 9660 primary volume descriptor: "CD001" after the type byte at sector 16 (ECMA-119 8.4).
// Plain 2048-byte sectors, or raw 2352-byte sectors (12 sync + 4 header, or +8 sub-header for Mode 2 Form 1),
// or raw sectors with 96 bytes of subchannel (2448).
bool isIso(const Bytes& b) {
    if (HAS(b, 16 * 2048 + 1, "CD001")) return true;
    for (size_t stride : {size_t{2352}, size_t{2448}})
        for (size_t head : {size_t{16}, size_t{24}})
            if (HAS(b, 16 * stride + head + 1, "CD001")) return true;
    return false;
}

// Raw CD sector sync pattern: 00, ten FF, 00 (ECMA-130 14.1); a raw dump with it but no ISO descriptor is a
// sector-level image (MDF).
bool hasSync(const Bytes& b) { return hasBytes(b, 0, {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0}); }

// A cue sheet is text; its first word is a CUE command and it declares a TRACK and an INDEX (CDRWIN cue format).
bool isCue(const Bytes& b) {
    std::string t;
    for (uint8_t c : b) {
        if (c == 0) return false;
        t.push_back(static_cast<char>(std::toupper(c)));
    }
    size_t s = t.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    if (t.compare(s, 3, "\xEF\xBB\xBF") == 0) s = t.find_first_not_of(" \t\r\n", s + 3);  // UTF-8 BOM
    if (s == std::string::npos) return false;
    static const char* const words[] = {"FILE", "REM", "CATALOG", "PERFORMER", "TITLE", "TRACK"};
    bool first = false;
    for (const char* w : words) first = first || t.compare(s, std::strlen(w), w) == 0;
    return first && t.find("TRACK") != std::string::npos && t.find("INDEX") != std::string::npos;
}

// Nero NRG keeps its signature in the trailer: "NER5" (64-bit offsets) 8 bytes before EOF, or "NERO" (32-bit)
// 12 bytes before EOF. This is the one read outside the leading window.
bool isNrg(const fs::path& p, uint64_t size) {
    if (size < 12) return false;
    std::ifstream f(p, std::ios::binary);
    std::array<char, 12> t{};
    f.seekg(static_cast<std::streamoff>(size - 12));
    f.read(t.data(), 12);
    return f && (std::memcmp(t.data(), "NERO", 4) == 0 || std::memcmp(t.data() + 4, "NER5", 4) == 0);
}

// E09 partial-download suffixes (docs/tasks/M6-public-release.md, A4): browsers and torrent clients.
bool isPartialName(const fs::path& p) {
    std::string e = path_text(p.extension());
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    static const char* const names[] = {".crdownload", ".part", ".download", ".!qb", ".partial", ".opdownload"};
    return std::any_of(std::begin(names), std::end(names), [&](const char* n) { return e == n; });
}

Kind kindOfBytes(const Bytes& b) {
    if (HAS(b, 0, "RVZ\x01") || HAS(b, 0, "WIA\x01")) return Kind::GameCube;  // Dolphin RVZ / WIA header
    if (hasBytes(b, 0x1C, {0xC2, 0x33, 0x9F, 0x3D})) return Kind::GameCube;    // GameCube disc header magic, 0x1C
    if (hasBytes(b, 0, {0x1F, 0x8B})) return Kind::Gz;                         // gzip (RFC 1952)
    if (HAS(b, 0, "PK\x03\x04") || HAS(b, 0, "PK\x05\x06")) return Kind::Zip;  // zip local header / empty archive
    if (HAS(b, 0, "MComprHD")) return Kind::Chd;                               // MAME CHD header tag
    if (HAS(b, 0, "CISO")) return Kind::Cso;                                   // CISO compressed ISO
    if (HAS(b, 0, "ZISO")) return Kind::Zso;                                   // ZISO (LZ4 compressed ISO)
    if (hasBytes(b, 0, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) return Kind::SevenZip;  // 7-Zip signature header
    if (HAS(b, 0, "Rar!\x1A\x07")) return Kind::Rar;                           // RAR 1.5+ / 5 signature
    if (HAS(b, 0, "ECM\x00")) return Kind::Ecm;                                // ECM (Error Code Modeler) header
    if (HAS(b, 0, "MEDIA DESCRIPTOR")) return Kind::Mdf;                       // Alcohol 120% .mds header
    if (hasBytes(b, 0, {0xD6, 0xC3, 0xC4})) return Kind::Xdelta;               // VCDIFF (RFC 3284), xdelta3
    if (HAS(b, 0, "PPF")) return Kind::Ppf;                                    // PlayStation Patch Format
    if (HAS(b, 0, "PATCH")) return Kind::Ips;                                  // IPS patch header
    if (HAS(b, 0, "BPS1")) return Kind::Bps;                                   // byuu/higan BPS patch header
    if (isIso(b)) return Kind::Iso;
    if (hasSync(b)) return Kind::Mdf;                                          // raw-sector dump without ISO PVD
    if (isCue(b)) return Kind::Cue;
    return Kind::Unknown;
}

}  // namespace

Classification classify(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        if (fs::exists(path / "SYSTEM.CNF", ec)) return {Kind::DirectoryWithGame, "folder holds SYSTEM.CNF"};
        return {Kind::Folder, "folder without SYSTEM.CNF"};
    }
    const uint64_t size = fs::is_regular_file(path, ec) ? fs::file_size(path, ec) : 0;
    std::ifstream f(path, std::ios::binary);
    if (ec || !f) return {Kind::Unknown, "cannot be read"};

    Bytes buf(kWindow);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<size_t>(f.gcount()));

    Classification c{kindOfBytes(buf), {}};
    if (c.kind == Kind::Unknown && isNrg(path, size)) c.kind = Kind::Nrg;
    if (c.kind == Kind::Unknown) {
        if (size < 17 * 2048) { c.kind = Kind::TooSmall; c.detail = "smaller than a disc image can be"; }
        else c.detail = "no known signature";
    }
    if (isPartialName(path)) c.detail = "still downloading";
    return c;
}

}  // namespace don_setup
