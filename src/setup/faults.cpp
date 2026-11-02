#include "faults.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace don_setup {

namespace {

constexpr int kKindCount = 7;
constexpr int kStageCount = 6;
constexpr unsigned long kMaxCount = 1000000;

struct Armed {
    FaultSpec spec;
    unsigned calls = 0;
};

struct State {
    std::vector<Armed> armed;
};

enum : int { kUnloaded = 0, kOff = 1, kOn = 2 };
std::atomic<int> g_mode{kUnloaded};  // the one flag the fast path loads: kOff when no valid entry exists
std::atomic<unsigned> g_parses{0};
std::mutex g_mutex;  // guards g_state after parsing
State g_state;

// Error per kind. Lines are of docs/tasks/M6-public-release.md (A4 catalogue, T-635 for the verify step).
const struct { FaultKind kind; const char* name; ErrorId error; } kKinds[kKindCount] = {
    {FaultKind::Enospc, "enospc", ErrorId::E31},              // :199 E31 Disk full part-way
    {FaultKind::Eio, "eio", ErrorId::E19},                    // :189 E19 Read error part-way
    {FaultKind::Kill, "kill", ErrorId::None},                 // our own process killed: no screen, it tests resume
    {FaultKind::Sigkill, "sigkill", ErrorId::E54},            // :220 E54 Compiler crash (sigkill@unit)
    {FaultKind::HashMismatch, "hashmismatch", ErrorId::E52},  // :218 E52 Generated tree hash differs (not E57)
    {FaultKind::Oom, "oom", ErrorId::E38},                    // :206 E38 A compiler is OOM-killed (oom@unit)
    {FaultKind::NoPortal, "noportal", ErrorId::E02},          // :171 E02 No system file dialog
};

const char* const kStages[kStageCount] = {"", "copy", "read", "build", "unit", "verify"};

int kind_index(FaultKind k) { return static_cast<int>(k); }

bool parse_kind(const std::string& name, FaultKind* out) {
    for (const auto& e : kKinds)
        if (name == e.name) { *out = e.kind; return true; }
    return false;
}

bool parse_stage(const std::string& name, FaultStage* out) {
    for (int i = 1; i < kStageCount; ++i)
        if (name == kStages[i]) { *out = static_cast<FaultStage>(i); return true; }
    return false;
}

// "N%" -> fraction, "N" -> count (N >= 1), "" -> the first call.
bool parse_when(const std::string& when, FaultSpec* spec) {
    spec->by_count = true;
    spec->count = 1;
    if (when.empty()) return true;
    if (when.back() == '%') {
        const std::string num = when.substr(0, when.size() - 1);
        char* end = nullptr;
        const double pct = std::strtod(num.c_str(), &end);
        if (num.empty() || end == num.c_str() || *end != '\0' || !(pct >= 0.0 && pct <= 100.0)) return false;
        spec->by_count = false;
        spec->fraction = pct / 100.0;
        return true;
    }
    unsigned long n = 0;
    for (char c : when) {
        if (c < '0' || c > '9' || n > kMaxCount) return false;
        n = n * 10 + static_cast<unsigned long>(c - '0');
    }
    if (n == 0 || n > kMaxCount) return false;
    spec->count = static_cast<unsigned>(n);
    return true;
}

bool parse_entry(const std::string& entry, FaultSpec* out) {
    FaultSpec spec;
    const std::size_t at = entry.find('@');
    if (at == std::string::npos) {  // bare kind: only noportal
        if (!parse_kind(entry, &spec.kind) || spec.kind != FaultKind::NoPortal) return false;
        *out = spec;
        return true;
    }
    if (!parse_kind(entry.substr(0, at), &spec.kind) || spec.kind == FaultKind::NoPortal) return false;
    const std::size_t colon = entry.find(':', at);
    const std::string stage = entry.substr(at + 1, colon == std::string::npos ? std::string::npos : colon - at - 1);
    if (!parse_stage(stage, &spec.stage)) return false;
    // "kind@stage:" (empty after the colon) is malformed; no colon at all means the first call.
    if (colon != std::string::npos && colon + 1 == entry.size()) return false;
    if (!parse_when(colon == std::string::npos ? std::string() : entry.substr(colon + 1), &spec)) return false;
    *out = spec;
    return true;
}

void load() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_mode.load(std::memory_order_relaxed) != kUnloaded) return;  // another thread parsed it first
    const char* v = std::getenv("DON_TEST_FAULT");
    g_state.armed.clear();
    if (v) {
        g_parses.fetch_add(1, std::memory_order_relaxed);
        for (const FaultSpec& s : parse_fault_spec(v)) g_state.armed.push_back(Armed{s, 0});
    }
    g_mode.store(g_state.armed.empty() ? kOff : kOn, std::memory_order_release);
}

}  // namespace

std::vector<FaultSpec> parse_fault_spec(const std::string& text, unsigned* rejected) {
    std::vector<FaultSpec> out;
    unsigned bad = 0;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t j = text.find(',', i);
        if (j == std::string::npos) j = text.size();
        const std::string entry = text.substr(i, j - i);
        if (!entry.empty()) {
            FaultSpec spec;
            if (parse_entry(entry, &spec)) out.push_back(spec);
            else ++bad;
        }
        i = j + 1;
    }
    if (rejected) *rejected = bad;
    return out;
}

bool fault_at(FaultKind kind, FaultStage stage, double progress) {
    int mode = g_mode.load(std::memory_order_acquire);
    if (__builtin_expect(mode == kUnloaded, 0)) {  // first use: parse once
        load();
        mode = g_mode.load(std::memory_order_acquire);
    }
    if (__builtin_expect(mode != kOn, 1)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    bool fire = false;
    for (Armed& a : g_state.armed) {
        if (a.spec.kind != kind || a.spec.stage != stage) continue;
        ++a.calls;
        if (a.spec.by_count ? a.calls == a.spec.count : progress >= a.spec.fraction) fire = true;
    }
    return fire;
}

ErrorId fault_error(FaultKind kind) { return kKinds[kind_index(kind)].error; }

const char* fault_kind_name(FaultKind kind) { return kKinds[kind_index(kind)].name; }

const char* fault_stage_name(FaultStage stage) { return kStages[static_cast<int>(stage)]; }

void faults_reset_for_tests() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state = State{};
    g_mode.store(kUnloaded, std::memory_order_release);
}

unsigned faults_parse_count_for_tests() { return g_parses.load(std::memory_order_relaxed); }

}  // namespace don_setup
