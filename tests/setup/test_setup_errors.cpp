// Tests of the error table (T-633). Pure data: no file is read.
#include <cstring>
#include <iostream>
#include <set>
#include <string>

#include "errors.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

// A3 gives no width for the S5 panel, so the budget is 2 lines of 60 characters.
static const std::size_t kLineChars = 60;
static const std::size_t kMaxLines = 2;
static const std::size_t kMaxTitle = 40;

// Lines needed by `s` when wrapped greedily at word boundaries.
static std::size_t wrapped_lines(const std::string& s) {
    std::size_t lines = 1, col = 0, i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(' ', i);
        if (j == std::string::npos) j = s.size();
        const std::size_t w = j - i;
        if (col == 0) col = w;
        else if (col + 1 + w <= kLineChars) col += 1 + w;
        else { ++lines; col = w; }
        if (w > kLineChars) return 99;  // a word that never fits
        i = j + 1;
    }
    return lines;
}

// Entries for which A4 says nothing is shown to the player.
static bool silent(ErrorId id) {
    switch (id) {
        case ErrorId::E01: case ErrorId::E02: case ErrorId::E04: case ErrorId::E20: case ErrorId::E21:
        case ErrorId::E22: case ErrorId::E35: case ErrorId::E40: case ErrorId::E41: case ErrorId::E70:
        case ErrorId::E72: case ErrorId::E73: case ErrorId::E75: case ErrorId::E76: case ErrorId::E80:
        case ErrorId::E82: case ErrorId::E83:
            return true;
        default:
            return false;
    }
}

int main() {
    std::size_t n = 0;
    const ErrorEntry* t = error_entries(&n);
    CHECK(n == 59);  // E01-E22, E30-E42, E50-E59, E70-E83

    std::set<std::string> codes;
    std::set<int> ids;
    for (std::size_t i = 0; i < n; ++i) {
        const ErrorInfo& e = t[i].info;
        const bool is_silent = silent(t[i].id);
        CHECK(ids.insert(static_cast<int>(t[i].id)).second);
        CHECK(&error_info(t[i].id) == &e);
        CHECK(e.code && e.title && e.why && e.what_to_do && e.action);
        if (!(e.code && e.title && e.why && e.what_to_do && e.action)) continue;
        CHECK(std::strncmp(e.code, "E-", 2) == 0);
        CHECK(codes.insert(e.code).second);
        CHECK(e.title[0] != 0);
        CHECK(std::strlen(e.title) <= kMaxTitle);
        if (is_silent) {
            CHECK(e.why[0] == 0 && e.what_to_do[0] == 0 && e.action[0] == 0);
        } else {
            CHECK(e.why[0] != 0 && e.what_to_do[0] != 0 && e.action[0] != 0);
            CHECK(wrapped_lines(e.why) <= kMaxLines);
            CHECK(wrapped_lines(e.what_to_do) <= kMaxLines);
            for (const char* p = e.action; *p; ++p) CHECK(!(*p >= 'a' && *p <= 'z'));
        }
        if (g_fail) std::cerr << "  at " << e.code << "\n";
    }
    // An id outside the catalogue still gets a valid entry.
    CHECK(error_info(static_cast<ErrorId>(999)).code != nullptr);

    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
