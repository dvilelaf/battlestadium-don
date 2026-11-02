// SPDX-License-Identifier: GPL-3.0-or-later
// The fake folders and the fake sysfs tree of `--setup-demo` (see setup_demo.cpp). Placeholder files only.
#pragma once

#include "../setup/platform/machine.h"

// Builds the in-app browser's fake disk under /tmp and sets the browser's start places to it.
namespace don_host {
void setup_demo_make_browser_tree();
}

// A laptop on battery: reads a discharging battery and a mains supply that is not online, from a fake /sys tree.
don_setup::PowerState setup_demo_fake_battery_state();
