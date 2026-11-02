// Tests of the time estimate (task T-630): schedules with known makespans, the smoothing and the strings.
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "estimate.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #c "\n"; ++g_fail; } } while (0)
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static std::vector<BatchCost> mk(std::initializer_list<std::pair<double, uint64_t>> l) {
    std::vector<BatchCost> v;
    for (auto& [s, r] : l) v.push_back({"", s, r});
    return v;
}

int main() {
    // Jobs limit: 4,3,3,2,2 on 2 workers, longest first: 4|3 -> 3 ends at 3, +3 -> 6; 4 ends at 4, +2 -> 6; 3+2 ->.. = 8
    // Worker A: 4, then 2, then ... trace: t0 start 4,3; t3 start 3 (ends 6); t4 start 2 (ends 6); t6 start 2 (ends 8).
    const auto a = mk({{2, 10}, {4, 10}, {3, 10}, {2, 10}, {3, 10}});
    CHECK(near(estimate_makespan(a, 2, 1000, 1.0), 8));
    CHECK(near(estimate_makespan(a, 1, 1000, 1.0), 14));
    CHECK(near(estimate_makespan(a, 5, 1000, 1.0), 4));
    CHECK(near(estimate_makespan(a, 99, 1000, 1.0), 4));
    CHECK(near(estimate_makespan(a, 0, 1000, 1.0), 14));  // jobs < 1 counts as 1
    CHECK(near(estimate_makespan(a, 2, 1000, 1.5), 12));  // the speed factor scales
    CHECK(near(estimate_makespan({}, 4, 1000, 1.0), 0));

    // Memory limits parallelism: four 5 s batches of 600 MB, 4 jobs, 1000 MB -> one at a time = 20 s.
    const auto m = mk({{5, 600}, {5, 600}, {5, 600}, {5, 600}});
    CHECK(near(estimate_makespan(m, 4, 1000, 1.0), 20));
    CHECK(near(estimate_makespan(m, 4, 1200, 1.0), 10));   // two fit
    CHECK(near(estimate_makespan(m, 4, 2400, 1.0), 5));    // all fit
    // A batch bigger than the budget runs alone and still finishes.
    CHECK(near(estimate_makespan(mk({{3, 5000}, {2, 100}}), 2, 1000, 1.0), 5));

    // module_build.toml
    std::vector<BatchCost> p;
    std::string err;
    const std::string toml =
        "# header\nversion = 3\n[[batch]]\nname = \"b0\"\nseconds = 2.5  # note\npeak_rss_mb = 800\n\n"
        "[[batch]]\nseconds = 1\npeak_rss_mb = 0\n[other]\nseconds = -1\n";
    CHECK(parse_module_build_toml(toml, p, err));
    CHECK(p.size() == 2 && p[0].name == "b0" && near(p[0].seconds, 2.5) && p[0].peak_rss_mb == 800);
    CHECK(p.size() == 2 && near(p[1].seconds, 1) && p[1].peak_rss_mb == 0);
    CHECK(!parse_module_build_toml("[[batch]]\nseconds = 1\n", p, err) && err.find("peak_rss_mb") != std::string::npos);
    CHECK(!parse_module_build_toml("[[batch]]\nseconds = 0\npeak_rss_mb = 1\n", p, err));
    CHECK(!parse_module_build_toml("[[batch]]\nseconds abc\n", p, err) && err.find("line 2") == 0);
    CHECK(parse_module_build_toml("", p, err) && p.empty());

    CHECK(near(speed_factor(6, 3), 2));
    CHECK(near(speed_factor(0, 3), 1));

    // Rate and smoothing. Steady 10 units/s over 1000 units.
    EtaTracker t;
    CHECK(t.update(0, 1000, 0, 0) < 0);                  // no rate yet
    CHECK(near(t.update(100, 1000, 10, 0), 90));          // 900 left at 10/s
    CHECK(near(t.update(200, 1000, 20, 0), 80));
    // The PC slows down to 1 unit/s: the rate is weighted (0.3*1 + 0.7*10 = 7.3), the shown value is capped.
    const double before = t.shown();
    const double slow = t.update(201, 1000, 21, 0);
    CHECK(slow > before && slow <= before * 1.2 + 1e-9);
    // Here the cap binds: raw = 799 / 7.3 = 109.45 s is above 80 * 1.2, so exactly the cap is shown.
    CHECK(799 / 7.3 > before * 1.2);
    CHECK(near(slow, 96));
    // Here it does not: a mild slowdown to 9 units/s gives rate 0.3*9 + 0.7*10 = 9.7, raw = 791 / 9.7 = 81.55 s,
    // below the cap of 96 s, so the raw value is shown.
    EtaTracker mild;
    mild.update(0, 1000, 0, 0);
    mild.update(100, 1000, 10, 0);
    mild.update(200, 1000, 20, 0);
    CHECK(near(mild.shown(), 80));
    CHECK(791 / 9.7 < mild.shown() * 1.2);
    CHECK(near(mild.update(209, 1000, 21, 0), 791 / 9.7));
    // Capped growth: it keeps rising by at most 20 % per update while the raw value is far above.
    double prev = slow;
    for (int i = 0; i < 5; ++i) {
        const double s = t.update(202 + i, 1000, 22 + i, 0);
        CHECK(s <= prev * 1.2 + 1e-9);
        prev = s;
    }
    // A stage change releases the limit: the new value is the raw estimate.
    const double raw_jump = t.update(208, 1000, 29, 1);
    CHECK(raw_jump > prev * 1.2);
    // Falling is never delayed.
    EtaTracker f;
    f.update(0, 100, 0, 0);
    f.update(1, 100, 1, 0);
    const double hi = f.shown();
    CHECK(f.update(90, 100, 2, 0) < hi);

    // Strings.
    CHECK(format_eta(0) == "Less than a minute left");
    CHECK(format_eta(59) == "Less than a minute left");
    CHECK(format_eta(60) == "About 1 min left");
    CHECK(format_eta(89) == "About 1 min left");
    CHECK(format_eta(12 * 60) == "About 12 min left");
    CHECK(format_eta(12 * 60 + 29) == "About 12 min left");
    CHECK(format_eta(12 * 60 + 31) == "About 13 min left");
    CHECK(format_eta(90 * 60) == "About 90 min left");
    CHECK(format_eta(91 * 60) == "About 1 h 31 min left");
    CHECK(format_eta(125 * 60) == "About 2 h 5 min left");
    CHECK(format_eta(180 * 60) == "About 3 h left");
    CHECK(format_eta(12 * 60, true) == "about 12 min left");
    CHECK(format_eta(125 * 60, true) == "about 2 h 5 min left");
    CHECK(format_eta(30, true) == "less than a minute left");
    CHECK(format_eta(-1) == "Estimating time left\xE2\x80\xA6");

    // T-718: copy+translate reach 15 % in 60 s, then build does 85 % in 480 s. Measured from the stage's own start
    // the time left is unknown until the build did kEtaMinStageFraction of its range, then close to the truth.
    {
        const double s0 = 0.15, build_s = 480;
        for (double f : {0.16, 0.18, 0.20}) {
            const double in_stage = (f - s0) / 0.85 * build_s, truth = build_s - in_stage;
            const double got = stage_seconds_left(f, s0, in_stage, 1.0, kEtaMinStageFraction);
            CHECK(got < 0 || (got > truth * 0.5 && got < truth * 1.5));
        }
        const double f = 0.5, in_stage = (f - s0) / 0.85 * build_s;
        CHECK(std::fabs(stage_seconds_left(f, s0, in_stage, 1.0, kEtaMinStageFraction) - (build_s - in_stage)) < 1e-6);
        CHECK(stage_seconds_left(0.15, s0, 0, 1.0, kEtaMinStageFraction) == kSecondsLeftUnknown);
    }

    // T-718 relaunch: the play test of f18d746 (log timing: `setup progress` prints at most once a second and at each
    // stage change; the log shows `0 copy`, `5 translate` x2, `11 translate`, `15 translate`, `15 verify`, `15 build`:
    // the log has no clock; the text "About 1 min" needs the old value in 60..90 s, i.e. copy+translate took 10.6 to 15.9 s, not a minute). The old extrapolation, elapsed * (1 - f) / (f - first) with
    // first = 0 (the first progress is at 0.0) and elapsed since the job start, then said about a minute at 15 %.
    {
        auto old_seconds_left = [](double f, double elapsed_since_start) { return elapsed_since_start * (1.0 - f) / (f - 0.0); };
        for (double fast_part_s : {11.0, 13.0, 15.5}) {
            const double old_left = old_seconds_left(0.15, fast_part_s);   // 62 to 88 s
            CHECK(old_left >= 60 && old_left < 90);
            CHECK(format_eta(old_left) == "About 1 min left");
            // the new rule: the build stage has just begun (stage_start 0.15, 0 s into it), the true time is minutes
            CHECK(stage_seconds_left(0.15, 0.15, 0.0, 1.0, kEtaMinStageFraction) == kSecondsLeftUnknown);
            CHECK(format_eta(-1.0) == "Estimating time left…");
            CHECK(format_eta(0.0) == "Less than a minute left");
        }
    }

    if (g_fail == 0) std::cout << "test_setup_estimate: OK\n";
    return g_fail == 0 ? 0 : 1;
}
