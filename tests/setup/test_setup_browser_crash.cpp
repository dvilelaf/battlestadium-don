// T-701: the in-app browser crashed the whole program when a folder was chosen. These cases walk the model over odd
// folders; build with -fsanitize=address,undefined so a use-after-free or overflow aborts the run.
// Synthetic files only, in a temporary folder.
#include "../../src/host/file_browser.cpp"
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"

#include <unistd.h>

#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using don_host::FileBrowserModel;
using don_setup::Nav;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::cerr << __FILE__ << ":" << __LINE__ << " FAIL " #c "\n"; ++g_fail; } \
    } while (0)

static void touch(const fs::path& p, const std::string& body = "x") {
    std::ofstream(p, std::ios::binary) << body;
}

static std::vector<std::string> labels(FileBrowserModel& m) {
    std::vector<std::string> r;
    for (auto& row : m.view().rows) r.push_back(row.label);
    return r;
}

// Opens the start folder `root` and returns a model focused on its first entry.
static FileBrowserModel at(const fs::path& root) {
    FileBrowserModel m({root});
    m.set_visible_rows(8);
    m.navigate(Nav::Confirm);  // the first place
    return m;
}

int main() {
    fs::path base = fs::temp_directory_path() / ("t701-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    // Enter a folder: the focused row's path must stay valid while the listing is replaced.
    {
        fs::path root = base / "enter";
        fs::create_directories(root / "inner");
        touch(root / "inner" / "a.iso");
        FileBrowserModel m = at(root);
        m.navigate(Nav::Confirm);  // inner
        CHECK(m.view().title == (root / "inner").string());
        CHECK(labels(m) == std::vector<std::string>{"a.iso"});
        m.navigate(Nav::Back);  // ".." -> root
        CHECK(m.view().title == root.string());
        m.navigate(Nav::Back);  // a start place: the list of places
        CHECK(m.view().title == "Places");
    }
    // Empty folder, unreadable folder.
    {
        fs::path root = base / "odd";
        fs::create_directories(root / "empty");
        fs::create_directories(root / "locked");
        touch(root / "locked" / "f");
        fs::permissions(root / "locked", fs::perms::none);
        FileBrowserModel m = at(root);
        m.navigate(Nav::Confirm);  // empty
        CHECK(!m.view().empty_reason.empty());
        m.navigate(Nav::Back);
        m.navigate(Nav::Down);
        m.navigate(Nav::Confirm);  // locked (as root it stays readable; either way no crash)
        m.navigate(Nav::Back);
        fs::permissions(root / "locked", fs::perms::owner_all);
    }
    // 5000 files.
    {
        fs::path root = base / "big";
        fs::create_directories(root / "d");
        for (int i = 0; i < 5000; ++i) touch(root / "d" / ("f" + std::to_string(i) + ".bin"), "");
        FileBrowserModel m = at(root);
        m.navigate(Nav::Confirm);
        for (int i = 0; i < 4999; ++i) m.navigate(Nav::Down);
        auto v = m.view();
        CHECK(v.rows.size() <= 8 && !v.rows.empty() && v.rows.back().selected);
    }
    // Symlinked folder, spaces and non-ASCII names, a file vanishing between listing and choosing.
    {
        fs::path root = base / "links";
        fs::create_directories(root / "real dir ñandú 日本");
        touch(root / "real dir ñandú 日本" / "disc.iso");
        fs::create_directory_symlink(root / "real dir ñandú 日本", root / "link");
        touch(root / "gone.iso");
        FileBrowserModel m = at(root);
        CHECK(m.view().rows.size() == 3);
        m.navigate(Nav::Confirm);  // first folder
        m.navigate(Nav::Back);
        m.navigate(Nav::Down);
        m.navigate(Nav::Down);
        m.view();  // lists "gone.iso" and its verdict
        fs::remove(root / "gone.iso");
        m.navigate(Nav::Confirm);  // choosing a file that is no longer there
        auto c = m.take_choice();
        CHECK(c && fs::path(*c).filename() == "gone.iso");
    }
    // Confirm on a folder at the start list and ".." from deep inside.
    {
        fs::path root = base / "deep";
        fs::create_directories(root / "a" / "b" / "c");
        FileBrowserModel m = at(root);
        for (int i = 0; i < 3; ++i) m.navigate(Nav::Confirm);
        for (int i = 0; i < 4; ++i) m.navigate(Nav::Back);
        CHECK(m.view().title == "Places");
    }

    fs::remove_all(base);
    if (g_fail) { std::cerr << g_fail << " checks failed\n"; return 1; }
    std::cout << "ok\n";
    return 0;
}
