#include "error_values.h"

#include <cstdio>

namespace don_setup {

bool parse_have_of_total(const std::string& detail, uint64_t& have, uint64_t& total) {
    unsigned long long h = 0, t = 0;
    int used = 0;
    if (std::sscanf(detail.c_str(), "%llu of %llu%n", &h, &t, &used) != 2) return false;
    if (std::size_t(used) != detail.size()) return false;
    have = h;
    total = t;
    return true;
}

std::string format_gb(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", double(bytes) / double(1ull << 30));
    return buf;
}

}  // namespace don_setup
