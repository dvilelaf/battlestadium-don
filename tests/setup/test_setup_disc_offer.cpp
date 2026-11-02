// Tests for the disc offer of the first-run screen S1 (T-702): a found image is offered only after it is identified
// (the identification `don --prepare` runs), the offer names the edition, and compressed or unidentified files are
// never offered. A fake backend plays the part of the real one; no SDL, no disc. The .cpp files are included so one
// command builds them.
#define DON_SETUP_RUN_NO_SDL 1
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"
#include "../../src/host/setup_run.cpp"

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
    std::vector<Candidate> candidates;   // raw images the search found (no edition yet)
    std::vector<std::string> calls;      // "discovery", "offer:<path>", "identify:<path>"
    bool offer_done = false;             // the running offer check has ended
    double offer_fraction = 0.0;
    DiscIdentity offer_id;
    AsyncSnapshot snap;

    Environment environment() override { Environment e; e.free_bytes = 100ull << 30; return e; }
    void begin_discovery() override { calls.push_back("discovery"); }
    bool discovery_ready(std::vector<Candidate>& found) override {
        if (!discovery_done) return false;
        discovery_done = false;
        found = candidates;
        return true;
    }
    bool resumable(std::string&, int&) override { return false; }
    void begin_offer_check(const std::string& image) override { calls.push_back("offer:" + image); offer_done = false; }
    bool offer_check_poll(DiscIdentity& id, double& fraction) override {
        fraction = offer_fraction;
        if (!offer_done) return false;
        id = offer_id;
        return true;
    }
    void identify(const std::string& image) override { calls.push_back("identify:" + image); snap = AsyncSnapshot{}; snap.running = true; }
    void prepare(const std::string&) override {}
    void stop() override {}
    AsyncSnapshot poll() override { return snap; }

    void finish_offer(bool known, const char* edition, ErrorId error = ErrorId::None) {
        offer_id = DiscIdentity{};
        offer_id.known = known;
        offer_id.description = edition;
        offer_id.error = known ? error : ErrorId::E15;
        offer_done = true;
    }
};

struct Rig {
    SetupModel model;
    FakeBackend backend;
    SetupController controller{model, backend, [](const ActionRequest&) { return true; }};
    void search_finds(std::vector<Candidate> c) {
        controller.start();
        backend.candidates = std::move(c);
        backend.discovery_done = true;
        controller.step();
    }
};

static bool has_body(const View& v, const std::string& text) {
    for (const auto& b : v.body)
        if (b == text) return true;
    return false;
}

static bool offers_a_disc(const SetupModel& m) {
    for (const auto& item : m.view().menu)
        if (item.label.rfind("USE THIS DISC", 0) == 0) return true;
    return false;
}

void test_offer_waits_for_the_identification() {
    Rig r;
    r.search_finds({{"/x/a.iso", ""}, {"/x/b.iso", ""}});
    CHECK(r.backend.calls.back() == "offer:/x/a.iso");       // the first file is being checked
    View v = r.model.view();
    CHECK(has_body(v, "Checking a.iso…"));
    CHECK(v.bar >= 0.0);                                      // with progress
    CHECK(!offers_a_disc(r.model));                           // nothing is offered before it is identified
    CHECK(r.controller.busy());

    r.backend.offer_fraction = 0.5;
    r.controller.step();
    CHECK(r.model.view().bar > 0.0);

    r.backend.finish_offer(false, "");                        // a.iso is not the game: never offered, the next is checked
    r.controller.step();
    CHECK(r.backend.calls.back() == "offer:/x/b.iso");
    CHECK(has_body(r.model.view(), "Checking b.iso…"));
    CHECK(!offers_a_disc(r.model));

    r.backend.finish_offer(true, "Japanese (original)");      // b.iso is
    r.controller.step();
    v = r.model.view();
    CHECK(offers_a_disc(r.model));
    CHECK(v.menu[0].label == "USE THIS DISC" && v.menu[0].primary);
    CHECK(has_body(v, "Battle Stadium D.O.N · Japanese (original) · b.iso"));
    CHECK(!r.controller.busy());

    r.model.navigate(Nav::Confirm);                           // using it identifies it again for the prepare
    r.controller.step();
    CHECK(r.backend.calls.back() == "identify:/x/b.iso");
    CHECK(r.model.screen() == Screen::Checking);
}

void test_nothing_identified_offers_only_choose() {
    Rig r;
    r.search_finds({{"/x/a.iso", ""}});
    r.backend.finish_offer(false, "");
    r.controller.step();
    const View v = r.model.view();
    CHECK(!offers_a_disc(r.model));
    CHECK(v.menu[0].label == "CHOOSE DISC IMAGE…" && v.menu[0].primary);
    CHECK(v.body.size() == 1);                                // the one sentence; no "checking", no offer
    CHECK(!r.controller.busy());
}

void test_nothing_found_offers_only_choose() {
    Rig r;
    r.search_finds({});
    CHECK(!offers_a_disc(r.model));
    CHECK(r.model.view().menu[0].label == "CHOOSE DISC IMAGE…");
    for (const std::string& c : r.backend.calls) CHECK(c.rfind("offer:", 0) != 0);
}

void test_modified_image_is_not_offered() {
    Rig r;
    r.search_finds({{"/x/a.iso", ""}});
    r.backend.finish_offer(true, "Japanese (original)", ErrorId::E17);   // known game, data we do not recognise
    r.controller.step();
    CHECK(!offers_a_disc(r.model));
}

void test_choosing_a_file_ends_the_offer_checks() {
    Rig r;
    r.search_finds({{"/x/a.iso", ""}, {"/x/b.iso", ""}});
    r.model.file_chosen("/y/mine.iso");                       // the player picks one while the search checks
    r.controller.step();
    CHECK(r.backend.calls.back() == "identify:/y/mine.iso");
    r.backend.finish_offer(true, "Japanese (original)");      // a late answer changes nothing
    r.controller.step();
    CHECK(r.model.screen() == Screen::Checking);
    CHECK(!offers_a_disc(r.model));
    for (const std::string& c : r.backend.calls) CHECK(c != "offer:/x/b.iso");
}

void test_view_has_no_offer_text_without_one() {
    SetupModel m;
    m.set_discovery({});
    for (const std::string& b : m.view().body) CHECK(b.find("Battle Stadium D.O.N ·") == std::string::npos);
}

// The error card is the whole screen: the view carries none of the text of the screen behind it, so no line can run
// under or over the card (the owner saw text drawn over text, 2026-10-26).
void test_error_card_has_no_text_behind() {
    SetupModel m;
    m.file_chosen("/x/a.iso");
    m.set_error_id(ErrorId::E08, {{"have", "3.1"}, {"total", "4.2"}});
    const View v = m.view();
    CHECK(v.error_overlay);
    CHECK(v.title.empty() && v.body.empty() && v.steps.empty() && v.panel.empty() && v.notes.empty());
    CHECK(v.hint.empty() && v.status.empty() && v.link.empty() && v.banner.empty() && v.time_text.empty());
    CHECK(v.bar < 0.0);
    CHECK(!v.error.title.empty() && !v.error.why.empty() && v.menu.size() == 2);
}

}  // namespace

int main() {
    test_offer_waits_for_the_identification();
    test_nothing_identified_offers_only_choose();
    test_nothing_found_offers_only_choose();
    test_modified_image_is_not_offered();
    test_choosing_a_file_ends_the_offer_checks();
    test_view_has_no_offer_text_without_one();
    test_error_card_has_no_text_behind();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
