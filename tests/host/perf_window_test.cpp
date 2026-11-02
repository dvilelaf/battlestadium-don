// SPDX-License-Identifier: GPL-3.0-or-later
// T-757: the arithmetic of the "last window" fields of the perf line (header only, no game needed).
#include "../../src/host/perf_window.h"

#include <cmath>
#include <cstdio>

namespace
{
    int failures = 0;
    void check(bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    }
    bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }
} // namespace

int main()
{
    perfwin::Window w;

    // First window: from the start (t=0, nothing counted) to t=10 s, 600 frames, 2 slow, worst 40 ms.
    perfwin::Result r = w.advance({10.0, 600, 2, 40000});
    check(r.frames == 600, "first window frames");
    check(near(r.fps, 60.0), "first window fps");
    check(near(r.avg_ms, 1000.0 / 60.0), "first window avg ms");
    check(r.slow == 2, "first window slow");
    check(r.max_us == 40000, "first window max is the window worst");

    // Second window: a drop. 10 s, 450 frames, 5 more slow, its worst was 148 ms.
    r = w.advance({20.0, 1050, 7, 148000});
    check(r.frames == 450, "drop window frames");
    check(near(r.fps, 45.0), "drop window fps");
    check(near(r.avg_ms, 10000.0 / 450.0), "drop window avg ms");
    check(r.slow == 5, "drop window slow is the delta");
    check(r.max_us == 148000, "drop window max is its own worst");

    // Third window: recovered; its worst is 33 ms, below the earlier 148 ms (the glue reset it).
    r = w.advance({30.0, 1650, 7, 33000});
    check(r.frames == 600 && near(r.fps, 60.0), "recovered window");
    check(r.slow == 0, "recovered window has no slow frames");
    check(r.max_us == 33000, "recovered window max is lower than the earlier drop");

    // A window with no frames and no time elapsed must not divide by zero.
    r = w.advance({30.0, 1650, 7, 0});
    check(r.frames == 0 && near(r.fps, 0.0) && near(r.avg_ms, 0.0), "empty window is all zero");

    // Counters that go backwards (a reset) clamp to zero instead of wrapping.
    r = w.advance({40.0, 10, 0, 1000});
    check(r.frames == 0 && r.slow == 0, "backwards counters clamp to zero");

    // T-772: the worst-window figure only counts windows that start once a frame has been shown.
    {
        perfwin::WorstWindow ww;
        check(!ww.has(), "worst: empty has nothing");
        ww.add(0, 0); // loading: no frame yet
        check(!ww.has(), "worst: loading window is ignored");
        ww.add(0, 300); // first frame shown inside this window, it still started at frame 0
        check(!ww.has(), "worst: a window that starts at frame 0 is ignored even with frames");
        ww.add(300, 580);
        check(ww.has() && ww.worst() == 580, "worst: first play window counts");
        ww.add(880, 600);
        check(ww.worst() == 580, "worst: keeps the minimum");
        ww.add(1480, 0); // a real stall after play began
        check(ww.worst() == 0, "worst: a stall after the first frame counts");
    }

    if (failures == 0)
        std::printf("perf_window_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
