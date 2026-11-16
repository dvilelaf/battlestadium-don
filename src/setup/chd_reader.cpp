#include "chd_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include <libchdr/chd.h>

namespace don_setup {

namespace {

constexpr uint32_t kIsoSector = 2048;
// chdman createdvd's metadata tag (MAME src/lib/util/chd.h DVD_METADATA_TAG; libchdr has no name for it).
constexpr uint32_t kDvdTag = CHD_MAKE_TAG('D', 'V', 'D', ' ');
// A CD frame in a CHD: 2352 bytes of sector and 96 of subcode (MAME CD_FRAME_SIZE).
constexpr uint32_t kCdFrame = 2352 + 96;

// The file behind libchdr's core_file. libchdr closes it (fclose) from chd_close, and also on most failed opens; the
// stream lives in ChdImage::Impl, which outlives the chd_file, so fclose only marks it closed.
struct Stream {
    core_file cf{};
    std::ifstream in;
    uint64_t size = 0;
};

Stream* stream_of(core_file* f) { return static_cast<Stream*>(f->argp); }

uint64_t cf_fsize(core_file* f) { return stream_of(f)->size; }

size_t cf_fread(void* buf, size_t size, size_t count, core_file* f) {
    std::ifstream& in = stream_of(f)->in;
    if (size == 0 || count == 0) return 0;
    in.clear();
    in.read(static_cast<char*>(buf), std::streamsize(size * count));
    return size_t(in.gcount()) / size;
}

int cf_fclose(core_file* f) {
    stream_of(f)->in.close();
    return 0;
}

int cf_fseek(core_file* f, int64_t off, int whence) {
    std::ifstream& in = stream_of(f)->in;
    in.clear();
    const std::ios::seekdir dir = whence == SEEK_SET ? std::ios::beg : whence == SEEK_CUR ? std::ios::cur : std::ios::end;
    in.seekg(std::streamoff(off), dir);
    return in ? 0 : -1;
}

// "TYPE:MODE2_RAW" -> "MODE2_RAW" from a CHT2/CHTR metadata text.
std::string field(const std::string& text, const char* key) {
    const size_t k = text.find(key);
    if (k == std::string::npos) return {};
    const size_t b = k + std::strlen(key);
    const size_t e = text.find(' ', b);
    return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

}  // namespace

struct ChdImage::Impl {
    Stream stream;
    chd_file* chd = nullptr;
    uint32_t hunk_bytes = 0;
    // Where sector s's 2048 bytes are in the CHD's logical data: s * stride + first_offset (DVD: 2048 and 0; CD:
    // one frame per sector, the user data at 0, 16 or 24 bytes into it, after the pregap frames stored in the file).
    uint64_t stride = kIsoSector, first_offset = 0, sectors = 0;
    std::vector<uint8_t> hunk;
    uint64_t cached = UINT64_MAX;

    ~Impl() {
        if (chd) chd_close(chd);
    }

    bool load_hunk(uint64_t index) {
        if (index == cached) return true;
        if (chd_read(chd, uint32_t(index), hunk.data()) != CHDERR_NONE) {
            cached = UINT64_MAX;
            return false;
        }
        cached = index;
        return true;
    }
};

ChdImage::ChdImage() : impl_(std::make_unique<Impl>()) {}
ChdImage::~ChdImage() = default;

bool ChdImage::open(const std::filesystem::path& path, std::string& err) {
    Impl& m = *impl_;
    m.stream.in.open(path, std::ios::binary);
    if (!m.stream.in) { err = "cannot open the file"; return false; }
    m.stream.in.seekg(0, std::ios::end);
    const auto end = m.stream.in.tellg();
    m.stream.size = end > 0 ? uint64_t(end) : 0;
    m.stream.in.seekg(0);
    m.stream.cf.argp = &m.stream;
    m.stream.cf.fsize = cf_fsize;
    m.stream.cf.fread = cf_fread;
    m.stream.cf.fclose = cf_fclose;
    m.stream.cf.fseek = cf_fseek;

    const chd_error e = chd_open_core_file(&m.stream.cf, CHD_OPEN_READ, nullptr, &m.chd);
    if (e != CHDERR_NONE) {
        m.chd = nullptr;
        err = e == CHDERR_REQUIRES_PARENT ? "CHD: it needs its parent CHD (make a standalone one with chdman)"
                                          : std::string("CHD: ") + chd_error_string(e);
        return false;
    }
    const chd_header* h = chd_get_header(m.chd);
    m.hunk_bytes = h->hunkbytes;
    if (m.hunk_bytes == 0 || m.hunk_bytes > (1u << 24)) { err = "CHD: unusable hunk size"; return false; }
    m.hunk.resize(m.hunk_bytes);

    char meta[512] = {};
    uint32_t len = 0;
    if (chd_get_metadata(m.chd, kDvdTag, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) == CHDERR_NONE) {
        m.stride = kIsoSector;
        m.first_offset = 0;
        m.sectors = h->logicalbytes / kIsoSector;
        return true;
    }
    // A CD: the first track has to be the data track.
    if (chd_get_metadata(m.chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) !=
            CHDERR_NONE &&
        chd_get_metadata(m.chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta) - 1, &len, nullptr, nullptr) !=
            CHDERR_NONE) {
        err = "CHD: neither a DVD nor a CD image";
        return false;
    }
    const std::string text(meta, std::min<size_t>(len, sizeof(meta) - 1));
    const std::string type = field(text, "TYPE:");
    uint64_t data_at = 0;
    if (type == "MODE1" || type == "MODE2_FORM1") data_at = 0;
    else if (type == "MODE1_RAW") data_at = 16;
    else if (type == "MODE2_RAW") data_at = 24;
    else { err = "CHD: the first track is " + (type.empty() ? std::string("of no known type") : type); return false; }
    const long frames = std::strtol(field(text, "FRAMES:").c_str(), nullptr, 10);
    const long pregap = std::strtol(field(text, "PREGAP:").c_str(), nullptr, 10);
    // A pregap whose PGTYPE starts with V is stored in the file before the track's first sector.
    const bool pregap_stored = field(text, "PGTYPE:").rfind('V', 0) == 0;
    if (h->unitbytes != kCdFrame || frames <= 0) { err = "CHD: unusable CD layout"; return false; }
    m.stride = kCdFrame;
    m.first_offset = uint64_t(pregap_stored && pregap > 0 ? pregap : 0) * kCdFrame + data_at;
    m.sectors = uint64_t(frames) - (pregap_stored && pregap > 0 ? uint64_t(pregap) : 0);
    return true;
}

uint64_t ChdImage::size() const { return impl_->sectors * kIsoSector; }

bool ChdImage::read(uint64_t off, void* buf, size_t n, bool& eof) {
    Impl& m = *impl_;
    eof = false;
    if (off + n > size()) { eof = true; return false; }
    uint8_t* out = static_cast<uint8_t*>(buf);
    while (n) {
        const uint64_t sector = off / kIsoSector;
        const uint32_t in_sector = uint32_t(off % kIsoSector);
        uint64_t logical = sector * m.stride + m.first_offset + in_sector;
        // The run of bytes that stays inside this sector's user data (and, below, inside one hunk).
        size_t take = std::min<size_t>(n, kIsoSector - in_sector);
        while (take) {
            const uint64_t index = logical / m.hunk_bytes;
            const uint32_t at = uint32_t(logical % m.hunk_bytes);
            const size_t part = std::min<size_t>(take, m.hunk_bytes - at);
            if (!m.load_hunk(index)) return false;
            std::memcpy(out, m.hunk.data() + at, part);
            out += part;
            logical += part;
            off += part;
            n -= part;
            take -= part;
        }
    }
    return true;
}

}  // namespace don_setup
