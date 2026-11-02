// The text the setup screen shows for every state (A3 in docs/tasks/M6-public-release.md): menus and view().
// No SDL, no drawing.
#include "setup_flow_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../setup/errors.h"
#include "../setup/estimate.h"

namespace don_setup {

static const char* const kReplacement = "\xEF\xBF\xBD";  // U+FFFD

// Length of the well-formed UTF-8 sequence at s[i] (RFC 3629: no overlongs, no surrogates, at most U+10FFFF), or 0.
static size_t utf8_length(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned c = b(i);
    auto cont = [&](size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(i + 1) || !cont(i + 2)) return 0;
        if (c == 0xE0 && b(i + 1) < 0xA0) return 0;
        if (c == 0xED && b(i + 1) > 0x9F) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
        if (c == 0xF0 && b(i + 1) < 0x90) return 0;
        if (c == 0xF4 && b(i + 1) > 0x8F) return 0;
        return 4;
    }
    return 0;
}

std::string sanitize_utf8(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8_length(s, i);
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (n == 0 || c < 0x20 || c == 0x7F) {
            out += kReplacement;
            i += n ? n : 1;
        } else {
            out.append(s, i, n);
            i += n;
        }
    }
    return out;
}

std::string base_name(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return sanitize_utf8(i == std::string::npos ? p : p.substr(i + 1));
}

int percent_of(const Progress& p) {
    double f = std::min(1.0, std::max(0.0, p.fraction));
    int v = static_cast<int>(std::floor(f * 100.0 + 1e-9));
    if (p.stage == PrepStage::Done) return 100;
    return std::min(v, 99);
}

std::string fill_placeholders(std::string text, const std::vector<std::pair<std::string, std::string>>& values) {
    for (const auto& kv : values) {
        const std::string key = "{" + kv.first + "}";
        for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + kv.second.size()))
            text.replace(at, key.size(), kv.second);
    }
    return text;
}

// ---------------------------------------------------------------- helpers

// Pre-start estimate ("about 3-4 min"): format_eta only words a running countdown ("About N min left"), so these stay.
static long ceil_div(double v, double d) { return static_cast<long>(std::ceil(v / d - 1e-9)); }

std::string time_about(double seconds) {
    if (seconds <= 60.0) return "less than a minute";
    return "about " + std::to_string(ceil_div(seconds, 60.0)) + " min";
}

std::string time_range(double seconds, double uncertainty) {
    if (seconds <= 60.0 || uncertainty < 0.25) return time_about(seconds);
    long lo = ceil_div(seconds * (1.0 - uncertainty), 60.0);
    long hi = ceil_div(seconds * (1.0 + uncertainty), 60.0);
    if (lo < 1) lo = 1;
    if (hi <= lo) return "about " + std::to_string(lo) + " min";
    return "about " + std::to_string(lo) + "-" + std::to_string(hi) + " min";
}

static std::string tenths(uint64_t t) { return std::to_string(t / 10) + "." + std::to_string(t % 10); }

std::string format_gb_ceil(uint64_t bytes) { return tenths((bytes + 99999999ull) / 100000000ull); }
std::string format_gb_floor(uint64_t bytes) { return tenths(bytes / 100000000ull); }

std::string with_thousands(int n) {
    std::string s = std::to_string(n < 0 ? -n : n);
    std::string out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++c;
    }
    return (n < 0 ? "-" : "") + out;
}

// ---------------------------------------------------------------- menus

static std::string estimate_text(const Environment& e) {
    return e.estimate_seconds > 0 ? time_range(e.estimate_seconds, e.estimate_uncertainty) : "estimating…";
}

std::string SetupModel::use_label() const { return found_.empty() ? std::string() : std::string(kUseOffer); }

std::vector<MenuItem> SetupModel::menu_items() const {
    if (error_open_) return {{error_.action, true, true}, {"COPY REPORT", false, true}};
    const int nv = static_cast<int>(settings_.variants.size());
    const std::string variant = nv ? settings_.variants[std::min(std::max(settings_.current, 0), nv - 1)] : "";
    switch (screen_) {
        case Screen::Welcome: {
            std::vector<MenuItem> m;
            if (!found_.empty()) {
                m.push_back({use_label(), true, true});
                m.push_back({"CHOOSE DISC IMAGE…", false, true});
            } else {
                m.push_back({"CHOOSE DISC IMAGE…", true, true});
            }
            if (env_.disc_present) m.push_back({"READ FROM DISC DRIVE", false, true});
            m.push_back({from_settings_ ? "BACK" : "QUIT", false, true});
            return m;
        }
        case Screen::Checking:
            return {{"CANCEL", true, true}};
        case Screen::Decision:
            return {{decision_.action, true, !(decisions_.front() == ErrorId::E30 && space_blocked())},
                    {"CHOOSE ANOTHER DISC", false, true}};
        case Screen::Preparing:
            if (mode_ == PrepMode::CloseConfirm) return {{"KEEP PREPARING", true, true}, {"STOP AND CLOSE", false, true}};
            // No SPEED item: the module build reads its job cap once, when it starts (src/setup/module_build.cpp), so a
            // change made while preparing could not apply. A control that does nothing is not shown.
            if (updating_) return {{"STOP — CONTINUE LATER", true, true}};
            return {{"STOP — CONTINUE LATER", true, true}, {"WRONG DISC? CHOOSE ANOTHER", false, true}};
        case Screen::Ready:
            return {{"PLAY", true, true}};
        case Screen::Resume:
            return {{"CONTINUE PREPARING · " + format_eta(resume_left_, true), true, true},
                    {"START OVER WITH ANOTHER DISC", false, true}};
        case Screen::Settings:
            return {{"GAME VERSION ◂▸", true, !settings_.variants.empty(), variant},
                    {"ADD ANOTHER DISC…", false, true},
                    {"FREE UP SPACE", false, true},
                    {"REMOVE GAME DATA", false, true},
                    {"REPAIR", false, true}};
    }
    return {};
}

ErrorPanel SetupModel::panel_of(ErrorId id,
                                const std::vector<std::pair<std::string, std::string>>& values) const {
    const ErrorInfo& info = error_info(id);
    ErrorPanel p;
    p.code = info.code;
    p.title = fill_placeholders(info.title, values);
    p.why = fill_placeholders(info.why, values);
    p.todo = *info.what_to_do ? fill_placeholders(info.what_to_do, values) : "";
    p.action = info.action;
    return p;
}

void SetupModel::dismiss_error() {
    error_open_ = false;
    if (error_origin_ == Screen::Checking) {
        emit(Action::CancelIdentify);
        enter(Screen::Welcome);
    } else if (error_origin_ == Screen::Preparing && have_progress_ && prog_.stage == PrepStage::Failed) {
        mode_ = PrepMode::Running;
        enter(Screen::Welcome);
    }
}

// The error's primary action (A3 S5): CHOOSE ANOTHER DISC / CONTINUE / PREPARE AGAIN / TRY AGAIN, and the few more
// the table uses.
void SetupModel::error_primary() {
    const std::string a = error_.action;
    const Screen origin = error_origin_;
    error_open_ = false;
    if (a == "TRY AGAIN" || a == "PREPARE AGAIN") {
        if (origin == Screen::Preparing) {
            begin_prepare();
        } else if (origin == Screen::Checking && !chosen_path_.empty()) {
            enter(Screen::Welcome);
            file_chosen(chosen_path_);
        } else {
            enter(Screen::Welcome);
            emit(Action::OpenFilePicker);
        }
    } else if (a == "CHOOSE ANOTHER DISC") {
        decisions_.clear();
        enter(Screen::Welcome);
        emit(Action::OpenFilePicker);
    } else if (a == "CONTINUE" || a == "KEEP PREPARING") {
        if (origin == Screen::Preparing) emit(Action::Resume);
    } else if (a == "QUIT") {
        emit(Action::Quit);
    } else if (a == "PLAY") {
        emit(Action::Play);
    } else if (a == "REPAIR") {
        emit(Action::Repair);
    }  // "OK": the panel closes, nothing else
}

// ---------------------------------------------------------------- view

static void rail(View& v, int active, int done_count) {
    const char* names[3] = {"1 Your disc", "2 Prepare", "3 Play"};
    for (int i = 0; i < 3; ++i) {
        Row r;
        r.text = names[i];
        r.mark = i < done_count ? Mark::Done : (i == active ? Mark::Active : Mark::Pending);
        v.steps.push_back(r);
    }
}

static const char* const kAppName = "Battle Stadium D.O.N";

View SetupModel::view() const {
    View v;
    v.screen = screen_;
    v.mode = mode_;
    v.menu = menu_items();
    v.selected = error_open_ ? error_selected_ : selected_;
    v.window_title = kAppName;

    switch (screen_) {
        case Screen::Welcome: {
            rail(v, 0, 0);
            v.title = std::string("Set up ") + kAppName;
            v.body.push_back("Choose the disc image of your copy of the game.");
            if (from_settings_ && !current_disc_.empty())
                v.body.push_back("Current disc: " + sanitize_utf8(current_disc_));
            if (!found_.empty()) {   // identified: the line names what the first button uses
                v.body.push_back(std::string(kAppName) + " · " + sanitize_utf8(found_.front().edition) + " · " +
                                 base_name(found_.front().path));
            } else if (!offer_file_.empty()) {
                v.body.push_back("Checking " + base_name(offer_file_) + "…");
                v.bar = offer_fraction_;
            }
            if (space_blocked()) v.status = space_message();
            break;
        }
        case Screen::Checking:
            rail(v, 0, 0);
            v.title = "Checking your disc…";
            v.body.push_back(base_name(chosen_path_));
            v.bar = check_fraction_;
            v.waiting = true;
            v.time_text = "a few seconds";
            break;
        case Screen::Decision:
            rail(v, 0, 0);
            v.title = decision_.title;
            v.body.push_back(decision_.why);
            if (!decision_.todo.empty()) v.body.push_back(decision_.todo);
            v.error = decision_;
            break;
        case Screen::Preparing: {
            rail(v, 1, 1);
            v.waiting = true;
            const double left = have_progress_ ? shown_left_ : (shown_left_ > 0 ? shown_left_ : -1.0);
            v.percent = have_progress_ ? percent_of(prog_) : start_percent_;
            v.bar = v.percent / 100.0;
            v.time_text = format_eta(left);
            v.window_title = std::to_string(v.percent) + " % · Preparing — " + kAppName;
            if (mode_ == PrepMode::CloseConfirm) {
                v.title = "Stop preparing? Your progress is saved.";
            } else if (updating_) {
                v.title = "Updating your game";
                v.body.push_back("Version " + update_version_ + " · " + estimate_text(env_));
            } else {
                v.title = identity_.description.empty() ? "Preparing your game"
                                                        : "✓ " + identity_.description + " · checked";
            }
            v.panel_title = "Steps";
            int cur = static_cast<int>(last_stage_);
            if (have_progress_ && prog_.stage == PrepStage::Done) cur = 4;
            const char* names[4] = {"Copy game files", "Translate the game's code", "Build for this PC", "Final check"};
            for (int i = 0; i < 4; ++i) {
                Row r;
                r.text = names[i];
                r.mark = i < cur ? Mark::Done : (i == cur ? Mark::Active : Mark::Pending);
                if (i == 2 && cur >= 2 && have_progress_ && prog_.parts_total > 0)
                    r.detail = with_thousands(prog_.parts_done) + "/" + with_thousands(prog_.parts_total);
                v.panel.push_back(r);
            }
            v.status = have_progress_ ? sanitize_utf8(prog_.detail) : "";
            v.hint = "You can close this window; preparing continues next time.";
            break;
        }
        case Screen::Ready:
            rail(v, 2, 2);
            v.title = "Your game is ready.";
            v.body.push_back("You can delete the disc image now.");
            break;
        case Screen::Resume:
            rail(v, 1, 1);
            v.title = "Preparing was stopped at " + std::to_string(resume_percent_) + " %.";
            v.percent = resume_percent_;
            v.bar = resume_percent_ / 100.0;
            break;
        case Screen::Settings: {
            v.title = "Game";
            const int n = static_cast<int>(settings_.variants.size());
            v.panel.push_back({Mark::None, "GAME VERSION ◂▸",
                               n ? settings_.variants[std::min(std::max(settings_.current, 0), n - 1)] : ""});
            v.panel.push_back({Mark::None, "STORAGE", format_gb_ceil(settings_.used_bytes) + " GB used"});
            break;
        }
    }

    if (error_open_) {
        // The card is the whole screen: no text of the screen behind it, so no line can run under or over the card.
        View c;
        c.screen = v.screen;
        c.mode = v.mode;
        c.menu = v.menu;
        c.selected = v.selected;
        c.window_title = v.window_title;
        c.percent = v.percent;
        c.error_overlay = true;
        c.error = error_;
        return c;
    }
    return v;
}

}  // namespace don_setup
