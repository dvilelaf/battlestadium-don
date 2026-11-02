// SPDX-License-Identifier: GPL-3.0-or-later
// `--setup-demo`: see setup_demo.cpp.
#pragma once

#include <string>

// Runs the setup screen against a fake engine and writes one PNG per state into shots_dir. Returns the exit code.
int don_setup_demo(const std::string &shots_dir);
