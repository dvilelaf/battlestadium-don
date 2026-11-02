// The COPY REPORT text of the first-run setup (docs/tasks/M6-public-release.md, A1): one block of plain text the
// player pastes in a bug report when something fails. It holds versions, ids, the error, the machine and the last
// 50 log lines.
//
// Layout: one "key: value" line per field, in this order (program, module, variant, error, error_id, os, cpu, ram,
// free_space, toolchain_hash), then a line "--- log (last 50 lines) ---" and the log lines.
//
// Scrubbing (applied to every line of the report):
//   1. A log line that looks like the recompiler's generated game source is replaced by "[source line removed]".
//      It is any line that contains "generated/" or one of the markers in kSourceMarkers (the generated code's
//      register accesses such as "ctx->" and "READ32("). Field values are never generated source.
//   2. Every occurrence of the home folder becomes "~". An empty home disables this step.
// The header values are single-line: a newline inside one becomes a space.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "setup_types.h"

namespace don_setup {

// How many log lines the report keeps: the last ones.
inline constexpr std::size_t kReportLogLines = 50;

struct ReportInput {
    std::string program_version;
    std::string module_id;
    std::string variant_id;
    ErrorId error = ErrorId::None;
    std::string os;
    std::string cpu;
    std::string ram;
    std::string free_space;
    std::string toolchain_hash;
    std::vector<std::string> log_lines;  // oldest first
};

// The full report text, ending with a newline. `home` is the user's home folder as the system gives it.
std::string make_report(const ReportInput& input, const std::string& home);

}  // namespace don_setup
