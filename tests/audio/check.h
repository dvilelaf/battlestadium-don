// Minimal check macro for the audio tests: prints each failed check on stderr, the program exits non-zero.
// Same shape as tests/input/check.h.
#pragma once

#include <cstdio>

inline int &check_failures()
{
    static int n = 0;
    return n;
}

#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++check_failures();                                                           \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do                                                                                                     \
    {                                                                                                      \
        const long long va_ = static_cast<long long>(a), vb_ = static_cast<long long>(b);                  \
        if (va_ != vb_)                                                                                    \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, \
                         va_, #b, vb_);                                                                    \
            ++check_failures();                                                                            \
        }                                                                                                  \
    } while (0)

inline int check_result(const char *name)
{
    if (check_failures())
    {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, check_failures());
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}
