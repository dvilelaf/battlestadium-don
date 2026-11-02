// The engine that prepares the game: a queue of weighted jobs, a journal on disk (setup.json) that survives
// a kill at any moment, resume without redoing finished work, stop/continue and a speed setting.
// No threads: poll() advances the work by one job, or, with an asynchronous runner, keeps up to
// set_max_parallel() jobs running at once (the jobs are child processes; poll() only starts and reaps them).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "setup_types.h"

namespace don_setup {

enum class Speed { Fast, Gentle };

struct Job {
    std::string id;
    double weight = 1.0;       // relative cost; the estimate (T-630) and the progress fraction read it
    std::filesystem::path output;  // file the job leaves behind; its SHA-256 is the proof the job is done
    Stage stage = Stage::Copy;
    bool barrier = false;      // asynchronous runner: starts only when nothing else runs, and nothing starts beside it
};

// Runs one job. Returns true when the job finished and its output is in place.
class JobRunner {
public:
    virtual ~JobRunner() = default;
    virtual bool run(const Job& job, Speed speed) = 0;

    // Asynchronous runners (child processes) return true from async() and implement start/check; the engine then
    // never calls run(). start() returns false when the job could not even begin. check() never blocks.
    enum class State { Running, Done, Failed };
    virtual bool async() const { return false; }
    virtual bool start(const Job&, Speed) { return false; }
    virtual State check(const Job&) { return State::Failed; }
    // Stops a job that start() began and check() has not reported ended, and reaps it. Its output is not a proof.
    virtual void cancel(const Job&) {}
};

// One finished job as the journal records it.
struct JournalEntry {
    std::string id;
    std::string sha256;
};

struct Journal {
    Speed speed = Speed::Fast;
    std::vector<JournalEntry> done;
};

// Journal file: writes setup.json in `dir` atomically (temp file, flush, fsync, rename over the old one).
bool write_journal(const std::filesystem::path& dir, const Journal& j);
// Reads it back; false when missing or not a journal written by write_journal.
bool read_journal(const std::filesystem::path& dir, Journal& out);
std::string sha256_of_file(const std::filesystem::path& p);  // empty when the file cannot be read

class PrepareEngine {
public:
    PrepareEngine(std::filesystem::path journal_dir, std::vector<Job> jobs, JobRunner& runner);

    void set_speed(Speed s);
    Speed speed() const { return speed_; }
    // Finishes nothing new: the journal is already on disk after each job; the next poll() continues.
    void stop();
    // How many jobs of an asynchronous runner run at once (at least 1; default 1).
    void set_max_parallel(size_t n) { max_parallel_ = n < 1 ? 1 : n; }
    // Runs at most one job (skipping those the journal proves done); with an asynchronous runner it starts the jobs
    // that fit and reaps the finished ones. Returns the overall progress. After a failure nothing new starts; the jobs
    // already running are still reaped, so no child outlives the last poll() of a failed build.
    Progress poll();
    bool finished() const { return next_ >= jobs_.size() && running_.empty() && !failed_; }
    bool idle() const { return running_.empty(); }  // no job of an asynchronous runner is running
    bool failed() const { return failed_; }
    bool stopped() const { return stopped_; }
    // A stop request for an asynchronous runner: no new job starts from now on, and the running ones are cancelled
    // (runner.cancel) and reaped before this returns. The journal keeps the jobs that finished; the next engine on the
    // same journal continues from them. Not undone by poll().
    void halt();
    bool halted() const { return halted_; }
    // How many jobs the journal proved done without running them (a resumed run), and how many ran to the end.
    size_t proven() const { return proven_; }
    size_t ran() const { return ran_; }

private:
    void save();
    bool is_done(const Job& j) const;
    double fraction() const;
    Progress poll_async();

    std::filesystem::path dir_;
    std::vector<Job> jobs_;
    JobRunner& runner_;
    Speed speed_ = Speed::Fast;
    Journal journal_;
    size_t next_ = 0;                // first job not yet started (async) / not yet finished (sync)
    size_t max_parallel_ = 1;
    size_t failed_index_ = 0;
    bool ran_any_ = false;           // some job ran in this session (a barrier job is then never proven done)
    std::vector<size_t> running_;    // jobs started and not yet reaped (async only)
    std::vector<char> complete_;     // per job: finished or proven done by the journal (async only)
    bool stopped_ = false;
    bool halted_ = false;
    size_t proven_ = 0, ran_ = 0;
    bool failed_ = false;
};

}  // namespace don_setup
