#include "disc_image.h"
#include "path_text.h"

#include <cctype>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>

#include "disc_classify.h"
#include "iso_reader.h"

namespace don_setup {

std::string sha1_hex(const void* data, size_t size) {
    Sha1 h;
    h.update(static_cast<const uint8_t*>(data), size);
    return h.finish();
}
std::string sha256_hex(const void* data, size_t size) {
    Sha256 h;
    h.update(static_cast<const uint8_t*>(data), size);
    return h.finish();
}

const DiscTable& builtin_disc_table() {
    // Values of config/discs.toml; keep them in step with that file.
    static const DiscTable t{
        "994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1",
        {{"jp", "Japanese (original)", "1c2cdbe5500e5c2328a2ec9f030789156fecc0b5"},
         {"en-jpvoice", "English, Japanese voices (Lord Izen patch, 2023-09-07)",
          "12f174bccbeadc5c89eeb172317d163365904fd2"},
         {"en-envoice", "English, English voices (Lord Izen patch, 2023-09-07)",
          "ca29e2ee6d21b9137533d1ddd80547a10133ae0f"}}};
    return t;
}

DiscIdentity identify_disc(const std::filesystem::path& image, const ProgressFn& progress) {
    return identify_disc(image, builtin_disc_table(), progress);
}

namespace {

constexpr const char* kDonSerial = "SLPS-25675";

// The disc's serial from SYSTEM.CNF ("BOOT2 = cdrom0:\SLUS_123.45;1" -> "SLUS-12345"); empty when there is none.
std::string read_serial(ImageReader& img, const std::vector<IsoFile>& files) {
    const IsoFile* cnf = find_file(files, "SYSTEM.CNF");
    if (!cnf || cnf->size == 0 || cnf->size > 4096) return {};
    std::string t(size_t(cnf->size), '\0');
    if (!img.read_at(cnf->lba * kSector, t.data(), t.size())) return {};
    size_t b = t.find("BOOT2");
    if (b == std::string::npos) return {};
    size_t s = t.find('\\', b);
    if (s == std::string::npos) return {};
    size_t e = t.find_first_of(";\r\n \t", s + 1);
    std::string name = t.substr(s + 1, e == std::string::npos ? e : e - s - 1), serial;
    for (char c : name)
        if (c != '.') serial += c == '_' ? '-' : c;
    return serial;
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Chd: return "CHD";
        case Kind::Cso: return "CSO";
        case Kind::Zso: return "ZSO";
        case Kind::SevenZip: return "7z";
        case Kind::Rar: return "RAR";
        case Kind::Ecm: return "ECM";
        case Kind::Nrg: return "NRG";
        case Kind::Mdf: return "MDF";
        case Kind::Cue: return "CUE";
        case Kind::Xdelta: return "xdelta";
        case Kind::Ppf: return "PPF";
        case Kind::Ips: return "IPS";
        case Kind::Bps: return "BPS";
        default: return "";
    }
}

// The file of a folder whose name matches `name` ignoring case (a copied disc may have any case); empty when absent.
std::filesystem::path find_in_folder(const std::filesystem::path& dir, const char* name) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && ieq(path_text(it->path().filename()), name)) return it->path();
    return {};
}

template <class Hasher>
bool hash_plain_file(const std::filesystem::path& p, Hasher& h, uint64_t& done, uint64_t total, const char* what,
                     const ProgressFn& progress) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> buf(1 << 20);
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        size_t n = size_t(in.gcount());
        h.update(buf.data(), n);
        done += n;
        if (progress && n) progress(total ? double(done) / double(total) : 1.0, what);
    }
    return in.eof();
}

// A known variant, or E17: the program file is the known one, the game data is not; the variant id is
// "custom-" + the first 8 hex digits of DATA.AFS's SHA-1, so the player can use it anyway.
DiscIdentity match_variant(const DiscTable& table, const std::string& sha1, DiscIdentity id) {
    for (auto& v : table.variants)
        if (v.data_afs_sha1 == sha1) {
            id.variant_id = v.id; id.description = v.description; id.known = true;
            return id;
        }
    id.known = false; id.reason = DiscVerdict::UnknownData; id.error = ErrorId::E17; id.detail = sha1;
    id.variant_id = "custom-" + sha1.substr(0, 8);
    id.description = "Custom game data";
    return id;
}

// A folder holding the game's files (E03 when it does not).
DiscIdentity identify_folder(const std::filesystem::path& dir, const DiscTable& table, const ProgressFn& progress) {
    DiscIdentity id;
    auto fail = [&](DiscVerdict why, ErrorId e, std::string detail = {}) {
        id.known = false; id.reason = why; id.error = e; id.detail = std::move(detail);
        return id;
    };
    const auto cnf = find_in_folder(dir, "SYSTEM.CNF"), elf = find_in_folder(dir, "SLPS_256.75");
    if (cnf.empty() || elf.empty()) return fail(DiscVerdict::NotDon, ErrorId::E03);
    const auto afs = find_in_folder(dir, "DATA.AFS");
    if (afs.empty()) return fail(DiscVerdict::NotDon, ErrorId::E15, "no DATA.AFS");
    std::error_code ec;
    const uint64_t total = std::filesystem::file_size(elf, ec) + std::filesystem::file_size(afs, ec);
    uint64_t done = 0;
    Sha256 s256;
    Sha1 s1;
    if (!hash_plain_file(elf, s256, done, total, "SLPS_256.75", progress) ||
        !hash_plain_file(afs, s1, done, total, "DATA.AFS", progress))
        return fail(DiscVerdict::Unreadable, ErrorId::E19, std::to_string(total ? done * 100 / total : 0));
    if (s256.finish() != table.elf_sha256) return fail(DiscVerdict::Modified, ErrorId::E16);
    return match_variant(table, s1.finish(), id);
}

}  // namespace

DiscIdentity identify_disc(const std::filesystem::path& image, const DiscTable& table, const ProgressFn& progress) {
    DiscIdentity id;
    auto fail = [&](DiscVerdict why, ErrorId e, std::string detail = {}) {
        id.known = false; id.reason = why; id.error = e; id.detail = std::move(detail);
        return id;
    };
    {
        std::error_code dec;
        if (std::filesystem::is_directory(image, dec)) return identify_folder(image, table, progress);
    }
    {
        errno = 0;
        std::ifstream probe(image, std::ios::binary);
        if (!probe) {
            int e = errno;  // EACCES: no permission (E05); ENOENT, ENODEV, EIO and anything else: gone (E06)
            return fail(DiscVerdict::Unreadable, e == EACCES ? ErrorId::E05 : ErrorId::E06, std::strerror(e));
        }
    }
    const Classification cls = classify(image);
    const bool partial = cls.detail == "still downloading";  // E09 is a hint: it wins only when the file is unusable
    std::error_code size_ec;
    uint64_t file_size = 0;
    if (std::filesystem::is_regular_file(image, size_ec)) {
        file_size = std::filesystem::file_size(image, size_ec);
    } else {  // a block device (the player's own drive, opened read-only) reports its size by seeking to the end
        std::ifstream dev(image, std::ios::binary);
        dev.seekg(0, std::ios::end);
        if (dev) file_size = uint64_t(std::max<std::streamoff>(0, dev.tellg()));
    }
    switch (cls.kind) {
        case Kind::TooSmall: {
            if (partial) return fail(DiscVerdict::Unreadable, ErrorId::E09, cls.detail);
            std::string ext = path_text(image.extension());  // E07 is for an empty file or a disc-image name; else E15
            for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
            const bool disc_ext = ext == ".iso" || ext == ".bin" || ext == ".img" || ext == ".mdf" || ext == ".nrg";
            if (file_size == 0 || disc_ext)
                return fail(DiscVerdict::Unreadable, ErrorId::E07, std::to_string(file_size) + " bytes");
            return fail(DiscVerdict::Unreadable, ErrorId::E15, cls.detail);
        }
        case Kind::Chd: case Kind::Cso: case Kind::Zso: case Kind::SevenZip: case Kind::Rar: case Kind::Ecm:
        case Kind::Nrg: case Kind::Mdf: case Kind::Cue:
            return fail(DiscVerdict::Unreadable, ErrorId::E10, kind_name(cls.kind));
        case Kind::Xdelta: case Kind::Ppf: case Kind::Ips: case Kind::Bps:
            return fail(DiscVerdict::NotDon, ErrorId::E12, kind_name(cls.kind));
        case Kind::GameCube: return fail(DiscVerdict::NotDon, ErrorId::E14);
        case Kind::Unknown:
            return fail(DiscVerdict::Unreadable, partial ? ErrorId::E09 : ErrorId::E15, cls.detail);
        case Kind::Folder: case Kind::DirectoryWithGame: break;  // folders are handled above, before classify
        case Kind::Iso: case Kind::Gz: case Kind::Zip: break;  // gz and zip are read as before (T-623b)
    }
    ImageReader img;
    std::string err;
    std::vector<IsoFile> files;
    if (!img.open(image, err)) return fail(DiscVerdict::Unreadable, ErrorId::E15, err);
    if (cls.kind == Kind::Iso) {
        // ISO 9660 primary volume descriptor: volume space size, a 32-bit field at byte 80 (ECMA-119 8.4.8).
        uint8_t pvd[kSector];
        uint64_t want = img.read_at(16ull * kSector, pvd, kSector) ? uint64_t(le32(pvd + 80)) * kSector : 0;
        if (want > file_size || !img.read_at(16ull * kSector, pvd, kSector))
            return fail(DiscVerdict::Unreadable, partial ? ErrorId::E09 : ErrorId::E08,
                        std::to_string(file_size) + " of " + std::to_string(want));
    }
    if (!list_files(img, files)) return fail(DiscVerdict::Unreadable, ErrorId::E15, "the file list can't be read");
    const IsoFile* elf = find_file(files, "SLPS_256.75");
    const IsoFile* afs = find_file(files, "DATA.AFS");
    if (!elf || !afs) {
        std::string serial = read_serial(img, files);
        if (!serial.empty() && serial != kDonSerial) return fail(DiscVerdict::NotDon, ErrorId::E13, serial);
        if (serial == kDonSerial && !elf) return fail(DiscVerdict::Modified, ErrorId::E16, serial);
        return fail(DiscVerdict::NotDon, ErrorId::E15, serial.empty() ? "no SYSTEM.CNF" : "no DATA.AFS");
    }
    uint64_t total = elf->size + afs->size, done = 0;
    Sha256 s256;
    Sha1 s1;
    // Stream in disc order so a compressed image is read once.
    bool elf_first = elf->lba <= afs->lba;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        if ((i == 0) == elf_first) ok = hash_file(img, *elf, s256, done, total, "SLPS_256.75", progress);
        else ok = hash_file(img, *afs, s1, done, total, "DATA.AFS", progress);
    }
    // E19: the percent of the two files read when the image stopped answering.
    if (!ok) return fail(DiscVerdict::Unreadable, ErrorId::E19, std::to_string(total ? done * 100 / total : 0));
    if (s256.finish() != table.elf_sha256) return fail(DiscVerdict::Modified, ErrorId::E16, read_serial(img, files));
    return match_variant(table, s1.finish(), id);
}

bool extract_disc(const std::filesystem::path& image, const std::filesystem::path& dest, const ProgressFn& progress,
                  std::string& error) {
    namespace fs = std::filesystem;
    ImageReader img;
    std::vector<IsoFile> files;
    if (!img.open(image, error) || !list_files(img, files)) {
        error = "This file isn't a disc image we can read.";
        return false;
    }
    uint64_t total = 0, done = 0;
    for (auto& f : files) total += f.size;
    std::vector<uint8_t> buf(1 << 20);
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (auto& f : files) {
        fs::path out = dest / path_from_text(f.path);
        fs::create_directories(out.parent_path(), ec);
        if (ec) { error = "Cannot create the folder " + path_text(out.parent_path()) + ": " + ec.message(); return false; }
        fs::path part = out;
        part += ".part";
        {
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            if (!o) { error = "Cannot write " + path_text(part); return false; }
            for (uint64_t off = 0; off < f.size;) {
                size_t n = size_t(std::min<uint64_t>(buf.size(), f.size - off));
                if (!img.read_at(f.lba * kSector + off, buf.data(), n)) {
                    o.close(); fs::remove(part, ec);
                    error = "The disc image ends before the end of " + f.path + ".";
                    return false;
                }
                o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n));
                if (!o) { o.close(); fs::remove(part, ec); error = "Cannot write " + path_text(out) + " (disk full?)"; return false; }
                off += n;
                done += n;
                if (progress) progress(total ? double(done) / double(total) : 1.0, f.path);
            }
            o.close();
            if (!o) { fs::remove(part, ec); error = "Cannot write " + path_text(out); return false; }
        }
        fs::rename(part, out, ec);
        if (ec) { error = "Cannot finish " + path_text(out) + ": " + ec.message(); return false; }
    }
    if (progress) progress(1.0, "done");
    return true;
}

}  // namespace don_setup
