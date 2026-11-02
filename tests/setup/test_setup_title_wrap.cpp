// Tests for the setup screen's title wrap (T-715: a long title wraps at a space; no SDL, no game file).
// The header is included here so one command builds the test.
#include "../../src/host/setup_title_wrap.h"

#include <cstdio>
#include <string>

using namespace don_setup;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        std::string _a = (a), _b = (b);                                                             \
        if (_a != _b) {                                                                             \
            ++g_fail;                                                                               \
            std::printf("FAIL %s:%d  [%s] != [%s]\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); \
        }                                                                                           \
    } while (0)

// T-715: a long title wraps at a space instead of running past the window edge (10 units per character here).
static void test_title_wrap() {
    const auto w = [](const std::string& s) { return 10.0f * static_cast<float>(s.size()); };
    const std::string desc = "English, Japanese voices (Lord Izen patch, 2023-09-03)";
    CHECK(wrap_title("Ready", 200.0f, 0.0f, w).size() == 1);
    CHECK(wrap_title(desc, 1000.0f, 0.0f, w).size() == 1);
    const auto lines = wrap_title(desc, 300.0f, 0.0f, w);
    CHECK(lines.size() == 2);
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        CHECK(w(lines[i]) <= 300.0f);
        joined += (i ? " " : "") + lines[i];
    }
    CHECK_EQ(joined, desc);
    // The tick's room is taken from the first line only.
    const auto ticked = wrap_title(desc, 300.0f, 60.0f, w);
    CHECK(ticked.size() == 2 && w(ticked[0]) <= 240.0f);
    // A word wider than the room stays whole; nothing is lost.
    const auto one = wrap_title("Supercalifragilistic words", 100.0f, 0.0f, w);
    CHECK(one.size() == 2 && one[0] == "Supercalifragilistic");
}

int main() {
    test_title_wrap();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
