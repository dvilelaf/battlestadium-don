// Tests for the setup screen model's states and transitions (A3 in docs/tasks/M6-public-release.md; no SDL).
// The .cpp files are included so one command builds them. The A2 rules are in test_setup_flow_model.cpp.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"

#include "../../src/setup/error_values.h"

#include <cstdio>
#include <cstdlib>

using namespace don_setup;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_fail;                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        std::string _a = (a), _b = (b);                                                             \
        if (_a != _b) {                                                                             \
            ++g_fail;                                                                               \
            std::printf("FAIL %s:%d  [%s] != [%s]\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); \
        }                                                                                           \
    } while (0)

static Environment good_env() {
    Environment e;
    e.free_bytes = 50ull * 1000000000ull;
    e.needed_bytes = 3200000000ull;
    e.drive = "/home";
    e.cores = 8;
    e.ram_bytes = 16ull << 30;
    e.estimate_seconds = 170;
    return e;
}

static bool has(const std::vector<ActionRequest>& a, Action x) {
    for (auto& r : a)
        if (r.action == x) return true;
    return false;
}

static std::string label(const SetupModel& m, size_t i) { return i < m.view().menu.size() ? m.view().menu[i].label : ""; }

static DiscIdentity known_disc() {
    DiscIdentity id;
    id.known = true;
    id.variant_id = "en";
    id.description = "English fan translation";
    return id;
}

static Progress prog(PrepStage s, double f, double left, int done = 0, int total = 0) {
    Progress p;
    p.stage = s;
    p.fraction = f;
    p.seconds_left = left;
    p.parts_done = done;
    p.parts_total = total;
    return p;
}

// A model with the environment set, a file chosen and the check answered with `id`.
static void to_checked(SetupModel& m, const DiscIdentity& id, const Environment& e = good_env()) {
    m.set_environment(e);
    m.file_chosen("/tmp/don.iso");
    m.set_disc_identity(id);
}
static void to_preparing(SetupModel& m) {
    to_checked(m, known_disc());
    m.take_actions();
}

static void test_time_strings() {
    CHECK_EQ(time_about(30), "less than a minute");
    CHECK_EQ(time_about(61), "about 2 min");
    CHECK_EQ(time_about(170), "about 3 min");
    CHECK_EQ(time_range(180, 0.1), "about 3 min");
    CHECK_EQ(time_range(180, 0.3), "about 3-4 min");
    CHECK_EQ(with_thousands(1876), "1,876");
    CHECK_EQ(format_gb_ceil(3200000001ull), "3.3");
    CHECK_EQ(format_gb_floor(1190000000ull), "1.1");
}

static void test_s0_results_feed_s1() {
    SetupModel m;
    m.set_environment(good_env());
    View v = m.view();
    CHECK(v.screen == Screen::Welcome);
    CHECK_EQ(label(m, 0), "CHOOSE DISC IMAGE…");
    CHECK_EQ(label(m, 1), "QUIT");
    CHECK_EQ(v.title, "Set up Battle Stadium D.O.N");
    CHECK_EQ(v.steps[0].text + "|" + v.steps[1].text + "|" + v.steps[2].text, "1 Your disc|2 Prepare|3 Play");
    CHECK(v.body.size() == 1 && v.body[0] == "Choose the disc image of your copy of the game.");
    CHECK(v.panel.empty() && v.notes.empty() && v.hint.empty() && v.link.empty() && v.status.empty());
    // S0 found an identified image: it becomes the primary action, and a line above names it
    m.set_discovery({{"/home/p/Downloads/my disc.iso", "Japanese (original)"}});
    v = m.view();
    CHECK_EQ(v.menu[0].label, "USE THIS DISC");
    CHECK(v.selected == 0 && v.menu[0].primary);
    CHECK_EQ(v.body.back(), "Battle Stadium D.O.N · Japanese (original) · my disc.iso");
    m.navigate(Nav::Confirm);
    auto a = m.take_actions();
    CHECK(a.size() == 1 && a[0].action == Action::IdentifyDisc && a[0].path == "/home/p/Downloads/my disc.iso");
    // an image without an edition (not identified) is never offered
    SetupModel p;
    p.set_discovery({{"/x/don.zip", ""}});
    CHECK_EQ(label(p, 0), "CHOOSE DISC IMAGE…");
    // S0 found nothing: still S1 with CHOOSE
    SetupModel n;
    n.set_discovery({});
    CHECK_EQ(label(n, 0), "CHOOSE DISC IMAGE…");
    CHECK(n.view().notes.empty());
}

static void test_s1_actions() {
    SetupModel m;
    m.navigate(Nav::Confirm);
    CHECK(has(m.take_actions(), Action::OpenFilePicker));
    m.file_picker_unavailable();  // E02
    CHECK(has(m.take_actions(), Action::OpenBrowser));
    CHECK(label(m, 1) == "QUIT");  // one entry to choose the disc; the browser is only the fallback; no disc drive item without a disc
    Environment e = good_env();
    e.disc_present = true;
    m.set_environment(e);
    CHECK_EQ(label(m, 1), "READ FROM DISC DRIVE");
    m.activate(1);
    CHECK(has(m.take_actions(), Action::ReadDiscDrive));
    m.navigate(Nav::Back);
    CHECK(has(m.take_actions(), Action::Quit));
    m.file_chosen("");  // picker cancelled (E01): stays
    CHECK(m.screen() == Screen::Welcome && m.take_actions().empty());
}

static void test_s2_checking() {
    SetupModel m;
    m.set_environment(good_env());
    m.file_chosen("/tmp/don.iso");
    auto a = m.take_actions();
    CHECK(a.size() == 1 && a[0].action == Action::IdentifyDisc && a[0].path == "/tmp/don.iso");
    View v = m.view();
    CHECK(v.screen == Screen::Checking);
    CHECK_EQ(v.title, "Checking your disc…");
    CHECK_EQ(v.body[0], "don.iso");
    CHECK_EQ(v.time_text, "a few seconds");
    CHECK(v.bar >= 0);
    m.file_chosen("/tmp/other.iso");  // a second drop while checking is ignored
    CHECK(m.take_actions().empty());
    m.navigate(Nav::Back);
    CHECK(has(m.take_actions(), Action::CancelIdentify));
    CHECK(m.screen() == Screen::Welcome);
    DiscIdentity id = known_disc();
    m.set_disc_identity(id);  // a result outside Checking is ignored
    CHECK(m.screen() == Screen::Welcome);
}

static void test_good_check_does_not_stop() {
    SetupModel m;
    to_checked(m, known_disc());
    CHECK(m.screen() == Screen::Preparing);
    auto a = m.take_actions();
    CHECK(has(a, Action::IdentifyDisc) && has(a, Action::StartPrepare));
    CHECK(a.back().action == Action::StartPrepare && a.back().path == "en");
}

static void test_decision_panel() {
    // E17: unknown data
    SetupModel m;
    DiscIdentity id;
    id.error = ErrorId::E17;
    id.variant_id = "custom-1a2b3c4d";
    to_checked(m, id);
    View v = m.view();
    CHECK(v.screen == Screen::Decision);
    CHECK_EQ(v.error.code, "E-DISC-5");
    CHECK_EQ(v.title, "Game data we don't recognise");
    CHECK_EQ(v.body[1], "For online play, both players need the same files.");
    CHECK_EQ(v.menu[0].label, "USE IT ANYWAY");
    CHECK_EQ(v.menu[1].label, "CHOOSE ANOTHER DISC");
    m.take_actions();
    m.navigate(Nav::Confirm);
    auto a = m.take_actions();
    CHECK(m.screen() == Screen::Preparing && a.size() == 1 && a[0].path == "custom-1a2b3c4d");

    // low space: CONTINUE enables itself when there is room
    SetupModel s;
    Environment e = good_env();
    e.free_bytes = 1100000000ull;
    to_checked(s, known_disc(), e);
    CHECK(s.screen() == Screen::Decision);
    CHECK_EQ(label(s, 0), "CONTINUE");
    CHECK(!s.view().menu[0].enabled);
    CHECK(s.view().status.empty());  // the body already says it
    s.take_actions();
    s.navigate(Nav::Confirm);
    CHECK(s.take_actions().empty() && s.screen() == Screen::Decision);
    e.free_bytes = 4000000000ull;
    s.set_environment(e);
    CHECK(s.view().menu[0].enabled);
    s.navigate(Nav::Confirm);
    CHECK(s.screen() == Screen::Preparing && has(s.take_actions(), Action::StartPrepare));

    // battery, then low RAM: one panel after the other
    SetupModel b;
    e = good_env();
    e.on_battery = true;
    e.low_memory = true;
    to_checked(b, known_disc(), e);
    CHECK_EQ(label(b, 0), "PREPARE ANYWAY");
    b.navigate(Nav::Confirm);
    CHECK(b.screen() == Screen::Decision);
    CHECK_EQ(b.view().error.code, "E-MEM-1");
    b.navigate(Nav::Confirm);
    CHECK(b.screen() == Screen::Preparing);

    // CHOOSE ANOTHER DISC leaves the decision
    SetupModel c;
    to_checked(c, id);
    c.take_actions();
    c.navigate(Nav::Down);
    c.navigate(Nav::Confirm);
    CHECK(c.screen() == Screen::Welcome && has(c.take_actions(), Action::OpenFilePicker));

    // no AVX2 is an error panel, not a decision
    SetupModel x;
    e = good_env();
    e.avx2 = false;
    to_checked(x, known_disc(), e);
    CHECK(x.has_error() && x.view().error.code == "E-SYS-5");
}

static void test_s3_preparing() {
    SetupModel m;
    to_preparing(m);
    View v = m.view();
    CHECK(v.screen == Screen::Preparing);
    CHECK_EQ(v.title, "✓ English fan translation · checked");
    CHECK(v.menu.size() == 2);
    CHECK_EQ(label(m, 0), "STOP — CONTINUE LATER");
    CHECK_EQ(label(m, 1), "WRONG DISC? CHOOSE ANOTHER");
    CHECK(v.menu[0].primary);
    CHECK_EQ(v.time_text, "About 3 min left");  // the estimate, before the first update
    CHECK_EQ(v.window_title, "0 % · Preparing — Battle Stadium D.O.N");
    CHECK_EQ(v.hint, "You can close this window; preparing continues next time.");
    m.set_progress(prog(PrepStage::Build, 0.42, 100, 812, 1876));
    v = m.view();
    CHECK(v.percent == 42);
    CHECK_EQ(v.window_title, "42 % · Preparing — Battle Stadium D.O.N");
    CHECK_EQ(v.time_text, "About 2 min left");
    CHECK_EQ(v.panel[0].text, "Copy game files");
    CHECK_EQ(v.panel[1].text, "Translate the game's code");
    CHECK_EQ(v.panel[2].text, "Build for this PC");
    CHECK_EQ(v.panel[3].text, "Final check");
    CHECK(v.panel[0].mark == Mark::Done && v.panel[1].mark == Mark::Done && v.panel[2].mark == Mark::Active);
    CHECK_EQ(v.panel[2].detail, "812/1,876");
    m.set_progress(prog(PrepStage::Build, 0.999, 3, 1870, 1876));
    CHECK(m.view().percent == 99);  // never 100 before Done

    // No SPEED item: left/right on the preparing screen do nothing
    m.take_actions();
    m.navigate(Nav::Right);
    CHECK(m.take_actions().empty());

    // WRONG DISC? CHOOSE ANOTHER
    m.navigate(Nav::Down);
    m.navigate(Nav::Confirm);
    auto a = m.take_actions();
    CHECK(has(a, Action::Cancel) && has(a, Action::OpenFilePicker) && m.screen() == Screen::Welcome);
}

static void test_stop_and_resume() {
    SetupModel m;
    to_preparing(m);
    m.set_progress(prog(PrepStage::Build, 0.42, 100, 800, 1876));
    m.navigate(Nav::Confirm);  // STOP — CONTINUE LATER
    auto a = m.take_actions();
    CHECK(has(a, Action::Cancel) && !has(a, Action::Quit));
    View v = m.view();
    CHECK(v.screen == Screen::Resume);
    CHECK_EQ(v.title, "Preparing was stopped at 42 %.");
    CHECK_EQ(v.menu[0].label, "CONTINUE PREPARING · about 2 min left");
    CHECK(v.menu[0].primary && v.selected == 0);
    CHECK_EQ(v.menu[1].label, "START OVER WITH ANOTHER DISC");
    m.navigate(Nav::Confirm);
    a = m.take_actions();
    CHECK(a.size() == 1 && a[0].action == Action::StartPrepare);
    CHECK(m.screen() == Screen::Preparing);
    CHECK(m.view().percent == 42 && m.view().time_text == "About 2 min left");

    SetupModel r;  // the next start
    r.set_environment(good_env());
    r.open_resume(42, 600);
    CHECK_EQ(label(r, 0), "CONTINUE PREPARING · about 10 min left");
    r.navigate(Nav::Down);
    r.navigate(Nav::Confirm);
    CHECK(r.screen() == Screen::Welcome && has(r.take_actions(), Action::OpenFilePicker));
}

static void test_close_confirmation() {
    SetupModel m;
    to_preparing(m);
    m.request_close();
    View v = m.view();
    CHECK(m.mode() == PrepMode::CloseConfirm && m.screen() == Screen::Preparing);
    CHECK_EQ(v.title, "Stop preparing? Your progress is saved.");
    CHECK_EQ(v.menu[0].label, "KEEP PREPARING");
    CHECK_EQ(v.menu[1].label, "STOP AND CLOSE");
    CHECK(v.menu[0].primary && !v.time_text.empty());
    CHECK(m.take_actions().empty());  // asking does not stop the work
    m.navigate(Nav::Back);            // one step back = keep preparing
    CHECK(m.mode() == PrepMode::Running && m.take_actions().empty());
    m.request_close();
    m.navigate(Nav::Down);
    m.navigate(Nav::Confirm);
    auto a = m.take_actions();
    CHECK(a.size() == 2 && a[0].action == Action::Cancel && a[1].action == Action::Quit);
    SetupModel w;  // elsewhere the close button just quits
    w.request_close();
    CHECK(has(w.take_actions(), Action::Quit));
}

static void test_done() {
    SetupModel m;
    to_preparing(m);
    m.set_ready_info(true, "v1.0");
    m.set_progress(prog(PrepStage::Done, 1.0, 0));
    View v = m.view();
    CHECK(v.screen == Screen::Ready);
    CHECK_EQ(v.title, "Your game is ready.");
    CHECK(v.body.size() == 1 && v.body[0] == "You can delete the disc image now.");
    CHECK_EQ(v.menu[0].label, "PLAY");
    m.take_actions();
    m.navigate(Nav::Back);  // nothing to go back to
    CHECK(m.screen() == Screen::Ready && m.take_actions().empty());
    m.navigate(Nav::Confirm);
    CHECK(has(m.take_actions(), Action::Play));
}

static void test_smoothing() {
    SetupModel m;
    to_preparing(m);
    m.set_progress(prog(PrepStage::Build, 0.1, 100));
    CHECK(m.shown_seconds_left() == 100.0);
    m.set_progress(prog(PrepStage::Build, 0.11, 400));  // wild jump up
    CHECK(m.shown_seconds_left() <= 120.0 + 1e-9);
    m.set_progress(prog(PrepStage::Build, 0.5, 30));  // going down is immediate
    CHECK(m.shown_seconds_left() == 30.0);
    CHECK_EQ(m.view().time_text, "Less than a minute left");
    m.set_progress(prog(PrepStage::Verify, 0.95, 500));  // a stage change may jump
    CHECK(m.shown_seconds_left() == 500.0);
}

// T-707: E08 shows the two sizes in GB, never the raw placeholders
static void test_incomplete_image_sizes() {
    uint64_t have = 0, total = 0;
    CHECK(parse_have_of_total("3328599654 of 4509715661", have, total) && have == 3328599654ull &&
          total == 4509715661ull);
    CHECK(!parse_have_of_total("100 bytes", have, total));
    CHECK_EQ(format_gb(3328599654ull), "3.1");
    CHECK_EQ(format_gb(4509715661ull), "4.2");
    SetupModel m;
    DiscIdentity id;
    id.error = ErrorId::E08;
    id.detail = "3328599654 of 4509715661";
    m.set_environment(good_env());
    m.file_chosen("/tmp/don.iso");
    m.set_disc_identity(id);
    CHECK(m.view().error.why.find('{') == std::string::npos);
    CHECK(m.view().error.why.find("3.1 GB of 4.2 GB") != std::string::npos);
}

int main() {
    test_incomplete_image_sizes();
    test_time_strings();
    test_s0_results_feed_s1();
    test_s1_actions();
    test_s2_checking();
    test_good_check_does_not_stop();
    test_decision_panel();
    test_s3_preparing();
    test_stop_and_resume();
    test_close_confirmation();
    test_done();
    test_smoothing();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
