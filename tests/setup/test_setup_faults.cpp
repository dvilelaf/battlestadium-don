// Tests of the DON_TEST_FAULT hooks (T-634): A4's grammar, each example's trigger, bad entries, the error map and
// the unset case.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "errors.h"
#include "faults.h"

using namespace don_setup;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void set_env(const char* v) {
    if (v) setenv("DON_TEST_FAULT", v, 1); else unsetenv("DON_TEST_FAULT");
    faults_reset_for_tests();
}

static void test_a4_examples_parse() {
    unsigned bad = 0;
    auto v = parse_fault_spec("enospc@copy:50%,eio@read:20%,kill@build:30%,sigkill@unit:17,hashmismatch@verify,"
                              "oom@unit:5,noportal", &bad);
    CHECK(v.size() == 7 && bad == 0);
    CHECK(v[0].kind == FaultKind::Enospc && v[0].stage == FaultStage::Copy && !v[0].by_count && v[0].fraction == 0.5);
    CHECK(v[1].kind == FaultKind::Eio && v[1].stage == FaultStage::Read && !v[1].by_count && v[1].fraction == 0.2);
    CHECK(v[2].kind == FaultKind::Kill && v[2].stage == FaultStage::Build && !v[2].by_count && v[2].fraction == 0.3);
    CHECK(v[3].kind == FaultKind::Sigkill && v[3].stage == FaultStage::Unit && v[3].by_count && v[3].count == 17);
    CHECK(v[4].kind == FaultKind::HashMismatch && v[4].stage == FaultStage::Verify && v[4].by_count && v[4].count == 1);
    CHECK(v[5].kind == FaultKind::Oom && v[5].stage == FaultStage::Unit && v[5].by_count && v[5].count == 5);
    CHECK(v[6].kind == FaultKind::NoPortal && v[6].stage == FaultStage::None && v[6].count == 1);
}

static void test_a4_examples_trigger() {
    set_env("enospc@copy:50%");
    CHECK(!fault_at(FaultKind::Enospc, FaultStage::Copy, 0.49));
    CHECK(fault_at(FaultKind::Enospc, FaultStage::Copy, 0.5));
    CHECK(!fault_at(FaultKind::Enospc, FaultStage::Read, 1.0));  // another stage never fires
    CHECK(!fault_at(FaultKind::Eio, FaultStage::Copy, 1.0));     // nor another kind

    set_env("eio@read:20%");
    CHECK(!fault_at(FaultKind::Eio, FaultStage::Read, 0.19));
    CHECK(fault_at(FaultKind::Eio, FaultStage::Read, 0.2));

    set_env("kill@build:30%");
    CHECK(!fault_at(FaultKind::Kill, FaultStage::Build, 0.29));
    CHECK(fault_at(FaultKind::Kill, FaultStage::Build, 0.31));

    set_env("sigkill@unit:17");
    for (int i = 1; i < 17; ++i) CHECK(!fault_at(FaultKind::Sigkill, FaultStage::Unit, 1.0));
    CHECK(fault_at(FaultKind::Sigkill, FaultStage::Unit, 0.0));  // the 17th call, whatever the progress
    CHECK(!fault_at(FaultKind::Sigkill, FaultStage::Unit, 1.0)); // once

    set_env("oom@unit:5");
    for (int i = 1; i < 5; ++i) CHECK(!fault_at(FaultKind::Oom, FaultStage::Unit, 1.0));
    CHECK(fault_at(FaultKind::Oom, FaultStage::Unit, 0.0));

    set_env("hashmismatch@verify");  // no when: the first call
    CHECK(fault_at(FaultKind::HashMismatch, FaultStage::Verify, 0.0));
    CHECK(!fault_at(FaultKind::HashMismatch, FaultStage::Verify, 0.0));

    set_env("noportal");
    CHECK(fault_at(FaultKind::NoPortal, FaultStage::None, 0.0));
}

static void test_bad_entries() {
    unsigned bad = 0;
    for (const char* s : {"", ",", "nope@copy", "enospc", "eio", "enospc@", "enospc@nowhere", "enospc@copy:",
                          "enospc@copy:0", "enospc@copy:x", "enospc@copy:101%", "enospc@copy:-1%", "enospc@copy:%",
                          "enospc@copy:5%x", "@copy", "noportal@copy", "enospc@copy:1.5", "enospc@copy:99999999"}) {
        auto v = parse_fault_spec(s, &bad);
        CHECK(v.empty());
    }
    // Every kind is accepted at every stage (A4 grammar): the restriction is not in the parser.
    auto any = parse_fault_spec("sigkill@copy,hashmismatch@unit,sigkill@build:3,hashmismatch@read:50%", &bad);
    CHECK(any.size() == 4 && any[0].kind == FaultKind::Sigkill && any[0].stage == FaultStage::Copy);
    CHECK(any[1].kind == FaultKind::HashMismatch && any[1].stage == FaultStage::Unit);
    CHECK(any[2].kind == FaultKind::Sigkill && any[2].stage == FaultStage::Build && any[2].count == 3);
    auto v = parse_fault_spec("bogus@1,oom@unit:2,,kill@read,eio", &bad);  // good ones survive
    CHECK(v.size() == 2 && bad == 2);
    CHECK(v[0].kind == FaultKind::Oom && v[1].kind == FaultKind::Kill && v[1].stage == FaultStage::Read);
    set_env("garbage,enospc@copy:9x");
    CHECK(!fault_at(FaultKind::Enospc, FaultStage::Copy, 1.0));
}

static void test_errors() {
    // A4 lines: E31 :199, E19 :189, E54 :220, E52 :218, E38 :206, E02 (no system file dialog).
    CHECK(fault_error(FaultKind::Enospc) == ErrorId::E31);
    CHECK(fault_error(FaultKind::Eio) == ErrorId::E19);
    CHECK(fault_error(FaultKind::Kill) == ErrorId::None);
    CHECK(fault_error(FaultKind::Sigkill) == ErrorId::E54);
    CHECK(fault_error(FaultKind::HashMismatch) == ErrorId::E52);
    CHECK(fault_error(FaultKind::Oom) == ErrorId::E38);
    CHECK(fault_error(FaultKind::NoPortal) == ErrorId::E02);
    CHECK(std::string(fault_kind_name(FaultKind::HashMismatch)) == "hashmismatch");
    CHECK(std::string(fault_stage_name(FaultStage::Verify)) == "verify");
    // The title of E31 says the pause is automatic; the body keeps the plan's sentence (M6 :199).
    const ErrorInfo& e31 = error_info(ErrorId::E31);
    CHECK(std::string(e31.title) == "Preparing paused");
    CHECK(std::string(e31.why).rfind("Your disk is full.", 0) == 0);
}

static void test_unset_costs_one_flag_check() {
    set_env(nullptr);
    const unsigned before = faults_parse_count_for_tests();
    for (int i = 0; i < 1000; ++i) {
        CHECK(!fault_at(FaultKind::Enospc, FaultStage::Copy, 1.0));
        CHECK(!fault_at(FaultKind::NoPortal, FaultStage::None, 1.0));
    }
    CHECK(faults_parse_count_for_tests() == before);  // the parser never ran with the variable unset
    set_env("enospc@copy");
    CHECK(fault_at(FaultKind::Enospc, FaultStage::Copy, 0.0));
    CHECK(faults_parse_count_for_tests() == before + 1);  // set: parsed exactly once, however many calls
    fault_at(FaultKind::Enospc, FaultStage::Copy, 0.0);
    CHECK(faults_parse_count_for_tests() == before + 1);
}

// Entries of the any-stage kinds must fire, not only parse: a count entry on a stage other than unit, and a
// percentage entry on a stage other than copy.
static void test_any_stage_entries_fire() {
    set_env("sigkill@build:3");
    CHECK(!fault_at(FaultKind::Sigkill, FaultStage::Build, 1.0));  // call 1
    CHECK(!fault_at(FaultKind::Sigkill, FaultStage::Build, 1.0));  // call 2
    CHECK(fault_at(FaultKind::Sigkill, FaultStage::Build, 1.0));   // call 3
    CHECK(!fault_at(FaultKind::Sigkill, FaultStage::Build, 1.0));  // once

    set_env("hashmismatch@read:50%");
    CHECK(!fault_at(FaultKind::HashMismatch, FaultStage::Read, 0.4));
    CHECK(fault_at(FaultKind::HashMismatch, FaultStage::Read, 0.5));
}

int main() {
    test_a4_examples_parse();
    test_a4_examples_trigger();
    test_any_stage_entries_fire();
    test_bad_entries();
    test_errors();
    test_unset_costs_one_flag_check();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("faults: all passed\n");
    return 0;
}
