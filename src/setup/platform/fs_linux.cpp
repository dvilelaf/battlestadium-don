// Linux part of discovery: is a folder on a network filesystem (task T-627).
#include "discover.h"

#include <sys/statfs.h>

namespace don_setup {

bool is_network_filesystem(const std::filesystem::path& dir) {
    // f_type values from statfs(2) and <linux/magic.h>.
    constexpr unsigned long kNfs = 0x6969, kSmb = 0x517B, kCifs = 0xFF534D42, kSmb2 = 0xFE534D42, kAfs = 0x5346414F,
                            kCeph = 0x00C36400, kNcp = 0x564C, kCoda = 0x73757245, k9p = 0x01021997;
    struct statfs st;
    if (statfs(dir.c_str(), &st) != 0) return false;
    switch (static_cast<unsigned long>(st.f_type)) {
        case kNfs: case kSmb: case kCifs: case kSmb2: case kAfs: case kCeph: case kNcp: case kCoda: case k9p:
            return true;
        default:
            return false;
    }
}

}  // namespace don_setup
