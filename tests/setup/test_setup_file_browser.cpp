// In-app file browser model (T-628a) driven by events over a fake home tree in a temporary folder. Synthetic files
// only; the real home, disc/ and real images are never read.
// file_browser.cpp lives in src/host/, which tests/setup/CMakeLists.txt does not compile; include it here.
#include "../../src/host/file_browser.cpp"
// sanitize_utf8 lives in setup_flow_model.cpp (which needs setup_screen_model.cpp); same reason as above.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"

#include <unistd.h>

#include <chrono>

#include <cstring>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using don_host::BrowserView;
using don_host::FileBrowserModel;
using don_setup::Nav;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void write_file(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

static std::vector<std::string> labels(const BrowserView& v) {
    std::vector<std::string> l;
    for (auto& r : v.rows) l.push_back(r.label);
    return l;
}
static size_t selected(const BrowserView& v) {
    for (size_t i = 0; i < v.rows.size(); ++i) if (v.rows[i].selected) return i;
    return size_t(-1);
}
static void go(FileBrowserModel& m, std::initializer_list<Nav> navs) { for (Nav n : navs) m.navigate(n); }

static void test_starts(const fs::path& home) {
    FileBrowserModel m = FileBrowserModel::from_home(home, "nobody");
    auto v = m.view();
    CHECK(v.title == "Places");
    auto want = don_setup::discovery_roots(home, "nobody");
    CHECK(v.rows.size() == want.size() + 1);
    for (size_t i = 0; i < want.size() && i < v.rows.size(); ++i) CHECK(v.rows[i].label == want[i].string());
    CHECK(!v.rows.empty() && v.rows.back().label == "/");
    for (auto& r : v.rows) CHECK(r.is_folder);
    CHECK(selected(v) == 0);
    go(m, {Nav::Back});  // nothing above the start list
    CHECK(m.view().title == "Places");
    CHECK(!m.take_choice());
}

static void test_listing_and_nav(const fs::path& home) {
    FileBrowserModel m({home / "Downloads"});
    // Enter the start place.
    m.navigate(Nav::Confirm);
    auto v = m.view();
    CHECK(v.title == (home / "Downloads").string());
    // Folders first, then files, case-insensitive by name.
    std::vector<std::string> want = {"loopdir", "sub", "Zdir", "apple.txt", "Banana.zip", "cherry.bin", "good.iso"};
    CHECK(labels(v) == want);
    for (size_t i = 0; i < v.rows.size(); ++i) CHECK(v.rows[i].is_folder == (i < 3));
    // Verdicts.
    CHECK(v.rows[4].verdict == "ZIP archive");
    CHECK(v.rows[6].verdict == "Disc image");
    CHECK(v.rows[3].verdict == "Too small");
    // Up/Down stop at the ends (no wrap).
    CHECK(selected(v) == 0);
    go(m, {Nav::Up});
    CHECK(selected(m.view()) == 0);
    for (int i = 0; i < 20; ++i) m.navigate(Nav::Down);
    CHECK(selected(m.view()) == want.size() - 1);
    // Confirm on a file returns its path, once.
    m.navigate(Nav::Confirm);
    auto c = m.take_choice();
    CHECK(c && *c == (home / "Downloads" / "good.iso").string());
    CHECK(!m.take_choice());
    // Back goes to the parent, then at the start place to the start list.
    FileBrowserModel k({home / "Downloads"});
    go(k, {Nav::Confirm, Nav::Down, Nav::Confirm});  // into "sub"
    CHECK(k.view().title == (home / "Downloads" / "sub").string());
    k.navigate(Nav::Back);
    CHECK(k.view().title == (home / "Downloads").string());
    k.navigate(Nav::Back);
    CHECK(k.view().title == "Places");
}

static void test_game_folder_and_loop(const fs::path& home) {
    FileBrowserModel m({home / "Games"});
    go(m, {Nav::Confirm});
    auto v = m.view();
    CHECK(labels(v) == std::vector<std::string>({"gamedir", "plain"}));
    CHECK(v.rows[0].verdict == "Game folder");
    CHECK(v.rows[1].verdict.empty());
    m.navigate(Nav::Confirm);  // a folder with the game's files is chosen, not entered
    auto c = m.take_choice();
    CHECK(c && *c == (home / "Games" / "gamedir").string());
    // A plain folder is entered, not chosen.
    m.navigate(Nav::Down);
    m.navigate(Nav::Confirm);
    CHECK(!m.take_choice());
    CHECK(m.view().title == (home / "Games" / "plain").string());

    // The decision "game folder" comes from the classifier's Kind, never from the label text.
    FileBrowserModel r({home / "Games"});
    r.set_label_fn([](don_setup::Kind) { return std::string("label changed"); });
    r.navigate(Nav::Confirm);
    auto rv = r.view();
    CHECK(rv.rows.size() == 2 && rv.rows[0].verdict == "label changed");
    r.navigate(Nav::Confirm);  // gamedir: still chosen
    auto rc = r.take_choice();
    CHECK(rc && *rc == (home / "Games" / "gamedir").string());
    // A plain folder is entered even when its label is the old "Game folder" text.
    FileBrowserModel q({home / "Games"});
    q.set_label_fn([](don_setup::Kind) { return std::string("Game folder"); });
    q.navigate(Nav::Confirm);
    q.navigate(Nav::Down);
    q.navigate(Nav::Confirm);  // plain
    CHECK(!q.take_choice());
    CHECK(q.view().title == (home / "Games" / "plain").string());
}

static void test_symlink_loop(const fs::path& home) {
    // loopdir links to its own parent: Confirm on it must not enter it, so the folder shown stays Downloads.
    auto t0 = std::chrono::steady_clock::now();
    FileBrowserModel l({home / "Downloads"});
    l.navigate(Nav::Confirm);  // Downloads: first row is "loopdir"
    CHECK(l.view().rows[0].label == "loopdir");
    CHECK(!l.view().rows[0].enterable);
    CHECK(l.view().rows[1].enterable);  // a plain folder is still enterable
    l.navigate(Nav::Confirm);
    CHECK(l.view().title == (home / "Downloads").string());
    CHECK(!l.take_choice());
    // 50 more Confirms return quickly and never move the folder shown.
    for (int i = 0; i < 50; ++i) l.navigate(Nav::Confirm);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(secs < 2.0);
    CHECK(l.view().title == (home / "Downloads").string());
    CHECK(!l.take_choice());
    // A link to an ancestor (sub/up -> home) is refused the same way, from inside sub.
    FileBrowserModel a({home / "Downloads" / "sub"});
    a.navigate(Nav::Confirm);
    a.navigate(Nav::Confirm);  // sub is empty except for "up" (see main)
    CHECK(a.view().rows.size() == 1 && !a.view().rows[0].enterable);
    CHECK(a.view().title == (home / "Downloads" / "sub").string());
    CHECK(!a.take_choice());
}

static void test_unreadable_and_invalid_utf8(const fs::path& home) {
    FileBrowserModel e({home / "Empty"});
    e.navigate(Nav::Confirm);
    auto v = e.view();
    CHECK(v.rows.empty() && v.empty_reason == "This folder is empty.");
    e.navigate(Nav::Confirm);  // nothing selected: no crash, no choice
    CHECK(!e.take_choice());

    if (geteuid() != 0) {
        FileBrowserModel u({home / "Locked"});
        u.navigate(Nav::Confirm);
        auto uv = u.view();
        CHECK(uv.rows.empty() && uv.empty_reason == "This folder can't be read.");
        u.navigate(Nav::Back);
        CHECK(u.view().title == "Places");
    }

    FileBrowserModel b({home / "Bad"});
    b.navigate(Nav::Confirm);
    auto bv = b.view();
    CHECK(bv.rows.size() == 2);
    CHECK(bv.rows.size() == 2 && bv.rows[0].label == "bad\xEF\xBF\xBD" "name.txt");  // U+FFFD, not dropped
    CHECK(bv.rows.size() == 2 && bv.rows[1].label == "ctl\xEF\xBF\xBD" "name.txt");  // control character replaced
}

static void test_lazy_window(const fs::path& home) {
    FileBrowserModel m({home / "Many"});
    m.set_visible_rows(4);
    m.navigate(Nav::Confirm);
    auto v = m.view();
    CHECK(v.rows.size() == 4);
    for (int i = 0; i < 9; ++i) m.navigate(Nav::Down);  // scrolls: the selected row stays inside the window
    v = m.view();
    CHECK(v.rows.size() == 4 && selected(v) != size_t(-1));
    CHECK(v.rows[selected(v)].label == "f09.txt");
}

static void test_hover_keeps_window(const fs::path& home) {
    FileBrowserModel m({home / "Many"});
    m.set_visible_rows(4);
    m.navigate(Nav::Confirm);
    for (int i = 0; i < 9; ++i) m.navigate(Nav::Down);  // scrolled: the window starts below row 0
    auto first_label = [&](const BrowserView& v) { return v.rows.empty() ? std::string() : v.rows[0].label; };
    const auto before = m.view();
    CHECK(first_label(before) != "f00.txt");
    for (size_t row = 0; row < before.rows.size(); ++row) {
        m.select_visible(row);
        auto v = m.view();
        CHECK(first_label(v) == first_label(before));  // the rows do not slide under the mouse
        CHECK(v.rows.size() == 4 && selected(v) == row);
    }
    m.select_visible(4);  // outside the window: nothing
    CHECK(selected(m.view()) == 3);
    m.navigate(Nav::Up);  // a key inside the window moves only the focus
    CHECK(first_label(m.view()) == first_label(before));
}

// An ISO 9660 image of `size` bytes whose primary volume descriptor (sector 16) says `sectors` volume sectors.
static Bytes make_iso(size_t size, uint32_t sectors) {
    Bytes iso(size, 0);
    std::memcpy(iso.data() + 16 * 2048 + 1, "CD001", 5);
    for (int i = 0; i < 4; ++i) iso[16 * 2048 + 80 + i] = uint8_t(sectors >> (8 * i));  // little-endian, ECMA-119 8.4.8
    return iso;
}

static void test_iso_completeness(const fs::path& home) {
    FileBrowserModel m({home / "Images"});
    m.navigate(Nav::Confirm);
    auto v = m.view();
    CHECK(labels(v) == std::vector<std::string>({"complete.iso", "incomplete.iso"}));
    CHECK(v.rows.size() == 2 && v.rows[0].verdict == "Disc image");
    CHECK(v.rows.size() == 2 && v.rows[1].verdict == "Incomplete disc image");
    // The verdict is cached per file: a second view shows the same labels.
    CHECK(m.view().rows.size() == 2 && m.view().rows[1].verdict == "Incomplete disc image");
}

// A raw 2352-byte-sector image (classified Iso) keeps its ISO marker at 2352-byte stride: sector 16 with the 16-byte
// sync and header in front of the user data (Mode 1). Bytes 80-83 after the 2048-stride descriptor offset hold
// unrelated non-zero data: the image is complete and must stay "Disc image".
static Bytes make_raw_iso(size_t size, size_t stride, uint32_t junk_sectors) {
    Bytes iso(size, 0);
    std::memcpy(iso.data() + 16 * stride + 16 + 1, "CD001", 5);
    for (int i = 0; i < 4; ++i) iso[16 * 2048 + 80 + i] = uint8_t(junk_sectors >> (8 * i));
    return iso;
}

static void test_raw_iso_stride(const fs::path& home) {
    FileBrowserModel m({home / "Raw"});
    m.navigate(Nav::Confirm);
    auto v = m.view();
    CHECK(labels(v) == std::vector<std::string>({"raw2352.iso"}));
    CHECK(v.rows.size() == 1 && v.rows[0].verdict == "Disc image");
}

int main() {
    fs::path tmp = fs::temp_directory_path() / ("don_browser_" + std::to_string(getpid()));
    fs::remove_all(tmp);
    fs::path home = tmp / "home";

    Bytes iso(17 * 2048 + 64, 0);
    std::memcpy(iso.data() + 16 * 2048 + 1, "CD001", 5);
    Bytes none;
    write_file(home / "Downloads" / "good.iso", iso);
    write_file(home / "Images" / "complete.iso", make_iso(17 * 2048 + 64, 17));  // 17 sectors fit in the file
    write_file(home / "Images" / "incomplete.iso", make_iso(17 * 2048 + 64, 1000));  // 1000 sectors do not
    write_file(home / "Raw" / "raw2352.iso", make_raw_iso(17 * 2352 + 64, 2352, 1000));
    write_file(home / "Downloads" / "Banana.zip", Bytes{'P', 'K', 3, 4, 0, 0, 0, 0});
    write_file(home / "Downloads" / "apple.txt", Bytes(100, 'a'));
    write_file(home / "Downloads" / "cherry.bin", Bytes(100, ' '));
    fs::create_directories(home / "Downloads" / "sub");
    fs::create_directories(home / "Downloads" / "Zdir");
    fs::create_directory_symlink(home / "Downloads", home / "Downloads" / "loopdir");  // loop onto its parent
    fs::create_directory_symlink(home, home / "Downloads" / "sub" / "up");  // loop onto an ancestor
    write_file(home / "Games" / "gamedir" / "SYSTEM.CNF", Bytes(8, 'x'));
    write_file(home / "Games" / "gamedir" / "SLPS_256.75", Bytes(8, 'x'));
    fs::create_directories(home / "Games" / "plain");
    fs::create_directories(home / "Empty");
    fs::create_directories(home / "Locked");
    fs::permissions(home / "Locked", fs::perms::none);
    write_file(home / "Bad" / "bad\xFFname.txt", Bytes(4, 'z'));
    write_file(home / "Bad" / "ctl\x01name.txt", Bytes(4, 'z'));
    for (int i = 0; i < 12; ++i) {
        char n[16];
        std::snprintf(n, sizeof n, "f%02d.txt", i);
        write_file(home / "Many" / n, Bytes(4, 'z'));
    }

    test_starts(home);
    test_listing_and_nav(home);
    test_game_folder_and_loop(home);
    test_symlink_loop(home);
    test_unreadable_and_invalid_utf8(home);
    test_lazy_window(home);
    test_hover_keeps_window(home);
    test_iso_completeness(home);
    test_raw_iso_stride(home);

    fs::permissions(home / "Locked", fs::perms::owner_all);
    fs::remove_all(tmp);
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    std::cout << "file browser model OK\n";
    return 0;
}
