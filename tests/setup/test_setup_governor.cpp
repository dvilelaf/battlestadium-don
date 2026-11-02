// Tests for T-632: machine readers on fake /proc and /sys trees, governor decisions, sleep inhibit.
#include "governor.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <signal.h>
#include <thread>
#include <unistd.h>

using namespace don_setup;
namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

MemorySample sample(uint64_t kib, double some = 0, double full = 0, bool psi = true) {
    MemorySample s;
    s.available_known = true;
    s.available_kib = kib;
    s.psi_present = psi;
    s.psi_some_avg10 = some;
    s.psi_full_avg10 = full;
    return s;
}

void test_memory(const fs::path& root) {
    put(root / "proc/meminfo", "MemTotal:       16000000 kB\nMemAvailable:    2048000 kB\n");
    put(root / "proc/pressure/memory",
        "some avg10=12.50 avg60=3.00 avg300=1.00 total=100\nfull avg10=0.75 avg60=0.00 avg300=0.00 total=5\n");
    MemorySample s = read_memory(root.string());
    CHECK(s.available_known && s.available_kib == 2048000);
    CHECK(s.psi_present && s.psi_some_avg10 == 12.5 && s.psi_full_avg10 == 0.75);

    fs::remove(root / "proc/pressure/memory");
    s = read_memory(root.string());
    CHECK(s.available_known && !s.psi_present);

    s = read_memory((root / "nothing").string());
    CHECK(!s.available_known && !s.psi_present);
}

void test_power(const fs::path& root) {
    CHECK(!read_power(root.string()).on_battery);  // no power_supply folder: on mains
    const fs::path ps = root / "sys/class/power_supply";
    put(ps / "BAT0/type", "Battery\n");
    put(ps / "BAT0/status", "Discharging\n");
    put(ps / "BAT0/capacity", "63\n");
    put(ps / "AC/type", "Mains\n");
    put(ps / "AC/online", "0\n");
    PowerState p = read_power(root.string());
    CHECK(p.battery_present && p.on_battery && p.percent == 63);
    put(ps / "AC/online", "1\n");
    CHECK(!read_power(root.string()).on_battery);
    put(ps / "AC/online", "0\n");
    put(ps / "mouse/type", "Battery\n");
    put(ps / "mouse/scope", "Device\n");
    put(ps / "BAT0/status", "Charging\n");
    CHECK(!read_power(root.string()).on_battery);
}

void test_disk(const fs::path& root) {
    DiskSpace d = free_disk_space((root / "not/yet/created").string());
    CHECK(d.known && d.free_bytes > 0);
    CHECK(d.checked_path == fs::absolute(root).string());
}

void test_governor() {
    Governor g;
    const std::vector<std::string> two = {"old", "new"};
    GovernorDecision d = g.update(sample(4u << 20), two);
    CHECK(!d.hold_new_jobs && d.requeue_job.empty());

    d = g.update(sample(kHoldBelowAvailableKib - 1), two);  // low but not severe: hold only
    CHECK(d.hold_new_jobs && d.requeue_job.empty());

    d = g.update(sample(kRequeueBelowAvailableKib - 1), two);  // severe: newest job requeued
    CHECK(d.hold_new_jobs && d.requeue_job == "new");
    d = g.update(sample(kRequeueBelowAvailableKib - 1), two);  // cooldown
    CHECK(d.hold_new_jobs && d.requeue_job.empty());

    for (int i = 0; i < kClearSamples - 1; ++i) d = g.update(sample(4u << 20), two);
    CHECK(d.hold_new_jobs);  // hysteresis: still holding
    d = g.update(sample(4u << 20), two);
    CHECK(!d.hold_new_jobs);

    Governor psi;  // PSI alone triggers the hold, plenty of MemAvailable
    d = psi.update(sample(8u << 20, kPsiSomeHold), two);
    CHECK(d.hold_new_jobs && d.requeue_job.empty());
    d = psi.update(sample(8u << 20, kPsiSomeHold, kPsiFullRequeue), two);
    CHECK(d.requeue_job == "new");

    Governor lone;  // a lone job is never requeued
    d = lone.update(sample(1000), {"only"});
    CHECK(d.hold_new_jobs && d.requeue_job.empty());

    Governor nopsi;  // old kernel: MemAvailable decides
    d = nopsi.update(sample(kHoldBelowAvailableKib - 1, 99, 99, false), two);
    CHECK(d.hold_new_jobs);
    d = Governor().update(sample(8u << 20, 99, 99, false), two);
    CHECK(!d.hold_new_jobs);

    DiskSpace small;
    small.known = true;
    small.free_bytes = 100;
    CHECK(disk_too_small(small, 101) && !disk_too_small(small, 100));
}


void test_inhibit(const fs::path& root) {
    std::string log;
    {
        SleepInhibitor missing;
        CHECK(!missing.start("test", log, (root / "empty").string()));
        CHECK(!missing.active() && log.find("not found") != std::string::npos);
    }
    const fs::path bin = root / "bin";
    const fs::path pid_file = root / "inhibitor.pid";
    put(bin / "systemd-inhibit", "#!/bin/sh\necho $$ > '" + pid_file.string() + "'\nexec sleep 60\n");
    fs::permissions(bin / "systemd-inhibit", fs::perms::owner_all);
    SleepInhibitor inh;
    CHECK(inh.start("test", log, bin.string()));
    CHECK(inh.active());
    // The fake inhibitor writes its pid, then becomes `sleep`; wait for the file, then it must be alive.
    long pid = 0;
    for (int i = 0; i < 200 && pid <= 0; ++i) {
        std::ifstream in(pid_file);
        if (!(in >> pid)) pid = 0;
        if (pid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(pid > 0);
    CHECK(pid > 0 && kill(static_cast<pid_t>(pid), 0) == 0);
    inh.stop();
    CHECK(!inh.active());
    CHECK(pid > 0 && kill(static_cast<pid_t>(pid), 0) != 0);  // killed and reaped, not left behind
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("don-governor-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "empty");
    test_memory(root);
    test_power(root / "power");
    test_disk(root);
    test_governor();
    test_inhibit(root);
    fs::remove_all(root);
    if (failures) return 1;
    std::puts("governor tests passed");
    return 0;
}
