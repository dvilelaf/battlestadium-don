// PrepareEngine with a fake runner on tiny synthetic files: journal atomicity, resume, stop/continue, speed.
#include "prepare_engine.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace don_setup;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static fs::path g_dir;

struct FakeRunner : JobRunner {
    std::map<std::string, int> runs;
    Speed last_speed = Speed::Fast;
    bool run(const Job& j, Speed s) override {
        ++runs[j.id];
        last_speed = s;
        std::ofstream(j.output, std::ios::binary) << "output of " << j.id;
        return true;
    }
};

static std::vector<Job> jobs(const fs::path& d) {
    return {{"a", 1, d / "a.bin", Stage::Copy}, {"b", 3, d / "b.bin", Stage::Translate},
            {"c", 6, d / "c.bin", Stage::Build}};
}

static fs::path fresh(const std::string& name) {
    fs::path d = g_dir / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

static void test_journal_kill() {
    fs::path d = fresh("kill");
    Journal j;
    j.speed = Speed::Gentle;
    j.done = {{"a\"x", "11"}, {"b", "22"}};
    CHECK(write_journal(d, j));
    // A kill in the middle of the next write: half a temp file, no rename.
    std::ofstream(d / "setup.json.tmp") << "{\"speed\":\"fast\",\"do";
    Journal back;
    CHECK(read_journal(d, back));
    CHECK(back.speed == Speed::Gentle && back.done.size() == 2 && back.done[0].id == "a\"x" &&
          back.done[1].sha256 == "22");
    // A torn journal itself is rejected, not half-read.
    std::ofstream(d / "setup.json") << "{\"speed\":\"fast\",\"do";
    CHECK(!read_journal(d, back));
}

static void test_resume() {
    fs::path d = fresh("resume");
    {
        FakeRunner r;
        PrepareEngine e(d, jobs(d), r);
        while (!e.finished()) e.poll();
        CHECK(r.runs["a"] == 1 && r.runs["b"] == 1 && r.runs["c"] == 1);
    }
    std::ofstream(d / "b.bin") << "changed";  // output no longer matches its journal hash
    FakeRunner r2;
    PrepareEngine e2(d, jobs(d), r2);
    Progress p;
    while (!e2.finished()) p = e2.poll();
    CHECK(r2.runs.count("a") == 0 && r2.runs["b"] == 1 && r2.runs.count("c") == 0);
    CHECK(p.stage == Stage::Done && p.fraction == 1.0);
}

static void test_stop_continue_speed() {
    fs::path d = fresh("stop");
    FakeRunner r;
    PrepareEngine e(d, jobs(d), r);
    Progress p = e.poll();
    CHECK(p.parts_done == 1 && p.fraction == 0.1);
    e.stop();
    CHECK(e.stopped() && !e.finished());
    Journal j;
    CHECK(read_journal(d, j) && j.done.size() == 1 && j.done[0].id == "a");  // stop kept the journal
    e.set_speed(Speed::Gentle);
    while (!e.finished()) e.poll();
    CHECK(!e.stopped() && r.last_speed == Speed::Gentle);
    CHECK(r.runs["a"] == 1 && r.runs["b"] == 1 && r.runs["c"] == 1);
}

// Asynchronous fake: a job finishes on its third check(); "bad" fails; counts how many run at once.
struct AsyncRunner : JobRunner {
    std::map<std::string, int> started, left;
    std::vector<std::string> order, cancelled;
    size_t now = 0, peak = 0;
    bool async() const override { return true; }
    void cancel(const Job& j) override { cancelled.push_back(j.id); --now; left.erase(j.id); }
    bool run(const Job&, Speed) override { return false; }
    bool start(const Job& j, Speed) override {
        ++started[j.id];
        order.push_back(j.id);
        left[j.id] = 3;
        peak = std::max(peak, ++now);
        return true;
    }
    State check(const Job& j) override {
        if (--left[j.id] > 0) return State::Running;
        --now;
        if (j.id == "bad") return State::Failed;
        std::ofstream(j.output, std::ios::binary) << "output of " << j.id;
        return State::Done;
    }
};

static std::vector<Job> many(const fs::path& d, const std::string& bad = "") {
    std::vector<Job> v;
    for (int i = 0; i < 6; ++i) {
        std::string id = "j" + std::to_string(i);
        if (id == bad) id = "bad";
        v.push_back({id, 1, d / (id + ".bin"), Stage::Build});
    }
    v.push_back({"link", 1, d / "link.bin", Stage::Build, true});
    return v;
}

static void test_parallel() {
    fs::path d = fresh("par");
    AsyncRunner r;
    PrepareEngine e(d, many(d), r);
    e.set_max_parallel(3);
    Progress p;
    while (!e.finished()) p = e.poll();
    CHECK(r.peak == 3);
    CHECK(r.order.back() == "link" && r.started.size() == 7 && p.stage == Stage::Done && p.fraction == 1.0);
    for (int i = 0; i < 6; ++i) CHECK(r.started["j" + std::to_string(i)] == 1);
    // Resume: nothing proven done is started again; a removed object is.
    fs::remove(d / "j2.bin");
    AsyncRunner r2;
    PrepareEngine e2(d, many(d), r2);
    e2.set_max_parallel(3);
    while (!e2.finished()) e2.poll();
    CHECK(r2.started.size() == 2 && r2.started["j2"] == 1 && r2.started["link"] == 1);  // the link follows a rerun
}

// A stop request (T-649): nothing new starts, the running jobs are cancelled, the finished ones stay in the journal, and
// the next engine on the same journal only runs what is left.
static void test_halt_and_resume() {
    fs::path d = fresh("halt");
    AsyncRunner r;
    PrepareEngine e(d, many(d), r);
    e.set_max_parallel(2);
    while (e.ran() < 2) e.poll();
    e.poll();   // some jobs are running now
    CHECK(!r.left.empty());
    const size_t started = r.started.size();
    e.halt();
    CHECK(e.halted() && e.idle() && !e.finished() && !e.failed());
    CHECK(!r.cancelled.empty() && r.now == 0);
    e.poll();
    CHECK(r.started.size() == started);   // nothing starts after the halt
    Journal j;
    CHECK(read_journal(d, j) && j.done.size() == e.ran());
    AsyncRunner r2;
    PrepareEngine e2(d, many(d), r2);
    e2.set_max_parallel(2);
    while (!e2.finished()) e2.poll();
    CHECK(e2.proven() == j.done.size() && e2.proven() + e2.ran() == 7);   // each finished job once, never again
}

static void test_parallel_failure() {
    fs::path d = fresh("parfail");
    AsyncRunner r;
    PrepareEngine e(d, many(d, "j1"), r);
    e.set_max_parallel(2);
    Progress p;
    for (int i = 0; i < 100 && !(e.failed() && e.idle()); ++i) p = e.poll();
    CHECK(e.failed() && e.idle() && !e.finished());
    CHECK(p.stage == Stage::Failed && p.detail == "job failed: bad");
    CHECK(r.started.count("link") == 0 && r.now == 0);
    size_t started = r.order.size();
    e.poll();
    CHECK(r.order.size() == started);  // nothing starts after a failure
    Journal j;
    CHECK(read_journal(d, j));
    bool bad_done = false;
    for (auto& x : j.done) bad_done = bad_done || x.id == "bad";
    CHECK(!bad_done);
    // The next session recompiles only what is missing.
    AsyncRunner r2;
    PrepareEngine e2(d, many(d), r2);
    e2.set_max_parallel(2);
    while (!e2.finished()) e2.poll();
    CHECK(r2.started["j1"] == 1 && r2.started.count("j0") == 0 && r2.started["link"] == 1);
}

int main(int argc, char** argv) {
    g_dir = fs::temp_directory_path() / "don_setup_engine_test";
    (void)argc; (void)argv;
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    test_journal_kill();
    test_resume();
    test_stop_continue_speed();
    test_parallel();
    test_parallel_failure();
    test_halt_and_resume();
    fs::remove_all(g_dir);
    if (g_fail) { std::cerr << g_fail << " failure(s)\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
