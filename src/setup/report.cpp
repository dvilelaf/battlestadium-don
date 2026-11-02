// The COPY REPORT text (T-642). The rules are documented in report.h.
#include "report.h"

#include <array>

#include "errors.h"

namespace don_setup {

namespace {

// Markers of the recompiler's output: a log line with one of them is game code, not a message.
const std::array<const char*, 11> kSourceMarkers = {
    "generated/", "ctx->", "READ8(", "READ16(", "READ32(", "READ64(", "WRITE8(", "WRITE16(", "WRITE32(", "WRITE64(",
    "WRITE128(",
};

const char* const kLogHeader = "--- log (last 50 lines) ---";
const char* const kRemovedLine = "[source line removed]";

bool looks_like_source(const std::string& line) {
    for (const char* marker : kSourceMarkers) {
        if (line.find(marker) != std::string::npos) return true;
    }
    return false;
}

// Replaces every occurrence of `from` in `text` with `to`. An empty `from` changes nothing.
std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::string single_line(std::string value) {
    for (char& c : value) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return value;
}

std::string scrub(const std::string& text, const std::string& home) {
    return replace_all(text, home, "~");
}

}  // namespace

std::string make_report(const ReportInput& input, const std::string& home) {
    std::string out;
    auto field = [&out, &home](const char* key, const std::string& value) {
        out += key;
        out += ": ";
        out += scrub(single_line(value), home);
        out += '\n';
    };

    field("program", input.program_version);
    field("module", input.module_id);
    field("variant", input.variant_id);
    if (input.error == ErrorId::None) {
        field("error", "none");
    } else {
        field("error", error_info(input.error).code);
    }
    field("error_id", std::to_string(static_cast<unsigned>(input.error)));
    field("os", input.os);
    field("cpu", input.cpu);
    field("ram", input.ram);
    field("free_space", input.free_space);
    field("toolchain_hash", input.toolchain_hash);

    out += kLogHeader;
    out += '\n';

    // The last kReportLogLines lines; the first ones are dropped.
    std::size_t first = input.log_lines.size() > kReportLogLines ? input.log_lines.size() - kReportLogLines : 0;
    for (std::size_t i = first; i < input.log_lines.size(); ++i) {
        const std::string& line = input.log_lines[i];
        out += looks_like_source(line) ? kRemovedLine : scrub(line, home);
        out += '\n';
    }
    return out;
}

}  // namespace don_setup
