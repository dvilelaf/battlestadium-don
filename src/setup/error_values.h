// Helpers that turn a DiscIdentity::detail into the values an error text asks for (placeholders in errors.h).
#pragma once

#include <cstdint>
#include <string>

namespace don_setup {

// Reads the documented E08 detail form "<have> of <want>" (bytes, setup_types.h). False when it has another form.
bool parse_have_of_total(const std::string& detail, uint64_t& have, uint64_t& total);

// Gigabytes (2^30 bytes, as file managers show them) with one decimal: 3328599654 -> "3.1".
std::string format_gb(uint64_t bytes);

}  // namespace don_setup
