// A MAME CHD disc image read as the 2048-byte sectors of its ISO 9660 data (issue #6), so the first-run setup takes
// a .chd made by chdman as it takes a .iso: "createdvd" images (PS2 DVDs, this game) and "createcd" images (the first
// data track: MODE1, MODE1_RAW, MODE2_RAW or MODE2_FORM1). Read with libchdr (third_party/pcsx2/3rdparty/libchdr,
// BSD-3-Clause), through a std::ifstream, so no OS file API is used here. A CHD that needs a parent CHD is refused.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace don_setup {

class ChdImage {
public:
    ChdImage();
    ~ChdImage();
    ChdImage(const ChdImage&) = delete;
    ChdImage& operator=(const ChdImage&) = delete;

    // Opens the CHD and finds its data. False with a short English reason in `err` ("needs its parent CHD"...).
    bool open(const std::filesystem::path& path, std::string& err);
    // Bytes of ISO data: sectors x 2048.
    uint64_t size() const;
    // Reads exactly n bytes of ISO data at off. False when the data ends first (`eof`) or a hunk can't be read or
    // decompressed (eof false).
    bool read(uint64_t off, void* buf, size_t n, bool& eof);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace don_setup
