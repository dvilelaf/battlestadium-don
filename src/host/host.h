// SPDX-License-Identifier: GPL-3.0-or-later
// The host program of the port (T-110): SDL3 window, the PCSX2 GS (libdon_gs) connected to the PS2Recomp
// runtime, options, screenshots. A library plus a thin main (main.cpp); nothing in it refers to generated code.
//
// Layout of src/host:
//   host.h / host.cpp      don_host_run(): the order of calls of docs/knowledge/13-runtime-host.md
//   options.h / .cpp       the command line
//   window.h / .cpp        the SDL3 window;  window_native.cpp: native handles (the per-platform file)
//   gs_glue.h / .cpp       loads libdon_gs at run time; the Ps2HostGs that feeds it (game thread)
//   pad_table.h / .cpp     pad states shared between the main thread and the game thread (T-111 fills it)
//   screenshots.h / .cpp   PNGs of the GS picture
//   main.cpp               the real executable's main (the real game's setup)
//
// Threads: the main thread owns the window and the SDL events and runs the loop of don_host_run; the
// runtime's game thread owns the GS library (opens it in thread_begin, closes it in thread_end) and runs every
// GS callback, the pad read callback and the screenshots.
#pragma once

#include "options.h"

class PS2Runtime;

struct DonHostGame
{
    // Window title and the name in messages.
    const char *title = "Battle Stadium D.O.N";

    // true: the command line must give an ELF and the host loads it with PS2Runtime::loadELF (the game's
    // own overrides register themselves and are applied by the runtime). false: no ELF; `setup` prepares
    // the guest (a test registers host functions and sets the program counter).
    bool needs_elf = true;

    // Optional, main thread, after the runtime is initialised and the host GS and pad are installed, and
    // after the ELF is loaded when needs_elf. Return false to end the program with exit code 4.
    bool (*setup)(PS2Runtime &runtime, const DonHostOptions &options, void *user) = nullptr;
    void *user = nullptr;

    // T-720: the first-run setup ended with PLAY in this process, so the start menu does not ask PLAY again.
    bool skip_start_screen = false;
};

// Runs the program and returns the process exit code: 0 normal end or end of --seconds, 2 argument error,
// 3 the GS library, the window or the GS could not be opened, 4 the runtime ended with an error.
int don_host_run(int argc, char **argv, const DonHostGame &game);
