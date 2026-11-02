// First-run setup flow (A3 in docs/tasks/M6-public-release.md): states and transitions only. No SDL, no drawing,
// no file system. A view asks for view() every frame and draws it; the app feeds results in with the set_* functions
// and performs the actions it takes out of take_actions(). The texts live in setup_flow_model.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../setup/setup_types.h"

namespace don_setup {

// The S1 button that uses the identified disc image; the body line above it names the image.
constexpr const char* kUseOffer = "USE THIS DISC";

// Welcome = S1, Checking = S2, Decision = the panel that stops only when one is needed, Preparing = S3 (and S7),
// Ready = Done, Resume = the next start after a stop, Settings = the S6 rows.
enum class Screen { Welcome, Checking, Decision, Preparing, Ready, Resume, Settings };
enum class Nav { Up, Down, Left, Right, Confirm, Back };
using PrepStage = Stage;
enum class PrepMode { Running, CloseConfirm };  // CloseConfirm: "Stop preparing? Your progress is saved."

enum class Action {
    OpenFilePicker,
    OpenBrowser,       // the in-app browser (only when the system dialog is missing)
    ReadDiscDrive,
    IdentifyDisc,      // ActionRequest::path
    StartPrepare,      // path = the variant id to build (empty = the one identified); also continues a stopped run
    Resume,            // after a wait the app caused (memory, disk full)
    Cancel,            // stop preparing; the progress stays saved
    Play,
    Quit,
    CopyReport,
    CancelIdentify,
    ReturnToSettings,  // setup was opened from Settings and the user stepped back out of it
    SwitchVariant,     // path = the variant's description; instant
    FreeUpSpace,
    RemoveGameData,    // never touches memory cards
    Repair,
};

struct ActionRequest {
    Action action;
    std::string path;
};

struct Environment {
    uint64_t free_bytes = 0;
    uint64_t needed_bytes = 0;  // 0 = unknown, never blocks; the measured peak, not a round guess
    std::string drive;          // "/home", "C:"
    int cores = 0;
    uint64_t ram_bytes = 0;
    bool avx2 = true;
    bool on_battery = false;   // E39
    bool low_memory = false;   // E36: the app decides what counts as low
    bool disc_present = false; // READ FROM DISC DRIVE is offered only then
    double estimate_seconds = 0.0;      // 0 = not measured yet
    double estimate_uncertainty = 0.0;  // fraction, e.g. 0.3 means +-30 %; >= 0.25 shows a range
};

// One image of the S0 discovery. Before the identification `edition` is empty; a disc image is offered (set_discovery)
// only with the edition name its identification returned.
struct Candidate {
    std::string path;
    std::string edition;
};

struct SettingsInfo {
    std::vector<std::string> variants;  // installed variants, by description
    int current = 0;
    uint64_t used_bytes = 0;
};

enum class Mark { None, Done, Active, Pending };

struct Row {
    Mark mark = Mark::None;
    std::string text;
    std::string detail;  // right-hand side / panel value
};

struct MenuItem {
    std::string label;
    bool primary = false;  // exactly one per menu, preselected when the menu opens
    bool enabled = true;
    std::string value;  // stepped items (SPEED, GAME VERSION): the current value, drawn between the triangles
};

// S5 and the decision panel: code, what happened, why, the action line, and the primary action.
struct ErrorPanel {
    std::string code;
    std::string title;
    std::string why;
    std::string todo;
    std::string action;
};

struct View {
    Screen screen = Screen::Welcome;
    std::string title;
    std::string window_title;
    std::vector<std::string> body;
    std::vector<std::string> notes;  // short reassurance lines under the body
    std::string link;                // "How do I make one from my disc?"
    std::string banner;              // the one-time "Your game is ready" banner
    std::vector<Row> steps;          // left rail
    std::string panel_title;
    std::vector<Row> panel;          // right panel / stage list / settings rows
    std::vector<MenuItem> menu;
    int selected = 0;
    std::string hint;
    std::string status;
    std::string time_text;  // "" when the state does not wait
    int percent = -1;       // -1 = no number; otherwise 0..100
    double bar = -1.0;      // -1 = no bar
    bool waiting = false;
    bool error_overlay = false;
    ErrorPanel error;       // the error overlay, or the decision on Screen::Decision
    PrepMode mode = PrepMode::Running;
};

// Time helpers (pure; used by the model and tested directly). The time left of a wait comes from format_eta.
std::string time_about(double seconds);                      // "about 3 min" / "less than a minute"
std::string time_range(double seconds, double uncertainty);  // "about 3-4 min" when uncertain
std::string format_gb_ceil(uint64_t bytes);                  // "3.2"
std::string format_gb_floor(uint64_t bytes);                 // "1.1"
std::string with_thousands(int n);                           // "1,876"

class SetupModel {
public:
    SetupModel();

    // --- entry points ---
    void open_first_run();                                     // S1
    void open_from_settings(const std::string& current_disc);  // S6 -> S1, nothing is erased
    void open_settings(const SettingsInfo& s);                 // S6
    void open_resume(int percent, double seconds_left);        // next start after a stop
    void begin_update(const std::string& version);             // S7, emits StartPrepare

    // --- inputs ---
    void navigate(Nav n);
    void select(int index);                     // mouse hover
    void activate(int index);                   // mouse click
    void file_chosen(const std::string& path);  // picker result or drop; "" = picker cancelled
    void file_picker_unavailable();             // E02: the in-app browser opens instead
    void request_close();                       // the window's close button

    // --- results pushed by the app ---
    void set_environment(const Environment& e);
    void set_discovery(const std::vector<Candidate>& found);  // the identified images (edition set); may be empty
    void set_offer_check(const std::string& file, double fraction);  // S1 checks `file` for the offer; "" = done
    void set_disc_identity(const DiscIdentity& id);
    void set_check_progress(double fraction, double seconds_left);
    void set_progress(const Progress& p);
    void set_error_id(ErrorId id, const std::vector<std::pair<std::string, std::string>>& values = {});
    void set_error(const ErrorPanel& e);  // S5 over any state, for a message the table does not have
    void set_ready_info(bool identical_to_reference, const std::string& version);

    // --- outputs ---
    View view() const;
    std::vector<ActionRequest> take_actions();
    Screen screen() const { return screen_; }
    PrepMode mode() const { return mode_; }
    bool has_error() const { return error_open_; }
    bool space_blocked() const;
    std::string space_message() const;  // "" when there is enough room
    double shown_seconds_left() const { return shown_left_; }

private:
    std::vector<MenuItem> menu_items() const;
    void activate_label(const std::string& label);
    void enter(Screen s);
    void emit(Action a, std::string path = {});
    void go_back();
    void dismiss_error();
    void error_primary();
    void begin_prepare();
    void next_decision();
    void stop_preparing();
    void cycle_variant(int dir);
    void reset_menu();
    int first_enabled_from(int from, int dir) const;
    std::string use_label() const;
    ErrorPanel panel_of(ErrorId id, const std::vector<std::pair<std::string, std::string>>& values) const;

    Screen screen_ = Screen::Welcome;
    PrepMode mode_ = PrepMode::Running;
    PrepStage last_stage_ = PrepStage::Copy;
    int selected_ = 0;
    Environment env_;
    DiscIdentity identity_;
    Progress prog_;
    bool have_progress_ = false;
    double shown_left_ = 0.0;
    PrepStage shown_stage_ = PrepStage::Copy;
    double check_fraction_ = 0.0;
    double check_left_ = 5.0;
    std::string chosen_path_;
    std::string current_disc_;
    std::string update_version_;
    std::vector<Candidate> found_;
    std::string offer_file_;       // the image S1 is identifying for the offer; "" when none
    double offer_fraction_ = 0.0;
    bool from_settings_ = false;
    bool updating_ = false;
    SettingsInfo settings_;
    int resume_percent_ = 0;
    int start_percent_ = 0;  // shown until the first progress arrives (0, or where a stopped run continues)
    double resume_left_ = 0.0;
    bool ready_identical_ = false;
    std::string ready_version_;
    std::vector<ErrorId> decisions_;  // still to ask, first on top
    ErrorPanel decision_;
    bool error_open_ = false;
    ErrorPanel error_;
    ErrorId error_id_ = ErrorId::None;
    int error_selected_ = 0;
    Screen error_origin_ = Screen::Welcome;
    std::vector<ActionRequest> actions_;
};

}  // namespace don_setup
