#include "prepare_engine.h"
#include "path_text.h"

#include <cstdio>
#include <fstream>
#include <iterator>

#include "iso_reader.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace don_setup {

namespace {

const char* kJournalName = "setup.json";
const char* kTempName = "setup.json.tmp";

// The only OS call of the engine: push a file's bytes to the disk.
bool sync_file(const fs::path& p) {
#ifdef _WIN32
    FILE* f = _wfopen(p.c_str(), L"rb+");
    if (!f) return false;
    bool ok = _commit(_fileno(f)) == 0;
    fclose(f);
    return ok;
#else
    FILE* f = std::fopen(path_text(p).c_str(), "rb+");
    if (!f) return false;
    bool ok = ::fsync(fileno(f)) == 0;
    std::fclose(f);
    return ok;
#endif
}

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

// Reads a quoted string starting at the opening quote; leaves `i` after the closing quote.
bool read_string(const std::string& t, size_t& i, std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out.clear();
    while (i < t.size() && t[i] != '"') {
        if (t[i] == '\\') ++i;
        if (i >= t.size()) return false;
        out += t[i++];
    }
    if (i >= t.size()) return false;
    ++i;
    return true;
}

bool expect(const std::string& t, size_t& i, const std::string& lit) {
    if (t.compare(i, lit.size(), lit) != 0) return false;
    i += lit.size();
    return true;
}

}  // namespace

std::string sha256_of_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    Sha256 h;
    uint8_t buf[1 << 16];
    while (in.read(reinterpret_cast<char*>(buf), sizeof buf) || in.gcount() > 0)
        h.update(buf, size_t(in.gcount()));
    return h.finish();
}

bool write_journal(const fs::path& dir, const Journal& j) {
    std::string t = std::string("{\"speed\":\"") + (j.speed == Speed::Fast ? "fast" : "gentle") + "\",\"done\":[";
    for (size_t k = 0; k < j.done.size(); ++k) {
        if (k) t += ",";
        t += "{\"id\":\"" + escape(j.done[k].id) + "\",\"sha256\":\"" + escape(j.done[k].sha256) + "\"}";
    }
    t += "]}";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path tmp = dir / kTempName;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << t;
        out.flush();
        if (!out) return false;
    }
    if (!sync_file(tmp)) return false;
    fs::rename(tmp, dir / kJournalName, ec);  // atomic replace on POSIX; MoveFileEx semantics on Windows
    return !ec;
}

bool read_journal(const fs::path& dir, Journal& out) {
    std::ifstream in(dir / kJournalName, std::ios::binary);
    if (!in) return false;
    std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Journal j;
    size_t i = 0;
    if (!expect(t, i, "{\"speed\":\"")) return false;
    if (expect(t, i, "fast\"")) j.speed = Speed::Fast;
    else if (expect(t, i, "gentle\"")) j.speed = Speed::Gentle;
    else return false;
    if (!expect(t, i, ",\"done\":[")) return false;
    while (i < t.size() && t[i] != ']') {
        JournalEntry e;
        if (!j.done.empty() && !expect(t, i, ",")) return false;
        if (!expect(t, i, "{\"id\":")) return false;
        if (!read_string(t, i, e.id)) return false;
        if (!expect(t, i, ",\"sha256\":")) return false;
        if (!read_string(t, i, e.sha256)) return false;
        if (!expect(t, i, "}")) return false;
        j.done.push_back(e);
    }
    if (!expect(t, i, "]}") || i != t.size()) return false;
    out = j;
    return true;
}

PrepareEngine::PrepareEngine(fs::path journal_dir, std::vector<Job> jobs, JobRunner& runner)
    : dir_(std::move(journal_dir)), jobs_(std::move(jobs)), runner_(runner) {
    if (read_journal(dir_, journal_)) speed_ = journal_.speed;
}

void PrepareEngine::set_speed(Speed s) {
    speed_ = s;
    journal_.speed = s;
    save();
}

void PrepareEngine::stop() {
    stopped_ = true;
    save();
}

void PrepareEngine::halt() {
    halted_ = true;
    for (size_t idx : running_) runner_.cancel(jobs_[idx]);
    running_.clear();
}

void PrepareEngine::save() { write_journal(dir_, journal_); }

bool PrepareEngine::is_done(const Job& j) const {
    for (const auto& e : journal_.done)
        if (e.id == j.id) return !e.sha256.empty() && sha256_of_file(j.output) == e.sha256;
    return false;
}

double PrepareEngine::fraction() const {
    double total = 0, done = 0;
    const bool async = runner_.async();
    for (size_t k = 0; k < jobs_.size(); ++k) {
        total += jobs_[k].weight;
        if (async ? (k < complete_.size() && complete_[k]) : k < next_) done += jobs_[k].weight;
    }
    return total > 0 ? done / total : 1.0;
}

Progress PrepareEngine::poll_async() {
    stopped_ = false;
    if (complete_.size() != jobs_.size()) complete_.assign(jobs_.size(), 0);
    Progress p;
    for (size_t k = 0; k < running_.size();) {
        const size_t idx = running_[k];
        const JobRunner::State st = runner_.check(jobs_[idx]);
        if (st == JobRunner::State::Running) { ++k; continue; }
        running_.erase(running_.begin() + long(k));
        if (st == JobRunner::State::Done) {
            const std::string h = sha256_of_file(jobs_[idx].output);
            bool replaced = false;
            for (auto& e : journal_.done)
                if (e.id == jobs_[idx].id) { e.sha256 = h; replaced = true; }
            if (!replaced) journal_.done.push_back({jobs_[idx].id, h});
            save();
            complete_[idx] = 1;
            ++ran_;
            ran_any_ = true;
        } else if (!failed_) {
            failed_ = true;
            failed_index_ = idx;
        }
    }
    while (!failed_ && !halted_ && running_.size() < max_parallel_) {
        while (next_ < jobs_.size() && (!(jobs_[next_].barrier && (ran_any_ || !running_.empty())) && is_done(jobs_[next_]))) {
            complete_[next_++] = 1;
            ++proven_;
        }
        if (next_ >= jobs_.size()) break;
        const Job& j = jobs_[next_];
        bool barrier_running = false;
        for (size_t r : running_) barrier_running = barrier_running || jobs_[r].barrier;
        if (barrier_running || (j.barrier && !running_.empty())) break;
        p.stage = j.stage;
        p.detail = j.id;
        if (!runner_.start(j, speed_)) { failed_ = true; failed_index_ = next_; break; }
        running_.push_back(next_++);
    }
    int complete = 0;
    for (char c : complete_) complete += c ? 1 : 0;
    p.fraction = fraction();
    p.parts_done = complete;
    p.parts_total = int(jobs_.size());
    if (failed_) { p.stage = Stage::Failed; p.detail = "job failed: " + jobs_[failed_index_].id; }
    else if (next_ >= jobs_.size() && running_.empty()) p.stage = Stage::Done;
    return p;
}

Progress PrepareEngine::poll() {
    if (runner_.async()) return poll_async();
    stopped_ = false;
    Progress p;
    // Skip what the journal proves done.
    while (next_ < jobs_.size() && is_done(jobs_[next_])) ++next_;
    if (next_ < jobs_.size() && !failed_) {
        const Job& j = jobs_[next_];
        p.stage = j.stage;
        p.detail = j.id;
        if (runner_.run(j, speed_)) {
            std::string h = sha256_of_file(j.output);
            bool replaced = false;
            for (auto& e : journal_.done)
                if (e.id == j.id) { e.sha256 = h; replaced = true; }
            if (!replaced) journal_.done.push_back({j.id, h});
            save();
            ++next_;
        } else {
            failed_ = true;
        }
    }
    p.fraction = fraction();
    p.parts_done = int(next_);
    p.parts_total = int(jobs_.size());
    if (failed_) { p.stage = Stage::Failed; p.detail = "job failed: " + jobs_[next_].id; }
    else if (next_ >= jobs_.size()) p.stage = Stage::Done;
    return p;
}

}  // namespace don_setup
