// SPDX-License-Identifier: GPL-3.0-or-later
// T-305: the arithmetic of DON_PACING_LOG's summary (src/host/pacing_stats.h): nearest-rank quantiles, the
// counts of intervals later than 20, 33 and 50 ms, the ten worst, and the split's remainder.
#include "../../src/host/pacing_stats.h"

#include <cstdio>

namespace
{
    int failures = 0;
    void expect(bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    }
} // namespace

int main()
{
    std::vector<pacing::Sample> samples;
    for (uint64_t i = 1; i <= 1000; ++i)
    {
        pacing::Sample s;
        s.blank = i;
        s.interval_ns = 16'000'000ull; // 16 ms
        s.renderer_ns = 3'000'000ull;
        s.pacing_ns = 5'000'000ull;
        samples.push_back(s);
    }
    samples[9].interval_ns = 21'000'000ull;   // later than 20 ms
    samples[99].interval_ns = 34'000'000ull;  // later than 20 and 33 ms
    samples[499].interval_ns = 51'000'000ull; // later than all three
    samples[999].interval_ns = 20'000'000ull; // exactly 20 ms: not later than 20 ms
    const pacing::Summary s = pacing::summarize(samples);
    expect(s.count == 1000, "count");
    expect(s.over20 == 3 && s.over33 == 2 && s.over50 == 1, "counts of late blanks");
    expect(s.median_ms == 16.0, "median");
    expect(s.p99_ms == 16.0, "p99: rank 990 of 1000 is still 16 ms");
    expect(s.p999_ms == 34.0, "p99.9: rank 999 of 1000 is the second largest");
    expect(s.max_ms == 51.0, "max");
    expect(s.worst.size() == 10 && s.worst[0].blank == 500 && s.worst[1].blank == 100 && s.worst[2].blank == 10,
           "the worst, longest first");
    expect(s.worst[3].blank == 1000, "then the 20 ms one");
    expect(s.worst[0].work_ns() == 51'000'000ull - 8'000'000ull, "work is the interval minus the two waits");
    expect(s.mean_pacing_ms == 5.0 && s.mean_renderer_ms == 3.0, "means of the waits");
    pacing::Sample small;
    small.interval_ns = 1;
    small.renderer_ns = 5;
    expect(small.work_ns() == 0, "work never goes negative");
    expect(pacing::summarize({}).count == 0, "empty input");
    if (failures != 0)
        return 1;
    std::printf("pacing_stats_test: all checks passed\n");
    return 0;
}
