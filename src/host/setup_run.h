// SPDX-License-Identifier: GPL-3.0-or-later
// First run (T-649): `don`, built without linked game code, finds nothing prepared and shows the setup screens (A3 in
// docs/tasks/M6-public-release.md: S1 welcome/discovery -> S2 checking -> decision -> S3 preparing -> ready). The
// screens run the real preparation (prepare_driver.h, the code `don --prepare` runs) on a worker thread and read its
// progress every frame, so the window never freezes; PLAY ends the screens and the caller loads the prepared module and
// starts the game in the same process.
//
// Closing the window while preparing keeps the journal (<data>/build/tmp/front/): the next start offers to continue.
//
// Test hooks for headless runs, both off by default and free when off:
//   DON_SETUP_SCRIPT=confirm,confirm   navigation keys (up, down, left, right, confirm, back) fed one at a time. A key
//                                      is fed once the screen has stayed the same for DON_SETUP_SCRIPT_FRAMES frames
//                                      (default 180) and nothing is running: not while the folders are searched, the
//                                      disc is checked or the game is prepared, so "confirm,confirm" = use the found
//                                      disc image, then PLAY once the game is ready.
//   DON_SETUP_SHOTS=<dir>              one PNG per screen change (NN-<screen>.png), saved after the frame is drawn.
//   DON_SETUP_QUIT_AT=<percent>        closes the window (as the player does) once the preparing bar reaches this percent.
//   DON_TRACE_SETUP=1                  "[don] setup progress <percent> <stage>" at most once a second while preparing.
// The window is the demo's size (DON_START_SCREEN_SIZE=WxH).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "prepare_driver.h"
#include "setup_screen_model.h"

namespace don_host {

// The world the screens act on. The real one is in setup_run.cpp; the tests drive the controller with a fake.
class SetupBackend {
public:
    virtual ~SetupBackend() = default;
    virtual don_setup::Environment environment() = 0;
    virtual void begin_discovery() = 0;
    // True once, when the search ended; `found` then holds its results (maybe none).
    virtual bool discovery_ready(std::vector<don_setup::Candidate>& found) = 0;
    // A stopped run left in the user folder: the image it works on and how far it got (percent); false when none.
    virtual bool resumable(std::string& image, int& percent) = 0;
    // The identification of a found image, in the background and apart from identify()/prepare(): the S1 offer. Poll
    // returns true once, when it ended (`id` is the result); until then `fraction` (0..1) says how far it got.
    virtual void begin_offer_check(const std::string& image) = 0;
    virtual bool offer_check_poll(don_setup::DiscIdentity& id, double& fraction) = 0;
    virtual void identify(const std::string& image) = 0;
    virtual void prepare(const std::string& image) = 0;
    virtual void stop() = 0;
    virtual AsyncSnapshot poll() = 0;
};

enum class SetupEnd { Running, Play, Quit };

// Connects a SetupModel to a backend: once per frame step() feeds the model what the backend learned and carries out the
// actions the model took. Actions of the screen itself (the file dialog, the in-app browser) go to `ui_action`.
class SetupController {
public:
    SetupController(don_setup::SetupModel& model, SetupBackend& backend,
                    std::function<bool(const don_setup::ActionRequest&)> ui_action);
    void start();
    SetupEnd step();
    bool busy() const { return busy_; }   // searching, checking or preparing: nothing for a player to press

private:
    void feed(const AsyncSnapshot& s);
    void act(const don_setup::ActionRequest& a);

    don_setup::SetupModel& model_;
    SetupBackend& backend_;
    std::function<bool(const don_setup::ActionRequest&)> ui_action_;
    std::string image_;
    void next_offer_check();
    void end_offer_checks(bool publish);

    bool searching_ = false;
    std::vector<std::string> offer_queue_;   // found images not yet identified, in order
    size_t offer_next_ = 0;                  // the next one of offer_queue_ to check
    bool offer_running_ = false;             // an offer check is under way
    bool offers_abandoned_ = false;          // the player chose a file: the search no longer offers anything
    std::vector<don_setup::Candidate> identified_;   // what the offer checks recognised so far
    bool consumed_ = true;   // the result of the last background job has been given to the model
    bool busy_ = false;
    SetupEnd end_ = SetupEnd::Running;
};

// The screens in their own window until PLAY or QUIT. Needs the install layout (prepare_cli.h).
SetupEnd setup_run(const InstallLayout& layout);

}  // namespace don_host
