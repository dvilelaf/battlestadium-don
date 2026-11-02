#include "setup_screen_model.h"

#include <algorithm>
#include <cmath>

#include "../setup/error_values.h"
#include "../setup/estimate.h"
#include "../setup/errors.h"
#include "setup_flow_model.h"

namespace don_setup {

static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ---------------------------------------------------------------- model

SetupModel::SetupModel() { open_first_run(); }

void SetupModel::emit(Action a, std::string path) { actions_.push_back({a, std::move(path)}); }

std::vector<ActionRequest> SetupModel::take_actions() {
    std::vector<ActionRequest> out;
    out.swap(actions_);
    return out;
}

void SetupModel::enter(Screen s) {
    screen_ = s;
    reset_menu();
}

void SetupModel::reset_menu() {
    selected_ = 0;
    auto m = menu_items();
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i].primary) selected_ = static_cast<int>(i);
}

void SetupModel::open_first_run() {
    from_settings_ = false;
    updating_ = false;
    error_open_ = false;
    mode_ = PrepMode::Running;
    decisions_.clear();
    enter(Screen::Welcome);
}

void SetupModel::open_from_settings(const std::string& current_disc) {
    open_first_run();
    from_settings_ = true;
    current_disc_ = current_disc;
    reset_menu();
}

void SetupModel::open_settings(const SettingsInfo& s) {
    open_first_run();
    settings_ = s;
    enter(Screen::Settings);
}

void SetupModel::open_resume(int percent, double seconds_left) {
    open_first_run();
    resume_percent_ = std::min(std::max(percent, 0), 99);
    resume_left_ = seconds_left;
    enter(Screen::Resume);
}

void SetupModel::begin_update(const std::string& version) {
    open_first_run();
    updating_ = true;
    update_version_ = version;
    begin_prepare();
    actions_.back().path.clear();
}

void SetupModel::begin_prepare() {
    have_progress_ = false;
    start_percent_ = 0;
    shown_left_ = env_.estimate_seconds;
    mode_ = PrepMode::Running;
    enter(Screen::Preparing);
    emit(Action::StartPrepare, identity_.variant_id);
}

bool SetupModel::space_blocked() const { return !space_message().empty(); }

std::string SetupModel::space_message() const {
    if (env_.needed_bytes == 0 || env_.free_bytes >= env_.needed_bytes) return {};
    return "Not enough space: needs " + format_gb_ceil(env_.needed_bytes) + " GB on " + env_.drive + ", " +
           format_gb_floor(env_.free_bytes) + " GB free.";
}

void SetupModel::set_environment(const Environment& e) {
    env_ = e;
    if (screen_ == Screen::Preparing && !have_progress_ && start_percent_ == 0) shown_left_ = e.estimate_seconds;
}

void SetupModel::set_discovery(const std::vector<Candidate>& found) {
    found_.clear();
    for (const Candidate& c : found)   // an image that was not identified is never offered
        if (!c.edition.empty()) found_.push_back(c);
    offer_file_.clear();
    if (screen_ == Screen::Welcome && !error_open_) reset_menu();
}

void SetupModel::set_offer_check(const std::string& file, double fraction) {
    offer_file_ = file;
    offer_fraction_ = std::min(1.0, std::max(0.0, fraction));
    if (screen_ == Screen::Welcome && !error_open_) reset_menu();
}

void SetupModel::set_check_progress(double fraction, double seconds_left) {
    check_fraction_ = std::min(1.0, std::max(0.0, fraction));
    check_left_ = std::max(0.0, seconds_left);
}

void SetupModel::set_ready_info(bool identical_to_reference, const std::string& version) {
    ready_identical_ = identical_to_reference;
    ready_version_ = version;
}

// A good check goes straight on; only E42, E17, low space, battery and low RAM stop it (A2 rule 6).
void SetupModel::set_disc_identity(const DiscIdentity& id) {
    if (screen_ != Screen::Checking) return;
    identity_ = id;
    decisions_.clear();
    std::vector<std::pair<std::string, std::string>> facts = {
        {"name", base_name(chosen_path_)}, {"serial", id.detail}, {"file", id.detail},
        {"percent", id.detail},            {"reason", id.detail}, {"container", id.detail}};
    uint64_t have = 0, total = 0;
    if (parse_have_of_total(id.detail, have, total)) {
        facts.push_back({"have", format_gb(have)});
        facts.push_back({"total", format_gb(total)});
    }
    if (!id.known && id.error != ErrorId::E17) {
        set_error_id(id.error == ErrorId::None ? ErrorId::E15 : id.error, facts);
        return;
    }
    if (!env_.avx2) {
        set_error_id(ErrorId::E42);
        return;
    }
    if (id.error == ErrorId::E17) decisions_.push_back(ErrorId::E17);
    if (space_blocked()) decisions_.push_back(ErrorId::E30);
    if (env_.on_battery) decisions_.push_back(ErrorId::E39);
    if (env_.low_memory) decisions_.push_back(ErrorId::E36);
    next_decision();
}

void SetupModel::next_decision() {
    if (decisions_.empty()) {
        begin_prepare();
        return;
    }
    const std::vector<std::pair<std::string, std::string>> values = {
        {"gb", format_gb_ceil(env_.needed_bytes)}, {"name", base_name(chosen_path_)}};
    decision_ = panel_of(decisions_.front(), values);
    enter(Screen::Decision);
}

void SetupModel::set_progress(const Progress& p) {
    if (screen_ != Screen::Preparing) return;
    bool stage_changed = !have_progress_ || p.stage != shown_stage_;
    const double raw = p.seconds_left;  // negative: unknown, kept as unknown (T-718)
    if (raw < 0.0) {
        shown_left_ = don_setup::kSecondsLeftUnknown;
    } else if (stage_changed || shown_left_ < 0.0) {  // the first real value after unknown is taken as is
        shown_left_ = raw;
    } else {
        double limit = std::max(shown_left_, 1.0) * 1.2;
        shown_left_ = std::min(raw, limit);
    }
    shown_stage_ = p.stage;
    have_progress_ = true;
    prog_ = p;
    if (p.stage != PrepStage::Failed) last_stage_ = p.stage;

    if (p.stage == PrepStage::Done) {
        mode_ = PrepMode::Running;
        enter(Screen::Ready);
    } else if (p.stage == PrepStage::Failed) {
        // The table has no entry for "failed with no ErrorId"; E53 is the generic build failure.
        ErrorPanel e = panel_of(ErrorId::E53, {});
        if (!p.detail.empty()) e.why = sanitize_utf8(p.detail);
        set_error(e);
    }
}

void SetupModel::set_error_id(ErrorId id, const std::vector<std::pair<std::string, std::string>>& values) {
    if (*error_info(id).action == '\0') return;  // a silent entry: A4 says nothing is shown
    set_error(panel_of(id, values));
    error_id_ = id;
}

void SetupModel::set_error(const ErrorPanel& e) {
    error_ = e;
    error_id_ = ErrorId::None;
    if (error_.code.empty()) error_.code = "E-APP-1";
    if (error_.title.empty()) error_.title = "Something went wrong";
    if (error_.why.empty()) error_.why = "The program could not finish this step.";
    if (error_.todo.empty()) error_.todo = "Try again. If it fails again, copy the report.";
    if (error_.action.empty()) error_.action = "TRY AGAIN";
    error_origin_ = screen_;
    error_open_ = true;
    error_selected_ = 0;
}

void SetupModel::file_chosen(const std::string& path) {
    if (path.empty() || error_open_) return;
    if (screen_ != Screen::Welcome && screen_ != Screen::Decision && screen_ != Screen::Resume) return;
    offer_file_.clear();   // the player chose: the offer checks end (the controller stops them on IdentifyDisc)
    chosen_path_ = path;
    check_fraction_ = 0.0;
    check_left_ = 5.0;
    enter(Screen::Checking);
    emit(Action::IdentifyDisc, path);
}

void SetupModel::file_picker_unavailable() { emit(Action::OpenBrowser); }

void SetupModel::request_close() {
    if (screen_ == Screen::Preparing && !error_open_) {
        if (mode_ != PrepMode::CloseConfirm) {
            mode_ = PrepMode::CloseConfirm;
            reset_menu();
        }
        return;
    }
    emit(Action::Quit);
}

// ---------------------------------------------------------------- input

int SetupModel::first_enabled_from(int from, int dir) const {
    auto m = menu_items();
    int n = static_cast<int>(m.size());
    for (int k = 1; k <= n; ++k) {
        int i = ((from + dir * k) % n + n) % n;
        if (m[i].enabled) return i;
    }
    return from;
}

void SetupModel::select(int index) {
    auto m = menu_items();
    if (index >= 0 && index < static_cast<int>(m.size()) && m[index].enabled) {
        if (error_open_) error_selected_ = index;
        selected_ = index;
    }
}

void SetupModel::activate(int index) {
    auto m = menu_items();
    if (index < 0 || index >= static_cast<int>(m.size())) return;
    if (!m[index].enabled) return;
    activate_label(m[index].label);
}

void SetupModel::navigate(Nav n) {
    const int cur = error_open_ ? error_selected_ : selected_;
    switch (n) {
        case Nav::Up:
        case Nav::Down: {
            int next = first_enabled_from(cur, n == Nav::Down ? 1 : -1);
            selected_ = next;
            if (error_open_) error_selected_ = next;
            break;
        }
        case Nav::Left:
        case Nav::Right: {
            const int dir = n == Nav::Left ? -1 : 1;
            if (error_open_ || cur != 0) break;
            if (screen_ == Screen::Settings) cycle_variant(dir);
            break;
        }
        case Nav::Confirm: {
            auto m = menu_items();
            if (cur < 0 || cur >= static_cast<int>(m.size()) || !m[cur].enabled) return;  // the status line says why
            activate_label(m[cur].label);
            break;
        }
        case Nav::Back:
            go_back();
            break;
    }
}

void SetupModel::cycle_variant(int dir) {
    const int n = static_cast<int>(settings_.variants.size());
    if (n < 2) return;
    settings_.current = ((settings_.current + (dir ? dir : 1)) % n + n) % n;
    emit(Action::SwitchVariant, settings_.variants[settings_.current]);
}

void SetupModel::stop_preparing() {
    const int pct = have_progress_ ? percent_of(prog_) : start_percent_;
    const double left = have_progress_ ? shown_left_ : shown_left_;
    emit(Action::Cancel);
    mode_ = PrepMode::Running;
    if (updating_) {  // keep the version that is already installed
        updating_ = false;
        enter(Screen::Welcome);
        emit(Action::Play);
    } else {
        open_resume(pct, left);
    }
}

void SetupModel::activate_label(const std::string& l) {
    if (error_open_) {
        if (l == "COPY REPORT") emit(Action::CopyReport);
        else error_primary();
        return;
    }
    if (l == kUseOffer) {
        if (!found_.empty()) file_chosen(found_.front().path);
    } else if (starts_with(l, "CHOOSE") || l == "START OVER WITH ANOTHER DISC" || l == "ADD ANOTHER DISC…") {
        if (screen_ == Screen::Settings) open_from_settings(settings_.variants.empty() ? "" : settings_.variants[settings_.current]);
        else if (screen_ != Screen::Welcome) enter(Screen::Welcome);
        decisions_.clear();
        emit(Action::OpenFilePicker);
    } else if (l == "READ FROM DISC DRIVE") {
        emit(Action::ReadDiscDrive);
    } else if (l == "QUIT") {
        emit(Action::Quit);
    } else if (l == "BACK") {
        emit(Action::ReturnToSettings);
    } else if (l == "CANCEL") {
        emit(Action::CancelIdentify);
        enter(Screen::Welcome);
    } else if (screen_ == Screen::Decision && l == decision_.action) {
        decisions_.erase(decisions_.begin());
        next_decision();
    } else if (starts_with(l, "CONTINUE PREPARING")) {
        begin_prepare();
        start_percent_ = resume_percent_;
        shown_left_ = resume_left_;
    } else if (l == "STOP — CONTINUE LATER") {
        stop_preparing();
    } else if (l == "WRONG DISC? CHOOSE ANOTHER") {
        emit(Action::Cancel);
        enter(Screen::Welcome);
        emit(Action::OpenFilePicker);
    } else if (l == "KEEP PREPARING") {
        mode_ = PrepMode::Running;
        reset_menu();
    } else if (l == "STOP AND CLOSE") {
        emit(Action::Cancel);
        emit(Action::Quit);
    } else if (l == "PLAY") {
        emit(Action::Play);
    } else if (l == "GAME VERSION ◂▸") {
        cycle_variant(0);
    } else if (l == "FREE UP SPACE") {
        emit(Action::FreeUpSpace);
    } else if (l == "REMOVE GAME DATA") {
        emit(Action::RemoveGameData);
    } else if (l == "REPAIR") {
        emit(Action::Repair);
    }
}

void SetupModel::go_back() {
    if (error_open_) {
        dismiss_error();
        return;
    }
    switch (screen_) {
        case Screen::Welcome:
            emit(from_settings_ ? Action::ReturnToSettings : Action::Quit);
            break;
        case Screen::Checking:
            emit(Action::CancelIdentify);
            enter(Screen::Welcome);
            break;
        case Screen::Decision:
            decisions_.clear();
            enter(Screen::Welcome);
            break;
        case Screen::Preparing:
            if (mode_ == PrepMode::CloseConfirm) activate_label("KEEP PREPARING");  // one step back
            else request_close();
            break;
        case Screen::Ready:
        case Screen::Resume:
        case Screen::Settings:
            break;  // nothing to go back to: the choice is on the screen
    }
}


}  // namespace don_setup
