// Copy rules over every entry of the error table (T-702). The `why` sentence must not repeat its title, and no
// `what_to_do` may start with the prefix the screen already prints ("What you can do").
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "errors.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static const std::size_t kMinRepeatWords = 3;

// Lower-case words of `s`; every run of non-letters and non-digits separates two words.
static std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            cur += static_cast<char>(std::tolower(c));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Words joined with one space and padded on both sides, so that a search for " a b c " matches whole words only.
static std::string padded(const std::vector<std::string>& w) {
    std::string s = " ";
    for (const std::string& x : w) s += x + " ";
    return s;
}

// The first run of kMinRepeatWords or more consecutive words of `title` that also appears in `why`, or "".
static std::string repeated_run(const std::string& title, const std::string& why) {
    const std::vector<std::string> tw = words_of(title);
    const std::string hay = padded(words_of(why));
    for (std::size_t i = 0; i + kMinRepeatWords <= tw.size(); ++i) {
        for (std::size_t n = tw.size() - i; n >= kMinRepeatWords; --n) {
            std::string run;
            for (std::size_t k = 0; k < n; ++k) run += tw[i + k] + " ";
            if (hay.find(" " + run) != std::string::npos) return run;
        }
    }
    return "";
}

static bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

int main() {
    std::size_t n = 0;
    const ErrorEntry* t = error_entries(&n);
    CHECK(t != nullptr);
    CHECK(n > 0);

    std::size_t checked_why = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const ErrorInfo& e = t[i].info;
        const std::string code = e.code;

        if (e.why[0] != '\0') {
            ++checked_why;
            const std::string run = repeated_run(e.title, e.why);
            if (!run.empty()) {
                std::cerr << code << ": why repeats the title (\"" << e.title << "\" / \"" << e.why << "\"): run \""
                          << run << "\"\n";
                ++g_fail;
            }
        }

        if (starts_with_ci(e.what_to_do, "What you can do")) {
            std::cerr << code << ": what_to_do starts with the screen prefix: \"" << e.what_to_do << "\"\n";
            ++g_fail;
        }
    }
    CHECK(checked_why > 0);

    if (g_fail) {
        std::cerr << g_fail << " error copy rule(s) broken\n";
        return 1;
    }
    std::cout << "error copy: " << n << " entries, " << checked_why << " why sentences checked\n";
    return 0;
}
