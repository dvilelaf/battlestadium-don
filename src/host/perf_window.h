// SPDX-License-Identifier: GPL-3.0-or-later
// T-757: the "last window" fields of the `[don] perf t=...` line. The line's older fields are totals since the
// start of the run, so they cannot say when a drop happened. This turns two successive readings of the same
// cumulative counters into the figures of the period between them. Header only, so tests/host can check it.
//
// The worst frame time of a window is exact: the caller reads it with a take-and-reset on the GS glue.
#pragma once

#include <cstdint>

namespace perfwin
{
    struct Totals
    {
        double t = 0.0;        // seconds since the start of the run
        uint64_t vblanks = 0;  // frames shown since the start
        uint64_t slow = 0;     // frames over 33 ms since the start
        uint64_t window_max_us = 0; // worst frame time of the period since the previous reading (already reset)
    };

    struct Result
    {
        double seconds = 0.0;
        uint64_t frames = 0;
        double fps = 0.0;
        double avg_ms = 0.0;
        uint64_t slow = 0;
        uint64_t max_us = 0;
    };

    class Window
    {
    public:
        // Returns the figures since the previous call (or since t=0 on the first one) and remembers `now`.
        Result advance(const Totals &now)
        {
            Result r;
            r.seconds = now.t > m_prev.t ? now.t - m_prev.t : 0.0;
            r.frames = now.vblanks > m_prev.vblanks ? now.vblanks - m_prev.vblanks : 0;
            r.slow = now.slow > m_prev.slow ? now.slow - m_prev.slow : 0;
            r.fps = r.seconds > 0.0 ? static_cast<double>(r.frames) / r.seconds : 0.0;
            r.avg_ms = r.frames ? r.seconds * 1000.0 / static_cast<double>(r.frames) : 0.0;
            r.max_us = now.window_max_us;
            m_prev = now;
            return r;
        }

    private:
        Totals m_prev;
    };
    // T-772: the minimum of frames over the 10 s windows of play. A window that starts before the first frame is
    // shown is the game loading, not play, so it is not counted; a window that starts later and has no frames is
    // a real stall and is counted. With no counted window there is no figure.
    class WorstWindow
    {
    public:
        void add(uint64_t frames_at_window_start, uint64_t frames_in_window)
        {
            if (frames_at_window_start == 0)
                return;
            if (!m_has || frames_in_window < m_worst)
                m_worst = frames_in_window;
            m_has = true;
        }
        bool has() const { return m_has; }
        uint64_t worst() const { return m_worst; }

    private:
        bool m_has = false;
        uint64_t m_worst = 0;
    };
} // namespace perfwin
