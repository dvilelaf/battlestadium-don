// Tests of the COPY REPORT text (T-642): home folder scrubbing, generated-source removal, the last 50 log lines and
// the A1 fields. No file is read.
#include <iostream>
#include <string>
#include <vector>

#include "errors.h"
#include "report.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

int main() {
    const std::string home = "/home/tester";
    const std::string generated =
        "ctx->gpr[4].u32 = READ32(ctx->gpr[5].u32 + 0x10);";  // one line of the recompiler's output

    ReportInput input;
    input.program_version = "0.9.3";
    input.module_id = "3f9c1a2b7d4e";
    input.variant_id = "usa-1.00";
    input.error = ErrorId::E06;
    input.os = "Linux 6.8.0 x86_64";
    input.cpu = "Intel Core i5-9600K";
    input.ram = "16 GB";
    input.free_space = "42.1 GB";
    input.toolchain_hash = "a1b2c3d4";

    // 60 lines: line 1 is the oldest. Line 5 (dropped) and line 58 hold the home folder; line 55 is generated code.
    for (int i = 1; i <= 60; ++i) {
        std::string line = "step " + std::to_string(i) + " ok";
        if (i == 5) line = "open " + home + "/.local/share/don/setup.json";
        if (i == 55) line = generated;
        if (i == 58) line = "cache at " + home + "/.local/share/don/cache/obj";
        input.log_lines.push_back(line);
    }
    input.cpu = "Intel Core\ni5-9600K";  // a newline in a header value becomes a space

    const std::string text = make_report(input, home);

    // Every A1 field is present as "key: value".
    CHECK(contains(text, "program: 0.9.3\n"));
    CHECK(contains(text, "module: 3f9c1a2b7d4e\n"));
    CHECK(contains(text, "variant: usa-1.00\n"));
    CHECK(contains(text, "error: " + std::string(error_info(ErrorId::E06).code) + "\n"));
    CHECK(contains(text, "error_id: 6\n"));
    CHECK(contains(text, "os: Linux 6.8.0 x86_64\n"));
    CHECK(contains(text, "cpu: Intel Core i5-9600K\n"));  // the newline became a space
    CHECK(contains(text, "ram: 16 GB\n"));
    CHECK(contains(text, "free_space: 42.1 GB\n"));
    CHECK(contains(text, "toolchain_hash: a1b2c3d4\n"));

    // Home is hidden, and no home path is left.
    CHECK(!contains(text, home));
    CHECK(contains(text, "cache at ~/.local/share/don/cache/obj"));

    // The generated line is replaced, and the code is not in the report.
    CHECK(!contains(text, "ctx->"));
    CHECK(!contains(text, "READ32("));
    CHECK(contains(text, "[source line removed]\n"));

    // Exactly the last 50 of the 60 log lines: lines 1-10 are out, line 11 is the first one in.
    CHECK(!contains(text, "step 10 ok"));
    CHECK(contains(text, "step 11 ok"));
    CHECK(contains(text, "step 60 ok"));

    // Count the log lines after the header: exactly 50.
    const std::string header = "--- log (last 50 lines) ---\n";
    const std::size_t at = text.find(header);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        std::string body = text.substr(at + header.size());
        std::size_t lines = 0;
        for (char c : body) {
            if (c == '\n') ++lines;
        }
        CHECK(lines == kReportLogLines);
    }

    if (g_fail == 0) std::cout << "test_setup_report: all checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
