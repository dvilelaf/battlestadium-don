#include "iso_reader.h"

#include <cctype>
#include <set>

namespace don_setup {

std::string clean_name(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    auto semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool safe_name(const std::string& s) {
    return !s.empty() && s != "." && s != ".." && s.find_first_of("/\\:") == std::string::npos;
}


bool list_files(ImageReader& img, std::vector<IsoFile>& files) {
    uint8_t pvd[kSector];
    if (!img.read_at(16ull * kSector, pvd, kSector)) return false;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    struct Dir { uint64_t lba, size; std::string path; int depth; };
    std::vector<Dir> todo{{le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", 0}};
    std::set<uint64_t> seen;
    while (!todo.empty()) {
        auto next = std::min_element(todo.begin(), todo.end(), [](const Dir& a, const Dir& b) { return a.lba < b.lba; });
        Dir d = *next;
        todo.erase(next);
        if (d.depth > 16 || !seen.insert(d.lba).second || d.size > (64u << 20)) continue;
        std::vector<uint8_t> buf(d.size);
        if (!img.read_at(d.lba * kSector, buf.data(), buf.size())) return false;
        for (size_t off = 0; off < buf.size();) {
            size_t len = buf[off];
            if (len == 0) { off = (off / kSector + 1) * kSector; continue; }
            if (len < 34 || off + len > buf.size()) return false;
            const uint8_t* r = &buf[off];
            size_t nl = r[32];
            if (33 + nl > len) return false;
            off += len;
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;
            std::string name = clean_name(r + 33, nl);
            if (!safe_name(name)) continue;
            std::string rel = d.path.empty() ? name : d.path + "/" + name;
            if (r[25] & 2) todo.push_back({le32(r + 2), le32(r + 10), rel, d.depth + 1});
            else files.push_back({rel, le32(r + 2), le32(r + 10)});
        }
    }
    std::sort(files.begin(), files.end(), [](const IsoFile& a, const IsoFile& b) { return a.lba < b.lba; });
    return true;
}


bool read_root_file(ImageReader& img, const char* name, uint64_t max_size, std::string& out) {
    constexpr uint64_t kDescriptorEnd = 17ull * kSector;  // sectors 0-16 hold the system area and the first descriptor
    constexpr uint64_t kMaxRootExtent = 1u << 20;
    if (!img.size_known()) img.bound_to(kDescriptorEnd);
    uint8_t pvd[kSector];
    if (!img.read_at(16ull * kSector, pvd, kSector)) return false;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    if (!img.size_known()) img.bound_to(std::max<uint64_t>(kDescriptorEnd + kSector, uint64_t(le32(pvd + 80)) * kSector));
    uint64_t root_lba = le32(pvd + 156 + 2), root_size = le32(pvd + 156 + 10);
    if (root_size == 0 || root_size > kMaxRootExtent) return false;
    std::vector<uint8_t> buf(root_size);
    if (!img.read_at(root_lba * kSector, buf.data(), buf.size())) return false;
    for (size_t off = 0; off < buf.size();) {
        size_t len = buf[off];
        if (len == 0) { off = (off / kSector + 1) * kSector; continue; }
        if (len < 34 || off + len > buf.size()) return false;
        const uint8_t* r = &buf[off];
        size_t nl = r[32];
        if (33 + nl > len) return false;
        off += len;
        if ((r[25] & 2) || (nl == 1 && (r[33] == 0 || r[33] == 1))) continue;
        if (!ieq(clean_name(r + 33, nl), name)) continue;
        uint64_t size = le32(r + 10);
        if (size == 0 || size > max_size) return false;
        out.assign(size_t(size), '\0');
        return img.read_at(uint64_t(le32(r + 2)) * kSector, out.data(), out.size());
    }
    return false;
}

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

const IsoFile* find_file(const std::vector<IsoFile>& files, const char* name) {
    for (auto& f : files)
        if (ieq(f.path, name)) return &f;
    return nullptr;
}

bool ImageReader::open(const std::filesystem::path& path, std::string& err) {
    in_.open(path, std::ios::binary);
    if (!in_) { err = "cannot open the file"; return false; }
    in_.seekg(0, std::ios::end);
    auto end_pos = in_.tellg();
    size_known_ = end_pos > 0;
    file_size_ = size_known_ ? uint64_t(end_pos) : 0;
    uint8_t head[4] = {0, 0, 0, 0};
    in_.seekg(0);
    in_.read(reinterpret_cast<char*>(head), 4);
    in_.clear();
    if (head[0] == 0x1f && head[1] == 0x8b) {
        mode_ = Mode::Gzip; base_ = 0; csize_ = file_size_;
    } else if (head[0] == 'P' && head[1] == 'K') {
        if (!open_zip(err)) return false;
    } else {
        mode_ = Mode::Raw;
    }
    return restart(err);
}

bool ImageReader::read_at(uint64_t off, void* buf, size_t n) {
    std::string err;
    fail_ = ReadFail::None;
    if (mode_ == Mode::Raw || mode_ == Mode::Stored) {
        uint64_t limit = mode_ == Mode::Raw ? file_size_ : csize_;
        if (off + n > limit) { fail_ = ReadFail::Eof; return false; }
        in_.clear();
        in_.seekg(std::streamoff(base_ + off));
        in_.read(static_cast<char*>(buf), std::streamsize(n));
        if (!in_) fail_ = in_.bad() ? ReadFail::Io : ReadFail::Eof;
        return bool(in_);
    }
    if (off < pos_) {
        ++restarts_;
        if (!restart(err)) { fail_ = ReadFail::Io; return false; }
    }
    std::array<uint8_t, 65536> scratch;
    while (pos_ < off) {
        size_t take = size_t(std::min<uint64_t>(off - pos_, scratch.size()));
        if (!inflate_some(scratch.data(), take)) return false;
    }
    return inflate_some(static_cast<uint8_t*>(buf), n);
}

bool ImageReader::drain() {
    if (!compressed()) return true;
    fail_ = ReadFail::None;
    std::array<uint8_t, 65536> scratch;
    while (!done_)
        if (!inflate_some(scratch.data(), scratch.size(), true)) return fail_ == ReadFail::None && done_;
    return fail_ == ReadFail::None;
}

uint64_t ImageReader::have() const {
    return mode_ == Mode::Raw ? file_size_ : mode_ == Mode::Stored ? csize_ : pos_;
}

bool ImageReader::open_zip(std::string& err) {
    auto problem = [&](const char* p) { zip_problem_ = p; err = std::string("zip: ") + p; return false; };
    // end of central directory
    size_t tail = size_t(std::min<uint64_t>(file_size_, 65557 + 20));
    std::vector<uint8_t> t(tail);
    in_.clear();
    in_.seekg(std::streamoff(file_size_ - tail));
    in_.read(reinterpret_cast<char*>(t.data()), std::streamsize(tail));
    if (!in_) { err = "unreadable zip"; return false; }
    size_t e = std::string::npos;
    for (size_t i = tail >= 22 ? tail - 22 + 1 : 0; i-- > 0;)
        if (le32(&t[i]) == 0x06054b50) { e = i; break; }
    if (e == std::string::npos) { err = "no zip directory"; return false; }
    // APPNOTE 4.3.16: a nonzero disk number, or fewer entries on this disk than in all, is a split archive.
    if (le16(&t[e + 4]) != 0 || le16(&t[e + 6]) != 0 || le16(&t[e + 8]) != le16(&t[e + 10])) return problem("split");
    uint64_t count = le16(&t[e + 10]), cd_off = le32(&t[e + 16]);
    if (cd_off == 0xFFFFFFFFu || count == 0xFFFF) {
        if (e < 20 || le32(&t[e - 20]) != 0x07064b50) { err = "bad zip64"; return false; }
        uint64_t eocd64 = le64(&t[e - 20 + 8]);
        uint8_t r[56];
        in_.clear();
        in_.seekg(std::streamoff(eocd64));
        in_.read(reinterpret_cast<char*>(r), 56);
        if (!in_ || le32(r) != 0x06064b50) { err = "bad zip64"; return false; }
        count = le64(r + 32); cd_off = le64(r + 48);
    }
    in_.clear();
    in_.seekg(std::streamoff(cd_off));
    struct Member { uint32_t method, flags, crc; uint64_t cs, us, lo; };
    std::vector<Member> isos;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t h[46];
        in_.read(reinterpret_cast<char*>(h), 46);
        if (!in_ || le32(h) != 0x02014b50) { err = "bad zip directory"; return false; }
        Member m{le16(h + 10), le16(h + 8), le32(h + 16), le32(h + 20), le32(h + 24), le32(h + 42)};
        size_t nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32);
        std::string name(nl, '\0');
        std::vector<uint8_t> extra(xl);
        in_.read(name.data(), std::streamsize(nl));
        in_.read(reinterpret_cast<char*>(extra.data()), std::streamsize(xl));
        in_.ignore(std::streamsize(cl));
        if (!in_) { err = "bad zip directory"; return false; }
        std::string low = name;
        for (auto& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".iso") != 0) continue;
        if (le16(h + 34) != 0) return problem("split");  // the member starts on another disk
        for (size_t x = 0; x + 4 <= xl;) {
            size_t id = le16(&extra[x]), sz = le16(&extra[x + 2]), p = x + 4;
            if (id == 1) {
                if (m.us == 0xFFFFFFFFu && p + 8 <= x + 4 + sz) { m.us = le64(&extra[p]); p += 8; }
                if (m.cs == 0xFFFFFFFFu && p + 8 <= x + 4 + sz) { m.cs = le64(&extra[p]); p += 8; }
                if (m.lo == 0xFFFFFFFFu && p + 8 <= x + 4 + sz) { m.lo = le64(&extra[p]); }
            }
            x += 4 + sz;
        }
        isos.push_back(m);
    }
    if (isos.empty()) return problem("no-iso");
    if (isos.size() > 1) return problem("several-isos");
    const Member& m = isos[0];
    if (m.flags & 0x41) return problem("password");  // bit 0 encrypted, bit 6 strong encryption
    if (m.method != 0 && m.method != 8) return problem("unsupported-method");
    uint8_t lh[30];
    in_.clear();
    in_.seekg(std::streamoff(m.lo));
    in_.read(reinterpret_cast<char*>(lh), 30);
    if (!in_ || le32(lh) != 0x04034b50) { err = "bad zip entry"; return false; }
    base_ = m.lo + 30 + le16(lh + 26) + le16(lh + 28);
    csize_ = m.method == 0 ? m.us : m.cs;
    zip_usize_ = m.us;
    zip_crc_ = m.crc;
    mode_ = m.method == 0 ? Mode::Stored : Mode::Deflate;
    return true;
}

bool ImageReader::restart(std::string& err) {
    end_inflate();
    pos_ = 0;
    crc_ = 0;
    if (mode_ == Mode::Raw || mode_ == Mode::Stored) return true;
    std::memset(&zs_, 0, sizeof zs_);
    int rc = inflateInit2(&zs_, mode_ == Mode::Gzip ? 16 + MAX_WBITS : -MAX_WBITS);
    if (rc != Z_OK) { err = "zlib failure"; return false; }
    zs_ready_ = true;
    consumed_ = 0;
    zs_.avail_in = 0;
    done_ = false;
    return true;
}

void ImageReader::end_inflate() {
    if (zs_ready_) inflateEnd(&zs_);
    zs_ready_ = false;
}

bool ImageReader::inflate_some(uint8_t* out, size_t n, bool tolerate_end) {
    zs_.next_out = out;
    zs_.avail_out = uInt(n);
    bool ended = false;
    while (zs_.avail_out) {
        if (zs_.avail_in == 0) {
            if (done_) break;
            if (consumed_ >= csize_) { fail_ = ReadFail::Eof; return false; }  // cut off: no end-of-stream mark
            size_t want = size_t(std::min<uint64_t>(in_buf_.size(), csize_ - consumed_));
            in_.clear();
            in_.seekg(std::streamoff(base_ + consumed_));
            in_.read(reinterpret_cast<char*>(in_buf_.data()), std::streamsize(want));
            size_t got = size_t(in_.gcount());
            if (got == 0) { fail_ = in_.bad() ? ReadFail::Io : ReadFail::Eof; return false; }
            consumed_ += got;
            zs_.next_in = in_buf_.data();
            zs_.avail_in = uInt(got);
        }
        int rc = inflate(&zs_, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) { done_ = ended = true; break; }
        if (rc != Z_OK && rc != Z_BUF_ERROR) { fail_ = ReadFail::Corrupt; return false; }  // incl. gzip CRC/length
    }
    size_t produced = n - zs_.avail_out;
    pos_ += produced;
    if (mode_ == Mode::Deflate) crc_ = uint32_t(crc32(crc_, out, uInt(produced)));
    // A zip member has its CRC-32 and length in the directory; zlib does not check them for raw deflate.
    if (ended && mode_ == Mode::Deflate && (pos_ != zip_usize_ || crc_ != zip_crc_)) {
        fail_ = ReadFail::Corrupt;
        return false;
    }
    if (produced != n && !(tolerate_end && ended)) { fail_ = ReadFail::Eof; return false; }
    return true;
}

}  // namespace don_setup
