#include "disc_drive.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "discover.h"

namespace fs = std::filesystem;

namespace don_setup {
namespace {

constexpr int kLastOpticalDrive = 15;  // /dev/sr0 ... /dev/sr15

// The list of DON_TEST_DISC_DEVICES, separated by kDeviceListSeparator, or the optical drives when it is not set. Empty items are skipped.
std::vector<fs::path> device_list() {
    std::vector<fs::path> out;
    const char* env = std::getenv("DON_TEST_DISC_DEVICES");
    if (!env) {
        for (int i = 0; i <= kLastOpticalDrive; ++i) out.push_back("/dev/sr" + std::to_string(i));
        return out;
    }
    std::string list = env;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(kDeviceListSeparator, start);
        if (end == std::string::npos) end = list.size();
        if (end > start) out.emplace_back(list.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

}  // namespace

// is_this_game opens the device for reading only (no write, no eject, no lock). A drive that is empty, missing or
// unreadable fails that open or the ISO check, so it is skipped without an error.
std::optional<fs::path> find_game_disc() {
    for (const fs::path& dev : device_list())
        if (is_this_game(dev)) return dev;
    return std::nullopt;
}

}  // namespace don_setup
