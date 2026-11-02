// In-app file browser model (T-628a; A3 S1 BROWSE, A4 E02/E03): the folders and files a player walks through when the
// system file dialog is missing or a gamepad is used. Pure model: no SDL, no ImGui. A view draws view() and feeds
// navigate(Nav); the chosen path is taken once with take_choice() and goes to SetupFlowModel::file_chosen.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "setup_screen_model.h"  // Nav
#include "../setup/disc_classify.h"
#include "../setup/discover.h"

namespace don_host {

struct BrowserRow {
    std::string label;    // file name, UTF-8; invalid bytes shown as U+FFFD
    std::string verdict;  // short label from classify(), "" for folders without the game
    bool is_folder = false;
    bool selected = false;
    bool enterable = true;  // false for a folder that Confirm refuses to enter: a link back to it or to an ancestor
};

struct BrowserView {
    std::string title;         // the folder shown, or "Places" for the start list
    std::vector<BrowserRow> rows;  // only the visible window
    std::string empty_reason;  // non-empty when the list has no entries ("Folder is empty", "Can't read this folder")
};

class FileBrowserModel {
public:
    // `starts` are the start places in order (existing folders only); "/" is appended unless already present.
    explicit FileBrowserModel(std::vector<std::filesystem::path> starts);
    // S0's folders under `home` (discovery_roots) plus "/".
    static FileBrowserModel from_home(const std::filesystem::path& home, const std::string& user);

    void navigate(don_setup::Nav nav);
    void set_visible_rows(size_t n) { visible_ = n ? n : 1; }  // window height; default 12
    // Replaces the verdict texts (translation, tests). Display only: no decision reads them.
    void set_label_fn(std::function<std::string(don_setup::Kind)> fn) { label_fn_ = std::move(fn); verdicts_.clear(); truncated_.clear(); }
    // Focus the row `row` of the current view() without moving the window (mouse hover); out of range = nothing.
    void select_visible(size_t row);
    BrowserView view();
    std::optional<std::string> take_choice();  // the chosen path, once

private:
    struct Entry { std::filesystem::path path; std::string name; bool is_folder = false; };
    void open_dir(std::filesystem::path dir);  // by value: callers pass a path stored in entries_, which this clears
    void show_starts();
    size_t count() const { return at_starts_ ? starts_.size() : entries_.size(); }
    don_setup::Kind kind_for(const std::filesystem::path& p);  // classifier verdict, cached
    std::string verdict_for(const std::filesystem::path& p);  // its display label
    bool is_truncated(const std::filesystem::path& p);  // an ISO whose volume is larger than the file, cached
    // True when `target` is the current folder or one of its ancestors (a symlink loop); errors count as "no".
    bool would_loop(const std::filesystem::path& target) const;

    std::vector<std::filesystem::path> starts_;
    bool at_starts_ = true;
    std::filesystem::path dir_;
    std::vector<Entry> entries_;
    std::string error_;
    size_t sel_ = 0;
    size_t first_ = 0;  // first row of the visible window; moves only when the focus leaves it
    void follow();      // keeps sel_ inside the window
    size_t visible_ = 12;
    std::map<std::string, don_setup::Kind> verdicts_;  // lazy cache, cleared on each directory change
    std::map<std::string, bool> truncated_;            // lazy cache of is_truncated, cleared with verdicts_
    std::function<std::string(don_setup::Kind)> label_fn_;
    std::optional<std::string> choice_;
};

}  // namespace don_host
