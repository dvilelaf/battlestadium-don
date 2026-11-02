// SPDX-License-Identifier: GPL-3.0-or-later
// See setup_browser_routing.h.
#include "setup_browser_routing.h"

namespace don_host
{
namespace
{
constexpr const char *kPlaces = "Places"; // BrowserView::title of the start list (file_browser.h)
} // namespace

std::vector<DialogFilter> disc_dialog_filters()
{
    return {{"Disc images", "iso;gz;zip;chd;cso;zso;7z;rar;ecm;nrg;mdf;cue"}, {"All files", "*"}};
}

void BrowserRouter::open_browser(const std::filesystem::path &home, const std::string &user)
{
    invalidate_dialog(); // the player is in the browser now: a late dialog answer must not act
    if (!places_.empty())
        browser_.emplace(places_);
    else
        browser_.emplace(FileBrowserModel::from_home(home, user));
}

std::uint64_t BrowserRouter::begin_dialog()
{
    std::lock_guard<std::mutex> lock(dialog_mutex_);
    dialog_result_.reset();
    return ++dialog_seq_;
}

bool BrowserRouter::is_current(std::uint64_t seq) const
{
    std::lock_guard<std::mutex> lock(dialog_mutex_);
    return seq == dialog_seq_;
}

void BrowserRouter::invalidate_dialog()
{
    std::lock_guard<std::mutex> lock(dialog_mutex_);
    ++dialog_seq_;
    dialog_result_.reset();
}

void BrowserRouter::deliver_dialog(const DialogResult &result)
{
    std::lock_guard<std::mutex> lock(dialog_mutex_);
    if (result.seq == dialog_seq_)
        dialog_result_ = result;
}

std::optional<DialogResult> BrowserRouter::take_dialog_result()
{
    std::lock_guard<std::mutex> lock(dialog_mutex_);
    auto r = std::move(dialog_result_);
    dialog_result_.reset();
    return r;
}

std::optional<std::string> BrowserRouter::navigate(don_setup::Nav nav)
{
    if (!browser_)
        return std::nullopt;
    if (nav == don_setup::Nav::Back && browser_->view().title == kPlaces)
    {
        browser_.reset();
        return std::nullopt;
    }
    browser_->navigate(nav);
    if (auto c = browser_->take_choice())
    {
        browser_.reset();
        return c;
    }
    return std::nullopt;
}

void route_dialog_result(don_setup::SetupModel &model, BrowserRouter &router, const DialogResult &result)
{
    if (!router.is_current(result.seq))
        return;
    router.invalidate_dialog(); // answered once
    if (result.failed)
        model.file_picker_unavailable();
    else
        model.file_chosen(result.path);
}

void route_drop(don_setup::SetupModel &model, BrowserRouter &router, const std::string &path)
{
    // A dropped file or folder is a choice like any other (SDL reports a folder as a drop too); it ends the browser.
    router.close_browser();
    router.invalidate_dialog();
    model.file_chosen(path);
}

bool route_browser_nav(don_setup::SetupModel &model, BrowserRouter &router, don_setup::Nav nav, bool escape)
{
    if (!router.browser_active())
        return false;
    if (escape)
        router.close_browser();
    else if (const auto chosen = router.navigate(nav))
        model.file_chosen(*chosen);
    return true;
}

void route_browser_mouse(don_setup::SetupModel &model, BrowserRouter &router, int row, bool click)
{
    FileBrowserModel *b = router.browser();
    if (!b || row < 0)
        return;
    const BrowserView v = b->view();
    if (static_cast<std::size_t>(row) >= v.rows.size())
        return;
    b->select_visible(static_cast<std::size_t>(row)); // the window stays where it is
    if (click)
        route_browser_nav(model, router, don_setup::Nav::Confirm, false);
}

bool route_action(BrowserRouter &router, const don_setup::ActionRequest &request, const std::filesystem::path &home,
                  const std::string &user, const std::function<void(std::uint64_t)> &show_dialog)
{
    switch (request.action)
    {
    case don_setup::Action::OpenFilePicker: show_dialog(router.begin_dialog()); return true;
    case don_setup::Action::OpenBrowser: router.open_browser(home, user); return true;
    default: return false;
    }
}

} // namespace don_host
