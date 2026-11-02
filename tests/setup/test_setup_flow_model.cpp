// Tests of the A2 UX rules (docs/tasks/M6-public-release.md) on the setup screen model, plus the error panel, the
// settings rows and the update view. No SDL. The .cpp files are included so one command builds them.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"

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

// One view of every state the flow has, for the rule tests.
static std::vector<std::pair<std::string, View>> all_views() {
    std::vector<std::pair<std::string, View>> out;
    SetupModel w;
    w.set_environment(good_env());
    out.push_back({"welcome", w.view()});
    w.set_discovery({{"/x/don.iso", "Japanese (original)"}});
    out.push_back({"welcome-found", w.view()});
    SetupModel c;
    c.set_environment(good_env());
    c.file_chosen("/tmp/don.iso");
    out.push_back({"checking", c.view()});
    DiscIdentity u;
    u.error = ErrorId::E17;
    SetupModel d;
    to_checked(d, u);
    out.push_back({"decision-e17", d.view()});
    SetupModel s;
    Environment low = good_env();
    low.free_bytes = 1;
    to_checked(s, known_disc(), low);
    out.push_back({"decision-space", s.view()});
    SetupModel p;
    to_preparing(p);
    out.push_back({"preparing-0", p.view()});
    p.set_progress(prog(PrepStage::Build, 0.42, 100, 812, 1876));
    out.push_back({"preparing-build", p.view()});
    p.request_close();
    out.push_back({"close-confirm", p.view()});
    SetupModel r;
    r.open_resume(42, 600);
    out.push_back({"resume", r.view()});
    SetupModel up;
    up.set_environment(good_env());
    up.begin_update("1.2.0");
    out.push_back({"update", up.view()});
    SetupModel done;
    to_preparing(done);
    done.set_progress(prog(PrepStage::Done, 1, 0));
    out.push_back({"ready", done.view()});
    SetupModel st;
    SettingsInfo si;
    si.variants = {"Original", "English fan translation"};
    st.open_settings(si);
    out.push_back({"settings", st.view()});
    SetupModel e;
    e.set_error_id(ErrorId::E15);
    out.push_back({"error", e.view()});
    return out;
}

static void rule1_exactly_one_primary_preselected() {
    for (auto& [name, v] : all_views()) {
        int n = 0;
        for (auto& m : v.menu) n += m.primary;
        const bool ok = n == 1 && v.selected >= 0 && v.selected < static_cast<int>(v.menu.size()) &&
                        v.menu[v.selected].primary;
        if (!ok) std::printf("rule1 broken in %s\n", name.c_str());
        CHECK(ok);
    }
}

static void rule2_every_wait_shows_time_left_and_an_estimate_comes_first() {
    for (auto& [name, v] : all_views()) {
        if (v.waiting && v.time_text.empty()) std::printf("rule2 broken in %s\n", name.c_str());
        CHECK(!v.waiting || !v.time_text.empty());
    }
    SetupModel m;
    m.set_environment(good_env());
    CHECK_EQ(label(m, 0), "CHOOSE DISC IMAGE…");
    SetupModel n;  // not measured yet: the wait says so
    n.file_chosen("/tmp/don.iso");
    n.set_disc_identity(known_disc());
    CHECK_EQ(n.view().time_text, "Estimating time left…");
    SetupModel r;
    r.open_resume(10, 6000);
    CHECK_EQ(label(r, 0), "CONTINUE PREPARING · about 1 h 40 min left");
}

static void rule3_welcome_is_one_short_sentence() {
    SetupModel m;
    const View v = m.view();
    CHECK_EQ(v.body[0], "Choose the disc image of your copy of the game.");
    CHECK(v.body.size() == 1 && v.notes.empty() && v.panel.empty());
}

static void rule4_never_a_frozen_screen() {
    for (auto& [name, v] : all_views()) {
        const bool ok = !v.waiting || v.bar >= 0 || v.percent >= 0;
        if (!ok) std::printf("rule4 broken in %s\n", name.c_str());
        CHECK(ok);
    }
    SetupModel c;  // the check moves with the app's updates
    c.set_environment(good_env());
    c.file_chosen("/tmp/don.iso");
    c.set_check_progress(0.5, 1.0);
    CHECK(c.view().bar == 0.5);
    SetupModel p;  // preparing shows 0 % and an empty bar before the first update
    to_preparing(p);
    CHECK(p.view().percent == 0 && p.view().bar == 0.0);
}

static void rule5_every_error_has_code_what_why_todo_and_a_fitting_action() {
    std::size_t count = 0;
    const ErrorEntry* e = error_entries(&count);
    int shown = 0, silent = 0;
    for (std::size_t i = 0; i < count; ++i) {
        SetupModel m;
        m.set_error_id(e[i].id);
        if (!*e[i].info.action) {  // A4 says nothing is shown
            CHECK(!m.has_error());
            ++silent;
            continue;
        }
        ++shown;
        const View v = m.view();
        CHECK(v.error_overlay);
        CHECK_EQ(v.error.code, e[i].info.code);
        CHECK(!v.error.title.empty() && !v.error.why.empty() && !v.error.todo.empty());
        CHECK(v.error.todo.find("What you can do") == std::string::npos);
        CHECK_EQ(v.menu[0].label, e[i].info.action);  // the primary action is the table's
        CHECK(v.menu[0].primary && v.selected == 0);
        CHECK_EQ(v.menu[1].label, "COPY REPORT");
        if (v.menu[0].label == "COPY REPORT") ++g_fail;
    }
    CHECK(shown > 40 && silent > 0);
    // placeholders are filled
    SetupModel m;
    m.set_error_id(ErrorId::E13, {{"serial", "SLES-12345"}});
    CHECK(m.view().error.why.find("SLES-12345") != std::string::npos);
    CHECK(m.view().error.why.find('{') == std::string::npos);
    // a message the table does not have still gets all five parts
    SetupModel g;
    g.set_error(ErrorPanel{});
    const View gv = g.view();
    CHECK(!gv.error.code.empty() && !gv.error.title.empty() && !gv.error.why.empty() && !gv.error.todo.empty());
    CHECK_EQ(gv.menu[0].label, "TRY AGAIN");
}

static void rule6_stops_only_when_a_decision_is_needed() {
    SetupModel m;  // nothing to decide: the check goes straight on
    to_checked(m, known_disc());
    CHECK(m.screen() == Screen::Preparing);
    for (auto id : {ErrorId::E17, ErrorId::E30, ErrorId::E39, ErrorId::E36}) {  // each decision stops once
        SetupModel d;
        Environment e = good_env();
        DiscIdentity disc = known_disc();
        if (id == ErrorId::E17) disc.error = ErrorId::E17;
        if (id == ErrorId::E30) e.free_bytes = 1;
        e.on_battery = id == ErrorId::E39;
        e.low_memory = id == ErrorId::E36;
        to_checked(d, disc, e);
        CHECK(d.screen() == Screen::Decision);
        d.set_environment(good_env());
        d.navigate(Nav::Confirm);
        CHECK(d.screen() == Screen::Preparing);
    }
}

static void rule7_accessibility_in_the_model() {
    for (auto& [name, v] : all_views()) {  // never colour alone: every mark, item and row carries text
        for (auto& r : v.steps) CHECK(!r.text.empty());
        for (auto& r : v.panel) CHECK(!r.text.empty());
        for (auto& m : v.menu) CHECK(!m.label.empty());
        CHECK(v.selected >= 0 && v.selected < static_cast<int>(v.menu.size()));  // there is always a focus to draw
    }
    SetupModel m;
    to_preparing(m);
    CHECK_EQ(label(m, 0), "STOP — CONTINUE LATER");
    m.set_environment(good_env());
    CHECK(m.view().status.empty() || m.view().status.find("✓") == std::string::npos);
    // UTF-8 is sanitised wherever a file name is shown
    CHECK_EQ(sanitize_utf8("a\xC3\xA9\xE6\x97\xA5"), "a\xC3\xA9\xE6\x97\xA5");
    CHECK_EQ(sanitize_utf8("a\xFF" "b"), "a\xEF\xBF\xBD" "b");
    CHECK_EQ(sanitize_utf8("a\xC0\xAF"), "a\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK_EQ(sanitize_utf8("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK_EQ(sanitize_utf8("x\ny\x1B"), "x\xEF\xBF\xBD" "y\xEF\xBF\xBD");
    CHECK_EQ(sanitize_utf8("trunc\xE6\x97"), "trunc\xEF\xBF\xBD\xEF\xBF\xBD");
    SetupModel c;
    c.file_chosen("/tmp/bad\xFF.iso");
    CHECK_EQ(c.view().body[0], "bad\xEF\xBF\xBD.iso");
}

static void s5_error_primary_actions() {
    // a failed build: TRY AGAIN starts again, COPY REPORT stays on the panel
    SetupModel m;
    to_preparing(m);
    Progress f = prog(PrepStage::Failed, 0.3, 0);
    f.detail = "The compiler ran out of memory.";
    m.set_progress(f);
    View v = m.view();
    CHECK(v.error_overlay && v.error.why == "The compiler ran out of memory.");
    CHECK_EQ(v.menu[0].label, "TRY AGAIN");
    m.navigate(Nav::Down);
    m.navigate(Nav::Confirm);
    CHECK(has(m.take_actions(), Action::CopyReport) && m.has_error());
    m.navigate(Nav::Up);
    m.navigate(Nav::Confirm);
    CHECK(has(m.take_actions(), Action::StartPrepare) && !m.has_error() && m.screen() == Screen::Preparing);
    m.set_progress(f);
    m.navigate(Nav::Back);  // dismissing a failed build returns to S1
    CHECK(!m.has_error() && m.screen() == Screen::Welcome);

    // CONTINUE after a wait the app caused resumes the work
    SetupModel d;
    to_preparing(d);
    d.set_error_id(ErrorId::E31, {{"gb", "1.2"}});
    CHECK_EQ(label(d, 0), "CONTINUE");
    d.navigate(Nav::Confirm);
    CHECK(has(d.take_actions(), Action::Resume) && d.screen() == Screen::Preparing);

    // CHOOSE ANOTHER DISC after a bad disc; TRY AGAIN re-checks the same file (E06)
    SetupModel b;
    DiscIdentity bad;
    bad.error = ErrorId::E13;
    bad.detail = "SLES-12345";
    to_checked(b, bad);
    CHECK(b.has_error() && b.view().error.code == error_info(ErrorId::E13).code);
    CHECK_EQ(label(b, 0), "CHOOSE ANOTHER DISC");
    b.take_actions();
    b.navigate(Nav::Confirm);
    CHECK(b.screen() == Screen::Welcome && has(b.take_actions(), Action::OpenFilePicker));
    SetupModel g;
    g.set_environment(good_env());
    g.file_chosen("/tmp/don.iso");
    g.set_error_id(ErrorId::E06, {{"name", "don.iso"}});
    CHECK_EQ(label(g, 0), "TRY AGAIN");
    g.take_actions();
    g.navigate(Nav::Confirm);
    auto a = g.take_actions();
    CHECK(g.screen() == Screen::Checking && a.size() == 1 && a[0].action == Action::IdentifyDisc);
    // while the panel is up, files do not leak through
    SetupModel w;
    w.set_error_id(ErrorId::E15);
    w.file_chosen("/tmp/a.iso");
    CHECK(w.take_actions().empty());
    // QUIT, REPAIR and PLAY entries emit their own actions
    SetupModel q;
    q.set_error_id(ErrorId::E50);
    q.navigate(Nav::Confirm);
    CHECK(has(q.take_actions(), Action::Quit));
    SetupModel rp;
    rp.set_error_id(ErrorId::E77);
    rp.navigate(Nav::Confirm);
    CHECK(has(rp.take_actions(), Action::Repair));
}

static void s6_settings_rows_and_s7_update() {
    SetupModel m;
    SettingsInfo si;
    si.variants = {"Original", "English fan translation"};
    si.used_bytes = 3200000000ull;
    m.open_settings(si);
    View v = m.view();
    CHECK(v.screen == Screen::Settings);
    CHECK_EQ(v.panel[0].text + "|" + v.panel[0].detail, "GAME VERSION ◂▸|Original");
    CHECK_EQ(v.panel[1].text + "|" + v.panel[1].detail, "STORAGE|3.2 GB used");
    CHECK_EQ(label(m, 1), "ADD ANOTHER DISC…");
    CHECK_EQ(label(m, 2), "FREE UP SPACE");
    CHECK_EQ(label(m, 3), "REMOVE GAME DATA");
    CHECK_EQ(label(m, 4), "REPAIR");
    m.navigate(Nav::Right);  // switching is instant: one action
    auto a = m.take_actions();
    CHECK(a.size() == 1 && a[0].action == Action::SwitchVariant && a[0].path == "English fan translation");
    CHECK_EQ(m.view().panel[0].detail, "English fan translation");
    m.navigate(Nav::Right);  // wraps
    CHECK_EQ(m.view().panel[0].detail, "Original");
    m.take_actions();
    m.activate(2);
    CHECK(has(m.take_actions(), Action::FreeUpSpace));
    m.activate(3);
    CHECK(has(m.take_actions(), Action::RemoveGameData));
    m.activate(4);
    CHECK(has(m.take_actions(), Action::Repair));
    m.activate(1);  // ADD ANOTHER DISC… opens the picker on S1; BACK returns to Settings
    CHECK(has(m.take_actions(), Action::OpenFilePicker) && m.screen() == Screen::Welcome);
    CHECK_EQ(label(m, m.view().menu.size() - 1), "BACK");

    SetupModel u;  // S7 is the S3 view; cancelling keeps the installed version playable
    u.set_environment(good_env());
    u.begin_update("1.2.0");
    auto ua = u.take_actions();
    CHECK(ua.size() == 1 && ua[0].action == Action::StartPrepare && ua[0].path.empty());
    View uv = u.view();
    CHECK(uv.screen == Screen::Preparing);
    CHECK_EQ(uv.body[0], "Version 1.2.0 · about 3 min");
    CHECK(uv.menu.size() == 1 && uv.menu[0].label == "STOP — CONTINUE LATER");
    u.navigate(Nav::Confirm);
    ua = u.take_actions();
    CHECK(has(ua, Action::Cancel) && has(ua, Action::Play));
}

static void t680_stepper_values_and_decision_focus() {
    SetupModel p, s, d; SettingsInfo si; si.variants = {"Original", "English fan translation"};
    Environment e = good_env(); e.free_bytes = 1;
    to_preparing(p); s.open_settings(si); to_checked(d, known_disc(), e);
    CHECK_EQ(p.view().menu[0].value + "|" + s.view().menu[0].value, "|Original");
    p.navigate(Nav::Right); s.navigate(Nav::Right);  // preparing has no stepper
    CHECK_EQ(p.view().menu[0].value + "|" + s.view().menu[0].value, "|English fan translation");
    CHECK(!d.view().menu[0].enabled && d.view().menu[d.view().selected].primary);  // rule 1: the primary stays preselected
}

int main() {
    rule1_exactly_one_primary_preselected();
    rule2_every_wait_shows_time_left_and_an_estimate_comes_first();
    rule3_welcome_is_one_short_sentence();
    rule4_never_a_frozen_screen();
    rule5_every_error_has_code_what_why_todo_and_a_fitting_action();
    rule6_stops_only_when_a_decision_is_needed();
    rule7_accessibility_in_the_model();
    s5_error_primary_actions();
    s6_settings_rows_and_s7_update();
    t680_stepper_values_and_decision_focus();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
