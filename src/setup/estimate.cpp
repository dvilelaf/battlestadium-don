#include "estimate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <sstream>

namespace don_setup {

namespace {
constexpr double kSecondsPerMinute = 60.0;
constexpr long kMinutesPerHour = 60;
constexpr long kHourFormLimit = 90;  // above this many minutes the text also gives hours

std::string trim(const std::string& s) {
    const char* ws = " \t\r";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
}  // namespace

bool parse_module_build_toml(const std::string& text, std::vector<BatchCost>& out, std::string& error) {
    out.clear();
    std::istringstream in(text);
    std::string raw;
    int line_no = 0;
    bool in_batch = false, has_seconds = false, has_rss = false;
    auto fail = [&](const std::string& msg) {
        error = "line " + std::to_string(line_no) + ": " + msg;
        out.clear();
        return false;
    };
    auto close_batch = [&]() -> bool {
        if (!in_batch) return true;
        if (!has_seconds) return fail("batch without seconds");
        if (!has_rss) return fail("batch without peak_rss_mb");
        return true;
    };
    while (std::getline(in, raw)) {
        ++line_no;
        std::string line = trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        if (line == "[[batch]]") {
            if (!close_batch()) return false;
            out.emplace_back();
            in_batch = true;
            has_seconds = has_rss = false;
            continue;
        }
        if (line.front() == '[') {  // another table: batches have ended
            if (!close_batch()) return false;
            in_batch = false;
            continue;
        }
        if (!in_batch) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        BatchCost& b = out.back();
        if (key == "name") {
            if (val.size() < 2 || val.front() != '"' || val.back() != '"') return fail("name must be a string");
            b.name = val.substr(1, val.size() - 2);
        } else if (key == "seconds") {
            char* end = nullptr;
            const double v = std::strtod(val.c_str(), &end);
            if (val.empty() || *end != 0 || !(v > 0) || !std::isfinite(v)) return fail("seconds must be > 0");
            b.seconds = v;
            has_seconds = true;
        } else if (key == "peak_rss_mb") {
            char* end = nullptr;
            const long long v = std::strtoll(val.c_str(), &end, 10);
            if (val.empty() || *end != 0 || v < 0) return fail("peak_rss_mb must be an integer >= 0");
            b.peak_rss_mb = uint64_t(v);
            has_rss = true;
        }
    }
    if (!close_batch()) return false;
    error.clear();
    return true;
}

double estimate_makespan(const std::vector<BatchCost>& batches, unsigned jobs, uint64_t memory_budget_mb,
                         double speed) {
    if (batches.empty()) return 0;
    if (jobs < 1) jobs = 1;
    std::vector<const BatchCost*> order;
    for (const auto& b : batches) order.push_back(&b);
    std::stable_sort(order.begin(), order.end(),
                     [](const BatchCost* a, const BatchCost* b) { return a->seconds > b->seconds; });

    struct Running { double end; uint64_t rss; };
    std::vector<Running> running;
    uint64_t used = 0;
    double now = 0;
    for (const BatchCost* b : order) {
        for (;;) {
            const bool slot = running.size() < jobs;
            const bool fits = running.empty() || used + b->peak_rss_mb <= memory_budget_mb;
            if (slot && fits) break;
            auto first = std::min_element(running.begin(), running.end(),
                                          [](const Running& x, const Running& y) { return x.end < y.end; });
            now = first->end;
            used -= first->rss;
            running.erase(first);
        }
        running.push_back({now + b->seconds, b->peak_rss_mb});
        used += b->peak_rss_mb;
    }
    double end = now;
    for (const auto& r : running) end = std::max(end, r.end);
    return end * speed;
}

double speed_factor(double measured, double reference) {
    if (!(measured > 0) || !(reference > 0)) return 1.0;
    return measured / reference;
}

double EtaTracker::update(double done, double total, double now_s, int stage) {
    const bool stage_changed = started_ && stage != stage_;
    if (!started_) {
        started_ = true;
        last_done_ = done;
        last_t_ = now_s;
        stage_ = stage;
        return shown_;
    }
    stage_ = stage;
    const double dt = now_s - last_t_;
    if (dt > 0) {
        const double sample = (done - last_done_) / dt;
        rate_ = rate_ > 0 ? kRateWeight * sample + (1 - kRateWeight) * rate_ : sample;
        last_done_ = done;
        last_t_ = now_s;
    }
    if (!(rate_ > 0)) return shown_;
    const double raw = std::max(0.0, total - done) / rate_;
    if (shown_ < 0 || stage_changed) shown_ = raw;
    else shown_ = std::min(raw, shown_ * (1 + kMaxGrowth));
    return shown_;
}

double stage_seconds_left(double fraction, double stage_start, double stage_elapsed_s, double end, double min_fraction) {
    const double range = end - stage_start, did = fraction - stage_start;
    if (!(range > 0) || !(did > min_fraction * range)) return kSecondsLeftUnknown;
    return stage_elapsed_s * (end - fraction) / did;
}

std::string format_eta(double seconds, bool mid_sentence) {
    std::string text;
    if (!(seconds >= 0)) {
        text = "Estimating time left\xE2\x80\xA6";
    } else if (seconds < kSecondsPerMinute) {
        text = "Less than a minute left";  // A3 gives no form for under a minute
    } else {
        const long minutes = std::lround(seconds / kSecondsPerMinute);
        if (minutes <= kHourFormLimit) {
            text = "About " + std::to_string(minutes) + " min left";
        } else {
            const long h = minutes / kMinutesPerHour, m = minutes % kMinutesPerHour;
            // The hours form is not in A3; this wording is our choice.
            text = "About " + std::to_string(h) + " h" + (m ? " " + std::to_string(m) + " min" : "") + " left";
        }
    }
    if (mid_sentence) text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    return text;
}

}  // namespace don_setup
