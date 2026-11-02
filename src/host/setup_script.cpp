// SPDX-License-Identifier: GPL-3.0-or-later
// The DON_SETUP_SCRIPT test hook: see setup_script.h.
#include "setup_script.h"

#include <cstdio>
#include <sstream>

#ifndef DON_SETUP_RUN_NO_SDL
#include <SDL3/SDL.h>

#include "file_browser_view.h"
#include "setup_screen.h"
#endif

using namespace don_setup;

namespace don_host {

namespace {

bool nav_of_name(const std::string& k, Nav& nav) {
    if (k == "up") nav = Nav::Up;
    else if (k == "down") nav = Nav::Down;
    else if (k == "left") nav = Nav::Left;
    else if (k == "right") nav = Nav::Right;
    else if (k == "confirm") nav = Nav::Confirm;
    else if (k == "back") nav = Nav::Back;
    else return false;
    return true;
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

}  // namespace

const char* setup_screen_name(Screen s) {
    switch (s) {
        case Screen::Welcome: return "welcome";
        case Screen::Checking: return "checking";
        case Screen::Decision: return "decision";
        case Screen::Preparing: return "preparing";
        case Screen::Ready: return "ready";
        case Screen::Resume: return "resume";
        case Screen::Settings: return "settings";
    }
    return "screen";
}

std::vector<ScriptStep> parse_script(const char* text) {
    std::vector<ScriptStep> steps;
    std::stringstream ss(text ? text : "");
    for (std::string k; std::getline(ss, k, ',');) {
        const std::string whole = k;
        ScriptStep step;
        if (starts_with(k, "now:")) {
            step.now = true;
            k = k.substr(4);
        }
        if (starts_with(k, "drop:") && k.size() > 5) {
            step.kind = ScriptStep::Kind::Drop;
            step.path = k.substr(5);
        } else if (!nav_of_name(k, step.nav)) {
            std::fprintf(stderr, "[don] DON_SETUP_SCRIPT: unknown key '%s'\n", whole.c_str());
            continue;
        }
        steps.push_back(step);
    }
    return steps;
}

#ifndef DON_SETUP_RUN_NO_SDL
void apply_script_step(SetupModel& model, const ScriptStep& step) {
    if (step.kind == ScriptStep::Kind::Drop) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_EVENT_DROP_FILE;
        ev.drop.data = step.path.c_str();
        setup_screen_event(model, ev);
        return;
    }
    if (!route_browser_nav(model, file_browser_router(), step.nav, false)) model.navigate(step.nav);
}
#endif

}  // namespace don_host
