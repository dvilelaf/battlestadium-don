// Tests for the glue between the setup screens' model and the preparation (T-649, src/host/setup_run.*): a fake backend
// plays the part of the real one (discovery, the background job); no SDL, no disc, no process. The .cpp files are
// included so one command builds them.
#define DON_SETUP_RUN_NO_SDL 1
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"
#include "../../src/host/setup_run.cpp"
#include "../../src/host/setup_script.cpp"

#include <cstdio>

using namespace don_setup;
using namespace don_host;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)

namespace {

class FakeBackend : public SetupBackend {
public:
    bool discovery_done = false;
    std::vector<Candidate> candidates;
    bool has_resume = false;
    std::string resume_image;
    int resume_percent = 0;
    std::vector<std::string> calls;   // "identify:<path>", "prepare:<path>", "stop", "discovery"
    AsyncSnapshot snap;

    Environment environment() override { Environment e; e.free_bytes = 100ull << 30; return e; }
    void begin_discovery() override { calls.push_back("discovery"); }
    bool discovery_ready(std::vector<Candidate>& found) override {
        if (!discovery_done) return false;
        discovery_done = false;
        found = candidates;
        return true;
    }
    bool resumable(std::string& image, int& percent) override {
        image = resume_image;
        percent = resume_percent;
        return has_resume;
    }
    bool offer_done = false;   // the identification of a found image has ended; its result is `offer_id`
    DiscIdentity offer_id;
    void begin_offer_check(const std::string& image) override { calls.push_back("offer:" + image); offer_done = false; }
    bool offer_check_poll(DiscIdentity& id, double& fraction) override {
        fraction = 0.0;
        if (!offer_done) return false;
        id = offer_id;
        return true;
    }
    void identify(const std::string& image) override { calls.push_back("identify:" + image); snap = AsyncSnapshot{}; snap.running = true; }
    void prepare(const std::string& image) override { calls.push_back("prepare:" + image); snap = AsyncSnapshot{}; snap.running = true; snap.was_prepare = true; }
    void stop() override { calls.push_back("stop"); }
    AsyncSnapshot poll() override { return snap; }
};

struct Rig {
    SetupModel model;
    FakeBackend backend;
    std::vector<Action> ui_actions;
    SetupController controller{model, backend, [this](const ActionRequest& a) { ui_actions.push_back(a.action); return true; }};
};

DiscIdentity good_disc() {
    DiscIdentity id;
    id.known = true;
    id.variant_id = "jp";
    id.description = "Battle Stadium D.O.N (Japan)";
    return id;
}

void finish_identify(Rig& r) {
    r.backend.snap.running = false;
    r.backend.snap.has_result = true;
    r.backend.snap.identity = good_disc();
}

void test_found_disc_to_ready_to_play() {
    Rig r;
    r.controller.start();
    CHECK(r.backend.calls.size() == 1 && r.backend.calls[0] == "discovery");
    CHECK(r.controller.step() == SetupEnd::Running);
    CHECK(r.controller.busy());                     // still searching
    r.backend.candidates = {{"/home/p/Downloads/don.iso", ""}};
    r.backend.discovery_done = true;
    r.controller.step();
    CHECK(r.controller.busy());                     // the found image is being identified
    r.backend.offer_id = good_disc();
    r.backend.offer_done = true;
    r.controller.step();
    CHECK(!r.controller.busy());
    CHECK(r.model.screen() == Screen::Welcome);

    r.model.navigate(Nav::Confirm);                 // USE THIS DISC IMAGE
    r.controller.step();
    CHECK(r.model.screen() == Screen::Checking);
    CHECK(r.backend.calls.back() == "identify:/home/p/Downloads/don.iso");
    CHECK(r.controller.busy());

    r.backend.snap.progress.fraction = 0.5;         // the check is under way
    r.controller.step();
    finish_identify(r);                             // a good disc goes straight on to preparing (A2 rule 6)
    r.controller.step();
    CHECK(r.model.screen() == Screen::Preparing);
    CHECK(r.backend.calls.back() == "prepare:/home/p/Downloads/don.iso");

    r.backend.snap.progress.stage = Stage::Build;   // the job reports; the model shows it
    r.backend.snap.progress.fraction = 0.6;
    r.backend.snap.progress.detail = "building the game module";
    r.backend.snap.progress.seconds_left = 120.0;
    r.controller.step();
    CHECK(r.model.view().percent == 60);

    r.backend.snap.running = false;                 // the job ends well
    r.backend.snap.has_result = true;
    r.backend.snap.outcome.ok = true;
    r.controller.step();
    CHECK(r.model.screen() == Screen::Ready);
    CHECK(!r.controller.busy());

    CHECK(r.controller.step() == SetupEnd::Running);
    r.model.navigate(Nav::Confirm);                 // PLAY
    CHECK(r.controller.step() == SetupEnd::Play);
}

void test_failure_shows_the_error_panel() {
    Rig r;
    r.controller.start();
    r.backend.discovery_done = true;                // the search found nothing and ended
    r.controller.step();
    r.model.file_chosen("/x/don.iso");
    r.controller.step();
    finish_identify(r);
    r.controller.step();
    CHECK(r.model.screen() == Screen::Preparing);
    r.backend.snap.running = false;
    r.backend.snap.has_result = true;
    r.backend.snap.outcome.error = ErrorId::E53;
    r.backend.snap.outcome.detail = "unit entry failed";
    r.controller.step();
    CHECK(r.model.has_error());
    CHECK(r.model.view().error_overlay);
    CHECK(r.model.view().error.code == error_info(ErrorId::E53).code);
    CHECK(!r.controller.busy());                    // the player has a button to press
}

void test_unreadable_disc_is_an_error_not_a_start() {
    Rig r;
    r.controller.start();
    r.model.file_chosen("/x/other.iso");
    r.controller.step();
    r.backend.snap.running = false;
    r.backend.snap.has_result = true;
    r.backend.snap.identity.known = false;
    r.backend.snap.identity.error = ErrorId::E15;
    r.controller.step();
    CHECK(r.model.has_error());
    CHECK(r.backend.calls.back() == "identify:/x/other.iso");   // nothing was prepared
}

void test_stop_and_resume_next_start() {
    Rig r;
    r.controller.start();
    r.model.file_chosen("/x/don.iso");
    r.controller.step();
    finish_identify(r);
    r.controller.step();
    r.model.request_close();                        // the close button while preparing asks first
    CHECK(r.model.mode() == PrepMode::CloseConfirm);
    r.model.navigate(Nav::Down);                    // STOP AND CLOSE
    r.model.navigate(Nav::Confirm);
    CHECK(r.controller.step() == SetupEnd::Quit);
    bool stopped = false;
    for (const auto& c : r.backend.calls) stopped = stopped || c == "stop";
    CHECK(stopped);

    Rig next;                                       // the next start finds the journal
    next.backend.has_resume = true;
    next.backend.resume_image = "/x/don.iso";
    next.backend.resume_percent = 37;
    next.controller.start();
    CHECK(next.model.screen() == Screen::Resume);
    CHECK(next.backend.calls.empty());              // no search while a run waits
    next.model.navigate(Nav::Confirm);              // CONTINUE PREPARING
    next.controller.step();
    CHECK(next.model.screen() == Screen::Preparing);
    CHECK(next.backend.calls.back() == "prepare:/x/don.iso");
}

void test_browser_actions_go_to_the_screen() {
    Rig r;
    r.controller.start();
    for (const MenuItem& item : r.model.view().menu) CHECK(item.label != "BROWSE");
    r.model.file_picker_unavailable();  // the system dialog failed: the in-app browser is the fallback
    r.controller.step();
    CHECK(r.ui_actions.size() == 1 && r.ui_actions[0] == Action::OpenBrowser);
}

void test_script_parser() {
    const auto steps = parse_script("confirm,now:back,drop:/tmp/a b/disc.iso,now:drop:/x/y.iso,bogus,now:,drop:");
    CHECK(steps.size() == 4);   // the unknown key, the empty key and the drop without a path are skipped
    CHECK(steps[0].kind == ScriptStep::Kind::Key && steps[0].nav == Nav::Confirm && !steps[0].now);
    CHECK(steps[1].kind == ScriptStep::Kind::Key && steps[1].nav == Nav::Back && steps[1].now);
    CHECK(steps[2].kind == ScriptStep::Kind::Drop && steps[2].path == "/tmp/a b/disc.iso" && !steps[2].now);
    CHECK(steps[3].kind == ScriptStep::Kind::Drop && steps[3].path == "/x/y.iso" && steps[3].now);
    CHECK(parse_script(nullptr).empty() && parse_script("").empty());
}

}  // namespace

int main() {
    test_found_disc_to_ready_to_play();
    test_failure_shows_the_error_panel();
    test_unreadable_disc_is_an_error_not_a_start();
    test_stop_and_resume_next_start();
    test_browser_actions_go_to_the_screen();
    test_script_parser();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
