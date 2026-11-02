// Memory governor and sleep inhibit (task T-632; A4 E30, E31, E37). The caller samples twice a second and calls
// `Governor::update`; the governor owns no thread. Under memory pressure it says "hold new jobs" and names the
// newest running job so the engine can requeue it (E37).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "platform/machine.h"
#include "platform/process.h"

namespace don_setup {

// Thresholds. A4 gives the behaviour (E37); the numbers are stated choices.
constexpr uint64_t kHoldBelowAvailableKib = 1024ull * 1024;  // choice: under 1 GiB free-able, stop starting jobs
constexpr uint64_t kRequeueBelowAvailableKib = 512ull * 1024;  // choice: under 512 MiB, give a job's memory back
constexpr double kPsiSomeHold = 10.0;   // choice: 10 % of 10 s stalled; our choice
constexpr double kPsiFullRequeue = 1.0; // choice: any sustained "full" stall means nothing runs well
constexpr int kClearSamples = 6;        // choice: 3 s of calm (6 samples at 2 Hz) before new jobs start again
constexpr int kRequeueCooldownSamples = 8;  // choice: 4 s for a requeue to free memory before asking for another

struct GovernorDecision {
    bool hold_new_jobs = false;
    std::string requeue_job;  // id of the newest running job to requeue; empty when none
};

class Governor {
public:
    // `running` lists the ids of the running jobs, oldest first. A lone job is never requeued: it would only start
    // again with the same memory need.
    GovernorDecision update(const MemorySample& sample, const std::vector<std::string>& running);
    bool holding() const { return holding_; }

private:
    bool holding_ = false;
    int calm_ = 0;
    int cooldown_ = 0;
};

// True when `free_bytes` cannot hold `needed_bytes` (E30 at the start, E31 part-way).
inline bool disk_too_small(const DiskSpace& d, uint64_t needed_bytes) { return d.known && d.free_bytes < needed_bytes; }

// Keeps the machine from sleeping while the setup runs, with a `systemd-inhibit` child.
class SleepInhibitor {
public:
    ~SleepInhibitor() { stop(); }
    // `search_path` is a PATH-style list ("" means $PATH). Returns false, with `log` saying why, when
    // systemd-inhibit is missing or cannot start; the setup then goes on without it.
    bool start(const std::string& why, std::string& log, const std::string& search_path = "");
    void stop();
    bool active() const { return active_; }

private:
    Process proc_;
    bool active_ = false;
};

}  // namespace don_setup
