// SPDX-License-Identifier: GPL-3.0-or-later
// T-305: how evenly the game is presented. DON_PACING_LOG=1 makes the host record, for every vertical blank,
// the wall time since the previous blank and its split, and print a summary when the run ends.
//
//   interval  wall time between this blank's presentation and the previous one's (steady clock)
//   renderer  time inside the GS library's vsync call (composing the picture and, with a window, the present,
//             which waits for the display's refresh when the vertical sync is on)
//   pacing    time the scheduler spent waiting for its host deadline (the idle sleep of patch 0038's pacing)
//   work      the rest: the game thread's own work (guest code, IOP, SPU2, GS packet handling)
//
// The sound is not on the game thread's path (the main thread pulls the SPU2 and writes the device); its
// worst write is reported separately. The arithmetic is here, away from the loop, so tests/host can check it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace pacing
{
    struct Sample
    {
        uint64_t blank = 0;
        uint64_t interval_ns = 0;
        uint64_t renderer_ns = 0;
        uint64_t pacing_ns = 0;
        uint64_t gif_ns = 0; // part of the work: the time inside the GS library's transfers (drawing calls) of this blank
        uint64_t work_ns() const
        {
            const uint64_t waits = renderer_ns + pacing_ns;
            return interval_ns > waits ? interval_ns - waits : 0;
        }
    };

    struct Summary
    {
        size_t count = 0;
        double median_ms = 0, p99_ms = 0, p999_ms = 0, max_ms = 0;
        size_t over20 = 0, over33 = 0, over50 = 0;
        double mean_work_ms = 0, mean_gif_ms = 0, mean_renderer_ms = 0, mean_pacing_ms = 0; // averages over the run
        std::vector<Sample> worst; // longest interval first, at most ten
    };

    // The value at quantile q (0..1) of an ascending list: the smallest element with at least q of the list at
    // or below it (nearest rank).
    inline uint64_t quantile_sorted(const std::vector<uint64_t> &sorted, double q)
    {
        if (sorted.empty())
            return 0;
        size_t rank = static_cast<size_t>(q * static_cast<double>(sorted.size()) + 0.999999999);
        if (rank < 1)
            rank = 1;
        if (rank > sorted.size())
            rank = sorted.size();
        return sorted[rank - 1];
    }

    inline Summary summarize(const std::vector<Sample> &samples, size_t worst_count = 10)
    {
        Summary s;
        s.count = samples.size();
        if (samples.empty())
            return s;
        std::vector<uint64_t> intervals;
        intervals.reserve(samples.size());
        double work = 0, gif = 0, renderer = 0, pacing = 0;
        for (const Sample &x : samples)
        {
            work += x.work_ns();
            gif += x.gif_ns;
            renderer += x.renderer_ns;
            pacing += x.pacing_ns;
            intervals.push_back(x.interval_ns);
            s.over20 += x.interval_ns > 20'000'000ull;
            s.over33 += x.interval_ns > 33'000'000ull;
            s.over50 += x.interval_ns > 50'000'000ull;
        }
        const double denom = static_cast<double>(samples.size()) * 1.0e6;
        s.mean_work_ms = work / denom;
        s.mean_gif_ms = gif / denom;
        s.mean_renderer_ms = renderer / denom;
        s.mean_pacing_ms = pacing / denom;
        std::sort(intervals.begin(), intervals.end());
        s.median_ms = quantile_sorted(intervals, 0.5) / 1.0e6;
        s.p99_ms = quantile_sorted(intervals, 0.99) / 1.0e6;
        s.p999_ms = quantile_sorted(intervals, 0.999) / 1.0e6;
        s.max_ms = intervals.back() / 1.0e6;
        s.worst = samples;
        const size_t n = std::min(worst_count, s.worst.size());
        std::partial_sort(s.worst.begin(), s.worst.begin() + static_cast<std::ptrdiff_t>(n), s.worst.end(),
                          [](const Sample &a, const Sample &b) { return a.interval_ns > b.interval_ns; });
        s.worst.resize(n);
        return s;
    }

    inline void print(std::FILE *out, const Summary &s, double audio_write_worst_ms)
    {
        std::fprintf(out,
                     "[don] pacing: %zu blanks, interval median %.2f ms, p99 %.2f ms, p99.9 %.2f ms, max %.2f ms; "
                     "later than 20 ms: %zu, 33 ms: %zu, 50 ms: %zu; worst sound write on the main thread %.2f ms\n",
                     s.count, s.median_ms, s.p99_ms, s.p999_ms, s.max_ms, s.over20, s.over33, s.over50,
                     audio_write_worst_ms);
        std::fprintf(out, "[don] pacing mean per blank: work %.2f ms (GS transfers %.2f) + renderer %.2f + pacing wait %.2f\n",
                     s.mean_work_ms, s.mean_gif_ms, s.mean_renderer_ms, s.mean_pacing_ms);
        for (const Sample &x : s.worst)
            std::fprintf(out,
                         "[don] pacing worst: blank %llu interval %.2f ms = work %.2f (of which GS transfers %.2f) + renderer %.2f + pacing wait %.2f\n",
                         static_cast<unsigned long long>(x.blank), x.interval_ns / 1.0e6, x.work_ns() / 1.0e6, x.gif_ns / 1.0e6,
                         x.renderer_ns / 1.0e6, x.pacing_ns / 1.0e6);
    }
} // namespace pacing
