// Time estimate of the setup flow (task T-630): a schedule simulated before starting, and a smoothed rate while
// running. Pure functions and small value types; no file or process is touched.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace don_setup {

// One build batch as measured at release.
struct BatchCost {
    std::string name;
    double seconds = 0;          // wall time of the batch alone, on the reference PC
    uint64_t peak_rss_mb = 0;    // peak resident memory of its compiler process
};

// Parses the batch list of `config/module_build.toml`. Only this subset is read, everything else is ignored:
//   [[batch]]            starts a batch
//   name = "b0001"       string, optional
//   seconds = 2.5        number, required, > 0
//   peak_rss_mb = 800    integer, required
// Lines starting with `#` and blank lines are skipped; a `#` after a value ends it. Keys outside a [[batch]]
// table are ignored. On error returns false and sets `error` (with the line number).
bool parse_module_build_toml(const std::string& text, std::vector<BatchCost>& out, std::string& error);

// Seconds the batches take on `jobs` parallel workers sharing `memory_budget_mb`, times `speed`. Longest first:
// the next batch in descending time waits until a worker is free and its peak fits in the memory left. A batch
// bigger than the whole budget runs alone. Returns 0 for no batches; jobs < 1 counts as 1.
double estimate_makespan(const std::vector<BatchCost>& batches, unsigned jobs, uint64_t memory_budget_mb,
                         double speed);

// Speed factor of this PC: measured calibration seconds over the reference seconds (1.0 = as fast as the PC
// where the batch times were measured). Non-positive input gives 1.0.
double speed_factor(double measured_calibration_seconds, double reference_calibration_seconds);

// Time left while running: an exponentially weighted rate, and a shown value that grows by at most
// kMaxGrowth between two updates unless the stage changes.
class EtaTracker {
public:
    // Weight of the newest sample in the rate. A chosen smoothing weight, not a measured value: a larger one
    // follows a speed change faster but makes the time left jump more. T-636a may tune it on the owner's PC.
    static constexpr double kRateWeight = 0.3;
    static constexpr double kMaxGrowth = 0.2;   // +20 % between two updates

    // `done` and `total` in the same unit (job weights); `now_s` is a monotonic clock in seconds.
    // `stage` is any id; a different one than last time resets the limit (the first update has no limit).
    // Returns the seconds left to show, or a negative value while there is no rate yet.
    double update(double done, double total, double now_s, int stage);

    double shown() const { return shown_; }

private:
    bool started_ = false;
    double last_done_ = 0, last_t_ = 0, rate_ = 0, shown_ = -1;
    int stage_ = 0;
};

// Share of a stage's own range it must have done before its rate is trusted for the time left (T-718).
constexpr double kEtaMinStageFraction = 0.02;

// "Time left unknown" is a negative value end to end (format_eta shows "Estimating time left…"); 0 means "now".
constexpr double kSecondsLeftUnknown = -1.0;

// Time left from the rate of the stage that is running only: it started at overall fraction `stage_start`
// `stage_elapsed_s` seconds ago, is at `fraction` now and the run ends at `end`. Returns kSecondsLeftUnknown until the
// stage has done kEtaMinStageFraction-like `min_fraction` of its own range (end - stage_start). A rate from an
// earlier, faster stage is never used.
double stage_seconds_left(double fraction, double stage_start, double stage_elapsed_s, double end, double min_fraction);

// Screen text in A3's wording (docs/tasks/M6-public-release.md): "About 12 min left"; above 90 minutes
// "About 2 h 5 min left" (that form is our choice, A3 has none). Under a minute: "Less than a minute left"
// (A3 has no form for it). Rounds to the nearest minute. Negative gives "Estimating time left…".
// `mid_sentence` lowercases the first letter for the resume button: "about 12 min left".
std::string format_eta(double seconds, bool mid_sentence = false);

}  // namespace don_setup
