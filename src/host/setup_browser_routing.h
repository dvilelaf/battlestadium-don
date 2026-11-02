// SPDX-License-Identifier: GPL-3.0-or-later
// How the setup screen's inputs reach the in-app browser and the model (A3 S1 BROWSE, A4 E02): the system dialog's
// answer, a dropped path, the actions the model emits. No SDL and no ImGui, so a test drives it without a window.
// Every dialog request has a sequence number; a result is accepted only while it is the current one, so an answer
// that arrives after a drop or after the browser opened is ignored.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "file_browser.h"
#include "setup_screen_model.h"

namespace don_host
{

struct DialogResult
{
    bool failed = false;   // no dialog could be shown here (no desktop portal, no helper program): E02
    std::string path;      // the chosen path; "" = the player cancelled
    std::uint64_t seq = 0; // the request this answers (BrowserRouter::begin_dialog)
};

// One entry of the system chooser's file-type list. `pattern` is SDL's form: extensions without the dot, joined by ';',
// or "*" for every file.
struct DialogFilter
{
    std::string name;
    std::string pattern;
};
// The disc-image kinds the identify step accepts (disc_classify.h Kind), then "All files": a disc image can carry any
// name, so the player can still pick one the first entry does not list.
std::vector<DialogFilter> disc_dialog_filters();

// The one browser that can be open and the one dialog request that can still be answered.
class BrowserRouter
{
public:
    bool browser_active() const { return browser_.has_value(); }
    FileBrowserModel *browser() { return browser_ ? &*browser_ : nullptr; }
    // Start places for the next open; empty = the S0 folders under `home`.
    void set_start_places(std::vector<std::filesystem::path> places) { places_ = std::move(places); }
    void open_browser(const std::filesystem::path &home, const std::string &user);
    void close_browser() { browser_.reset(); }
    // One key or button for the open browser; the chosen path closes it. Back at the start list closes it.
    std::optional<std::string> navigate(don_setup::Nav nav);
    // A new request: every earlier one is stale from now on, and an answer not yet taken is dropped. Returns the number
    // the dialog must report back.
    std::uint64_t begin_dialog();
    bool is_current(std::uint64_t seq) const;
    // Anything that makes a pending dialog answer obsolete.
    void invalidate_dialog();
    // The dialog's callback (any thread): keeps the answer only if it is for the current request, so a late answer of an
    // older one can not replace the current one. take_dialog_result() hands it over once.
    void deliver_dialog(const DialogResult &result);
    std::optional<DialogResult> take_dialog_result();

private:
    std::optional<FileBrowserModel> browser_;
    std::vector<std::filesystem::path> places_;
    mutable std::mutex dialog_mutex_; // guards the sequence number and the answer
    std::uint64_t dialog_seq_ = 0;
    std::optional<DialogResult> dialog_result_;
};

// The system dialog answered: a stale one is dropped; a path (or "" = cancel) goes to the model; a failure makes the
// model ask for the in-app browser (an OpenBrowser action).
void route_dialog_result(don_setup::SetupModel &model, BrowserRouter &router, const DialogResult &result);
// A dropped file or folder: it ends the browser and any pending dialog, and goes to the model.
void route_drop(don_setup::SetupModel &model, BrowserRouter &router, const std::string &path);
// A key or button while the browser is open (`escape` = the Escape key, which closes it); returns true when it was
// taken by the browser. Otherwise the caller gives it to the model.
bool route_browser_nav(don_setup::SetupModel &model, BrowserRouter &router, don_setup::Nav nav, bool escape);
// Mouse: `row` is the visible row hovered (-1 = none), `click` activates it.
void route_browser_mouse(don_setup::SetupModel &model, BrowserRouter &router, int row, bool click);
// One action taken from the model. OpenFilePicker calls `show_dialog` with the new request number; OpenBrowser opens
// the browser. Returns true when the action was one of those two.
bool route_action(BrowserRouter &router, const don_setup::ActionRequest &request, const std::filesystem::path &home,
                  const std::string &user, const std::function<void(std::uint64_t)> &show_dialog);

} // namespace don_host
