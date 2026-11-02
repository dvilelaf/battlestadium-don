// Deliberate failures for the first-run setup (task T-634): tests and the owner make the setup fail at a chosen
// point so every error screen can be exercised without a broken disc or a full disk.
//
// Grammar (docs/tasks/M6-public-release.md, A4, the end of the T-634 task: `DON_TEST_FAULT` hooks):
//
//   DON_TEST_FAULT=entry[,entry...]       entry = kind@stage[:when]   or a bare kind (noportal)
//
//   kind   enospc | eio | kill | sigkill | hashmismatch | oom | noportal
//   stage  copy | read | build | unit | verify
//   when   N%  a progress fraction: fires once the step's progress reaches N percent (0..100, decimals allowed)
//          N   the Nth unit/call of that kind and stage (N >= 1); fires on that call only
//          (none) the first call
//
// A4's examples, all accepted: enospc@copy:50%  eio@read:20%  kill@build:30%  sigkill@unit:17  hashmismatch@verify
//                              oom@unit:5       noportal
// Every kind is accepted at every stage; only the bare form is limited: only noportal may omit the stage.
// Bad entries are skipped and counted; good ones in the same list still apply. The variable is parsed once, on first use,
// thread-safely. When it is unset or holds no valid entry, fault_at is one load of a flag and a `false` return.
// Diagnostics are off by default (AGENTS.md).
#pragma once

#include <string>
#include <vector>

#include "setup_types.h"

namespace don_setup {

enum class FaultKind { Enospc, Eio, Kill, Sigkill, HashMismatch, Oom, NoPortal };
enum class FaultStage { None, Copy, Read, Build, Unit, Verify };  // None only for the bare noportal

// How a fault is armed: by progress fraction, or by call count (the default is the first call).
struct FaultSpec {
    FaultKind kind = FaultKind::Enospc;
    FaultStage stage = FaultStage::None;
    bool by_count = true;
    double fraction = 0.0;   // in [0,1], when !by_count
    unsigned count = 1;      // 1-based, when by_count
};

// Parses a DON_TEST_FAULT value. Invalid entries are skipped and counted in *rejected when given.
std::vector<FaultSpec> parse_fault_spec(const std::string& text, unsigned* rejected = nullptr);

// True when a fault armed for (kind, stage) fires now. `progress` is the fraction done in [0,1] of the step that
// calls the hook; a count-armed fault counts the calls of that (kind, stage) instead.
bool fault_at(FaultKind kind, FaultStage stage, double progress);

// The catalogue error each kind stands for (A4 line cited in faults.cpp); None for kill (it tests resume).
ErrorId fault_error(FaultKind kind);

const char* fault_kind_name(FaultKind kind);
const char* fault_stage_name(FaultStage stage);

// Test only: forgets the parsed variable and the call counts, so the next use parses again.
void faults_reset_for_tests();
// Test only: how many times the variable was parsed since the process started.
unsigned faults_parse_count_for_tests();

}  // namespace don_setup
