#include "governor.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


namespace don_setup {

namespace {

bool under_pressure(const MemorySample& s) {
    // Without PSI (old kernels) MemAvailable alone decides.
    const bool low = s.available_known && s.available_kib < kHoldBelowAvailableKib;
    const bool stalled = s.psi_present && s.psi_some_avg10 >= kPsiSomeHold;
    return low || stalled;
}

bool severe(const MemorySample& s) {
    const bool very_low = s.available_known && s.available_kib < kRequeueBelowAvailableKib;
    const bool full = s.psi_present && s.psi_full_avg10 >= kPsiFullRequeue;
    return very_low || full;
}

}  // namespace

GovernorDecision Governor::update(const MemorySample& sample, const std::vector<std::string>& running) {
    GovernorDecision d;
    if (cooldown_ > 0) --cooldown_;
    if (under_pressure(sample)) {
        holding_ = true;
        calm_ = 0;
    } else if (holding_ && ++calm_ >= kClearSamples) {
        holding_ = false;
        calm_ = 0;
    }
    d.hold_new_jobs = holding_;
    if (holding_ && severe(sample) && cooldown_ == 0 && running.size() >= 2) {
        d.requeue_job = running.back();
        cooldown_ = kRequeueCooldownSamples;
    }
    return d;
}

#ifdef _WIN32

// Windows: the system stays awake while this thread's execution state asks for it. The state belongs to the calling
// thread, so start() and stop() must run on the same thread (the setup's main loop does both).
bool SleepInhibitor::start(const std::string&, std::string& log, const std::string&) {
    if (active_) return true;
    if (SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) == 0) {
        log = "[don-setup] SetThreadExecutionState failed; the PC may sleep during the setup";
        return false;
    }
    active_ = true;
    log = "[don-setup] sleep inhibited";
    return true;
}

void SleepInhibitor::stop() {
    if (!active_) return;
    SetThreadExecutionState(ES_CONTINUOUS);
    active_ = false;
}

#else

bool SleepInhibitor::start(const std::string& why, std::string& log, const std::string& search_path) {
    if (active_) return true;
    const std::string exe = find_in_path("systemd-inhibit", search_path);
    if (exe.empty()) {
        log = "[don-setup] systemd-inhibit not found; the PC may sleep during the setup";
        return false;
    }
    SpawnOptions o;
    o.argv = {exe, "--what=sleep", "--who=Battle Stadium D.O.N setup", "--why=" + why, "sleep", "infinity"};
    // The child needs the user's bus to reach logind; everything else stays out.
    o.env = inhibitor_environment();
    std::string error;
    if (!spawn(o, proc_, error)) {
        log = "[don-setup] could not start systemd-inhibit: " + error;
        return false;
    }
    active_ = true;
    log = "[don-setup] sleep inhibited";
    return true;
}

void SleepInhibitor::stop() {
    if (!active_) return;
    kill_group(proc_);
    wait(proc_);
    active_ = false;
}

#endif

}  // namespace don_setup
