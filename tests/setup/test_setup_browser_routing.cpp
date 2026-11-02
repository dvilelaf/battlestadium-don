// Tests of the setup screen's routing to the in-app browser (T-677): a failed system dialog opens the browser (E02),
// BROWSE opens it, a drop closes it and reaches the model, a stale dialog answer is ignored, mouse hover and click.
// No SDL window. The .cpp files are included so one command builds them.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"
#include "../../src/host/file_browser.cpp"
#include "../../src/host/setup_browser_routing.cpp"

#include <cstdio>
#include <fstream>

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
namespace fs = std::filesystem;

fs::path make_tree() {
    const fs::path root = fs::temp_directory_path() / "don_t677_tree";
    fs::remove_all(root);
    fs::create_directories(root / "dirA");
    fs::create_directories(root / "dirB");
    std::ofstream(root / "file.bin") << "x";
    return root;
}

// Runs every action the model emitted through route_action; returns the dialog numbers it asked for.
std::vector<std::uint64_t> pump(SetupModel &m, BrowserRouter &r) {
    std::vector<std::uint64_t> dialogs;
    for (const ActionRequest &a : m.take_actions())
        route_action(r, a, "/nonexistent-home", "u", [&](std::uint64_t n) { dialogs.push_back(n); });
    return dialogs;
}

bool identified(SetupModel &m, const std::string &path) {
    for (const ActionRequest &a : m.take_actions())
        if (a.action == Action::IdentifyDisc && a.path == path)
            return true;
    return false;
}

struct TestRouter : BrowserRouter { // a router is not copyable (it holds a mutex): build it in place
    explicit TestRouter(const fs::path &root) { set_start_places({root}); }
};
} // namespace

int main() {
    const fs::path root = make_tree();

    { // BROWSE opens the browser
        SetupModel m;
        TestRouter r(root);
        CHECK(route_action(r, {Action::OpenBrowser, {}}, "", "", [](std::uint64_t) {}));
        CHECK(r.browser_active());
        CHECK(!route_action(r, {Action::Quit, {}}, "", "", [](std::uint64_t) {}));
    }
    { // a failed system dialog opens the in-app browser (E02)
        SetupModel m;
        TestRouter r(root);
        const auto asked = pump(m, r); // the model is on S1; ask for the picker the way the menu does
        (void)asked;
        const std::uint64_t seq = r.begin_dialog();
        route_dialog_result(m, r, {true, "", seq});
        CHECK(!r.browser_active()); // the model only asks...
        pump(m, r);
        CHECK(r.browser_active());  // ...and the action opens it
    }
    { // a drop while the browser is open closes it and hands the path to the model
        SetupModel m;
        TestRouter r(root);
        route_action(r, {Action::OpenBrowser, {}}, "", "", [](std::uint64_t) {});
        route_drop(m, r, "/some/disc.iso");
        CHECK(!r.browser_active());
        CHECK(identified(m, "/some/disc.iso"));
    }
    { // a dialog path reaches the model; "" (cancel) does not identify anything
        SetupModel m;
        TestRouter r(root);
        route_dialog_result(m, r, {false, "/a.iso", r.begin_dialog()});
        CHECK(identified(m, "/a.iso"));
        SetupModel m2;
        route_dialog_result(m2, r, {false, "", r.begin_dialog()});
        CHECK(m2.take_actions().empty());
    }
    { // stale: a result for an older request is ignored
        SetupModel m;
        TestRouter r(root);
        const std::uint64_t first = r.begin_dialog();
        const std::uint64_t second = r.begin_dialog();
        route_dialog_result(m, r, {false, "/old.iso", first});
        CHECK(m.take_actions().empty());
        route_dialog_result(m, r, {false, "/new.iso", second});
        CHECK(identified(m, "/new.iso"));
    }
    { // stale: a result that arrives after a drop is ignored
        SetupModel m;
        TestRouter r(root);
        const std::uint64_t seq = r.begin_dialog();
        route_drop(m, r, "/dropped.iso");
        CHECK(identified(m, "/dropped.iso"));
        SetupModel m2;
        route_dialog_result(m2, r, {false, "/late.iso", seq});
        CHECK(m2.take_actions().empty());
    }
    { // stale: a result that arrives after the browser opened is ignored, a failure too
        SetupModel m;
        TestRouter r(root);
        const std::uint64_t seq = r.begin_dialog();
        route_action(r, {Action::OpenBrowser, {}}, "", "", [](std::uint64_t) {});
        route_dialog_result(m, r, {false, "/late.iso", seq});
        CHECK(m.take_actions().empty());
        route_dialog_result(m, r, {true, "", seq});
        CHECK(m.take_actions().empty());
    }
    { // a late callback of an older request does not overwrite the current answer
        TestRouter r(root);
        const std::uint64_t first = r.begin_dialog();
        const std::uint64_t second = r.begin_dialog();
        r.deliver_dialog({false, "/new.iso", second});
        r.deliver_dialog({false, "/old.iso", first}); // arrives before the poll
        const auto got = r.take_dialog_result();
        CHECK(got && got->path == "/new.iso");
        CHECK(!r.take_dialog_result());
        r.deliver_dialog({false, "/old2.iso", first}); // stale on its own
        CHECK(!r.take_dialog_result());
        const std::uint64_t third = r.begin_dialog();
        r.deliver_dialog({false, "/x.iso", third});
        r.invalidate_dialog(); // a drop clears an answer not yet taken
        CHECK(!r.take_dialog_result());
    }
    { // a result is accepted once
        SetupModel m;
        TestRouter r(root);
        const std::uint64_t seq = r.begin_dialog();
        route_dialog_result(m, r, {false, "", seq});
        route_dialog_result(m, r, {false, "/twice.iso", seq});
        CHECK(m.take_actions().empty());
    }
    { // keys: Escape closes the browser; with it closed the keys are not taken
        SetupModel m;
        TestRouter r(root);
        CHECK(!route_browser_nav(m, r, Nav::Down, false));
        route_action(r, {Action::OpenBrowser, {}}, "", "", [](std::uint64_t) {});
        CHECK(route_browser_nav(m, r, Nav::Down, false));
        CHECK(r.browser_active());
        CHECK(route_browser_nav(m, r, Nav::Back, true));
        CHECK(!r.browser_active());
    }
    { // mouse: hover moves the focus, a click enters a folder, a click on a file picks it
        SetupModel m;
        TestRouter r(root);
        route_action(r, {Action::OpenBrowser, {}}, "", "", [](std::uint64_t) {});
        route_browser_nav(m, r, Nav::Confirm, false); // the start place -> its content
        BrowserView v = r.browser()->view();
        CHECK(v.rows.size() == 3);
        route_browser_mouse(m, r, 1, false);
        v = r.browser()->view();
        CHECK(v.rows[1].selected && !v.rows[0].selected);
        route_browser_mouse(m, r, 0, false);
        v = r.browser()->view();
        CHECK(v.rows[0].selected);
        route_browser_mouse(m, r, 99, true); // out of the list: nothing
        CHECK(r.browser_active() && r.browser()->view().rows[0].selected);
        route_browser_mouse(m, r, 0, true); // dirA: enters it
        CHECK(r.browser_active());
        CHECK(r.browser()->view().title.find("dirA") != std::string::npos);
        route_browser_nav(m, r, Nav::Back, false); // back to the root
        v = r.browser()->view();
        std::size_t file_row = 0;
        for (std::size_t i = 0; i < v.rows.size(); ++i)
            if (!v.rows[i].is_folder)
                file_row = i;
        route_browser_mouse(m, r, static_cast<int>(file_row), true);
        CHECK(!r.browser_active());
        CHECK(identified(m, (root / "file.bin").string()));
    }

    { // T-701: the system chooser filters by the disc-image kinds the identify step accepts, plus "All files"
        const auto f = disc_dialog_filters();
        CHECK(f.size() == 2 && f[0].pattern.find("iso") != std::string::npos);
        for (const char *ext : {"iso", "gz", "zip", "chd", "cso", "zso", "7z", "rar", "ecm", "nrg", "mdf", "cue"})
            CHECK(f[0].pattern.find(ext) != std::string::npos);
        CHECK(f[0].pattern.find(';') != std::string::npos && f[0].pattern.find('.') == std::string::npos);
        CHECK(f[1].pattern == "*");
        // A cancelled system chooser changes nothing; a failed one asks for the in-app browser.
        SetupModel m;
        BrowserRouter r;
        const auto seq = r.begin_dialog();
        route_dialog_result(m, r, DialogResult{false, "", seq});
        CHECK(!r.browser_active());
        const auto seq2 = r.begin_dialog();
        route_dialog_result(m, r, DialogResult{true, "", seq2});
        bool open_browser = false;
        for (const auto &a : m.take_actions())
            open_browser |= a.action == Action::OpenBrowser;
        CHECK(open_browser);
    }

    fs::remove_all(root);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
