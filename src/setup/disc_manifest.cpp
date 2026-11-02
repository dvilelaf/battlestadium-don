#include "disc_manifest.h"
#include "path_text.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <sstream>

#include "faults.h"
#include "iso_reader.h"

namespace don_setup {

bool parse_disc_manifest(const std::string& text, std::vector<DiscManifest>& out, std::string& error) {
    out.clear();
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    auto bad = [&](const char* why) { error = "disc_manifest.toml line " + std::to_string(lineno) + ": " + why; return false; };
    while (std::getline(in, line)) {
        ++lineno;
        auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        line = line.substr(b, line.find_last_not_of(" \t\r") - b + 1);
        if (line == "[[variant]]") { out.emplace_back(); continue; }
        if (line == "[[variant.file]]") {
            if (out.empty()) return bad("file before any variant");
            out.back().files.emplace_back();
            continue;
        }
        auto eq = line.find(" = ");
        if (eq == std::string::npos || out.empty()) return bad("expected key = value");
        std::string key = line.substr(0, eq), val = line.substr(eq + 3);
        bool quoted = val.size() >= 2 && val.front() == '"' && val.back() == '"';
        if (quoted) val = val.substr(1, val.size() - 2);
        DiscManifest& v = out.back();
        if (key == "id" && quoted) v.variant_id = val;
        else if (key == "serial" && quoted) v.serial = val;
        else if (v.files.empty()) return bad("unknown key");
        else if (key == "path" && quoted) v.files.back().path = val;
        else if (key == "sha256" && quoted) v.files.back().sha256 = val;
        else if (key == "size" && !quoted && !val.empty() && val.find_first_not_of("0123456789") == std::string::npos)
            v.files.back().size = std::stoull(val);
        else return bad("unknown key or bad value");
    }
    return true;
}

namespace {

// True when `p` already holds exactly the listed file: same size, same SHA-256 (resume, T-623b).
bool already_copied(const std::filesystem::path& p, const ManifestFile& want) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || std::filesystem::file_size(p, ec) != want.size) return false;
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> buf(1 << 20);
    Sha256 h;
    while (in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size())) || in.gcount() > 0)
        h.update(buf.data(), size_t(in.gcount()));
    return h.finish() == want.sha256;
}

// The file of a folder input that `rel` ('/' separated) names, matched without regard to case; empty when absent.
std::filesystem::path find_in_folder(const std::filesystem::path& dir, const std::string& rel) {
    namespace fs = std::filesystem;
    auto lower = [](std::string x) {
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return x;
    };
    fs::path cur = dir;
    size_t at = 0;
    while (at <= rel.size()) {
        size_t slash = rel.find('/', at);
        if (slash == std::string::npos) slash = rel.size();
        const std::string part = lower(rel.substr(at, slash - at));
        fs::path next;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(cur, ec))
            if (lower(path_text(e.path().filename())) == part) { next = e.path(); break; }
        if (next.empty()) return {};
        cur = next;
        at = slash + 1;
    }
    return cur;
}

// The folder form of extract_disc: the same allow-list, the same SHA-256 check of every file, the same error ids.
bool extract_folder(const std::filesystem::path& dir, const std::filesystem::path& dest, const ProgressFn& progress,
                    std::string& error, const DiscManifest& manifest, ErrorId* code, std::string* detail,
                    ExtractStats& st) {
    namespace fs = std::filesystem;
    auto fail = [&](ErrorId id, const std::string& why, const std::string& d = "") {
        if (code) *code = id;
        if (detail) *detail = d;
        error = why;
        return false;
    };
    auto damaged = [&](const std::string& what, const std::string& name) {
        return fail(ErrorId::E18, "Some files on this image are damaged (" + name + "): " + what + ".");
    };
    struct Item { fs::path src; const ManifestFile* want; };
    std::vector<Item> items;
    uint64_t total = 0, done = 0;
    for (const auto& m : manifest.files) {
        fs::path src = find_in_folder(dir, m.path);
        std::error_code ec;
        if (src.empty() || !fs::is_regular_file(src, ec)) return damaged("missing", m.path);
        if (fs::file_size(src, ec) != m.size) return damaged("wrong size", m.path);
        items.push_back({src, &m});
        total += m.size;
    }
    std::vector<uint8_t> buf(1 << 20);
    std::error_code ec;
    fs::create_directories(dest, ec);
    auto percent = [&] { return std::to_string(int(total ? 100.0 * double(done) / double(total) : 100.0)); };
    for (auto& it : items) {
        const ManifestFile& want = *it.want;
        fs::path out = dest / path_from_text(want.path);
        fs::create_directories(out.parent_path(), ec);
        if (ec) return fail(ErrorId::None, "Cannot create the folder " + path_text(out.parent_path()) + ": " + ec.message());
        fs::path part = out;
        part += ".part";
        if (already_copied(out, want)) {
            fs::remove(part, ec);
            ++st.skipped;
            done += want.size;
            if (progress) progress(total ? double(done) / double(total) : 1.0, want.path);
            continue;
        }
        std::ifstream in(it.src, std::ios::binary);
        if (!in) return fail(ErrorId::E19, "Reading stopped at " + percent() + " % (input/output error).", percent());
        Sha256 hash;
        {
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            if (!o) return fail(ErrorId::None, "Cannot write " + path_text(part));
            auto drop = [&] { o.close(); fs::remove(part, ec); };
            for (uint64_t off = 0; off < want.size;) {
                size_t n = size_t(std::min<uint64_t>(buf.size(), want.size - off));
                double frac = total ? double(done) / double(total) : 1.0;
                if (fault_at(FaultKind::Eio, FaultStage::Read, frac)) {
                    drop();
                    return fail(fault_error(FaultKind::Eio), "Reading stopped at " + percent() + " % (input/output error).", percent());
                }
                in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n));
                if (size_t(in.gcount()) != n) {
                    drop();
                    return fail(ErrorId::E19, "Reading stopped at " + percent() + " % (input/output error).", percent());
                }
                if (fault_at(FaultKind::Enospc, FaultStage::Copy, frac)) {
                    drop();
                    return fail(fault_error(FaultKind::Enospc), "Cannot write " + path_text(out) + " (disk full?)");
                }
                hash.update(buf.data(), n);
                o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n));
                if (!o) {
                    bool full = errno == ENOSPC;
                    drop();
                    return full ? fail(fault_error(FaultKind::Enospc), "Cannot write " + path_text(out) + " (disk full?)")
                                : fail(ErrorId::None, "Cannot write " + path_text(out));
                }
                off += n;
                done += n;
                if (progress) progress(total ? double(done) / double(total) : 1.0, want.path);
            }
            o.close();
            if (!o) { fs::remove(part, ec); return fail(ErrorId::None, "Cannot write " + path_text(out)); }
        }
        if (hash.finish() != want.sha256) {
            fs::remove(part, ec);
            return damaged("hash mismatch", want.path);
        }
        fs::rename(part, out, ec);
        if (ec) return fail(ErrorId::None, "Cannot finish " + path_text(out) + ": " + ec.message());
        ++st.copied;
    }
    if (progress) progress(1.0, "done");
    return true;
}

}  // namespace

bool extract_disc(const std::filesystem::path& image, const std::filesystem::path& dest, const ProgressFn& progress,
                  std::string& error, const DiscManifest& manifest, ErrorId* code, std::string* detail,
                  ExtractStats* stats) {
    namespace fs = std::filesystem;
    ExtractStats local;
    ExtractStats& st = stats ? *stats : local;
    st = ExtractStats{};
    std::error_code dir_ec;
    if (fs::is_directory(image, dir_ec)) return extract_folder(image, dest, progress, error, manifest, code, detail, st);
    auto fail = [&](ErrorId id, const std::string& why, const std::string& d = "") {
        if (code) *code = id;
        if (detail) *detail = d;
        error = why;
        return false;
    };
    ImageReader img;
    std::vector<IsoFile> files;
    if (!img.open(image, error)) {
        if (!img.zip_problem().empty())
            return fail(ErrorId::E11, "This zip can't be used (" + img.zip_problem() + ").", img.zip_problem());
        return fail(ErrorId::None, "This file isn't a disc image we can read.");
    }
    uint64_t want_bytes = 0;  // where the last listed file ends: the "want" of E08
    auto cut_short = [&](const std::string& what) {
        switch (img.fail()) {
            case ReadFail::Io: return fail(ErrorId::E19, "Reading stopped (input/output error).", "0");
            case ReadFail::Corrupt:
            case ReadFail::Eof:
            default:
                return fail(ErrorId::E08, "The disc image is incomplete or damaged (" + what + ").",
                            std::to_string(img.have()) + " of " + std::to_string(want_bytes));
        }
    };
    if (!list_files(img, files)) {
        if (img.fail() != ReadFail::None) return cut_short("while reading its directory");
        return fail(ErrorId::None, "This file isn't a disc image we can read.");
    }
    auto damaged = [&](const std::string& what, const std::string& name) {
        return fail(ErrorId::E18, "Some files on this image are damaged (" + name + "): " + what + ".");
    };
    // Resolve every listed file first, so a missing one fails before anything is written.
    struct Item { const IsoFile* file; const ManifestFile* want; };
    std::vector<Item> items;
    uint64_t total = 0, done = 0;
    for (auto& m : manifest.files) {
        const IsoFile* f = find_file(files, m.path.c_str());
        if (!f) return damaged("missing", m.path);
        if (f->size != m.size) return damaged("wrong size", m.path);
        items.push_back({f, &m});
        total += f->size;
        want_bytes = std::max(want_bytes, f->lba * kSector + f->size);
    }
    // Stream in disc order so a compressed image is read once (E21).
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.file->lba < b.file->lba; });
    std::vector<uint8_t> buf(1 << 20);
    std::error_code ec;
    fs::create_directories(dest, ec);
    auto percent = [&] { return std::to_string(int(total ? 100.0 * double(done) / double(total) : 100.0)); };
    for (auto& it : items) {
        const IsoFile& f = *it.file;
        fs::path out = dest / path_from_text(it.want->path);
        fs::create_directories(out.parent_path(), ec);
        if (ec) return fail(ErrorId::None, "Cannot create the folder " + path_text(out.parent_path()) + ": " + ec.message());
        fs::path part = out;
        part += ".part";
        if (already_copied(out, *it.want)) {
            fs::remove(part, ec);  // a stale partial copy of a file that is already whole
            ++st.skipped;
            done += f.size;
            if (progress) progress(total ? double(done) / double(total) : 1.0, it.want->path);
            continue;
        }
        Sha256 hash;
        {
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            if (!o) return fail(ErrorId::None, "Cannot write " + path_text(part));
            auto drop = [&] { o.close(); fs::remove(part, ec); };
            for (uint64_t off = 0; off < f.size;) {
                size_t n = size_t(std::min<uint64_t>(buf.size(), f.size - off));
                double frac = total ? double(done) / double(total) : 1.0;
                if (fault_at(FaultKind::Eio, FaultStage::Read, frac)) {
                    drop();
                    return fail(fault_error(FaultKind::Eio), "Reading stopped at " + percent() + " % (input/output error).", percent());
                }
                if (!img.read_at(f.lba * kSector + off, buf.data(), n)) {
                    drop();
                    if (img.fail() == ReadFail::Io)
                        return fail(ErrorId::E19, "Reading stopped at " + percent() + " % (input/output error).", percent());
                    return cut_short("in " + f.path);
                }
                if (fault_at(FaultKind::Enospc, FaultStage::Copy, frac)) {
                    drop();
                    return fail(fault_error(FaultKind::Enospc), "Cannot write " + path_text(out) + " (disk full?)");
                }
                hash.update(buf.data(), n);
                o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n));
                if (!o) {
                    bool full = errno == ENOSPC;
                    drop();
                    return full ? fail(fault_error(FaultKind::Enospc), "Cannot write " + path_text(out) + " (disk full?)")
                                : fail(ErrorId::None, "Cannot write " + path_text(out));
                }
                off += n;
                done += n;
                if (progress) progress(total ? double(done) / double(total) : 1.0, it.want->path);
            }
            o.close();
            if (!o) { fs::remove(part, ec); return fail(ErrorId::None, "Cannot write " + path_text(out)); }
        }
        if (hash.finish() != it.want->sha256) {
            fs::remove(part, ec);
            return damaged("hash mismatch", it.want->path);
        }
        fs::rename(part, out, ec);
        if (ec) return fail(ErrorId::None, "Cannot finish " + path_text(out) + ": " + ec.message());
        ++st.copied;
    }
    st.restarts = img.restarts();
    // The rest of a gz/zip stream is read too, so its CRC and length are checked.
    if (!img.drain()) return cut_short("its checksum does not match");
    if (progress) progress(1.0, "done");
    return true;
}

}  // namespace don_setup
