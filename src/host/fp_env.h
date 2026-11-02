// SPDX-License-Identifier: GPL-3.0-or-later
// T-206: the floating-point mode the game's code runs in.
//
// The PS2's FPU and vector units do not round to nearest: they truncate (round toward zero), and they
// flush denormals to zero. PCSX2 runs the guest thread with the same value (its default for the EE and
// for both VUs is "chop"). This port, until now, ran the game's code with whatever the host thread had,
// which on x86-64 is the C default: round to nearest, denormals kept. Measured consequence (T-202,
// journal 013): a constant the game computes and uploads to the VU1 (0x3F009A34 on the console, "a" in
// journal 013) comes out as 0x3F009A35 in the port, one unit in the last place up, in 1080 places.
//
// What this module does: gives the game thread the console's mode, once, when it starts, and gives the
// host's default back to the calls that enter a library which must behave as in the reference emulator
// (the GS library runs on a thread with the default mode there).
//   * the mode is the SSE control register: exceptions masked, round toward zero, flush-to-zero and
//     denormals-are-zero on;
//   * DON_FP_MODE=console (default) applies it; DON_FP_MODE=host leaves the thread as it is, so the two
//     can be compared in one pair of runs;
//   * on architectures without the SSE control register the functions do nothing and one "[don]"
//     warning is printed once.
#pragma once

#include <cstdint>
#include <string>

namespace fp_env
{

// Which mode the game thread should run in. `Console` is the PS2's (and PCSX2's): round toward zero,
// FTZ/DAZ on, exceptions masked. `Host` is "do nothing": the C default of the thread.
enum class Mode
{
    Console,
    Host,
};

// The value of DON_FP_MODE: "console" (the default, also for unset or empty) or "host"; anything else
// prints one [don] line and gives the default. The name of the parser is `mode_from_string` too, so it
// can be tested without the environment.
Mode mode_from_string(const char *value);
Mode mode_from_env();

// The name of a mode ("console" / "host"), for messages and tests.
const char *mode_name(Mode mode);

// The caller's floating-point control value, opaque. On x86-64 this is the SSE control register
// (_mm_getcsr / _mm_setcsr); elsewhere both are 0 and do nothing.
uint32_t capture();
void apply(Mode mode, uint32_t *previous);
void restore(uint32_t value);

// True when this build can set the mode (x86-64).
bool available();

// Applies `mode` to the calling thread and remembers the value it had, so that the game thread can be
// switched back and forth: `enter_game()` sets the game's mode (idempotent), `leave_game()` puts the
// host's value back. Used once per thread (thread_begin / thread_end).
void enter_game(Mode mode);
void leave_game();

// Game thread: a call that enters a library which has to see the host's default mode. Saves what the
// caller had, sets the host's value, and puts the caller's value back when it goes out of scope. With
// the mode set to `Host`, or when nothing was entered, it does nothing.
class HostCsrScope
{
public:
    HostCsrScope();
    ~HostCsrScope();
    HostCsrScope(const HostCsrScope &) = delete;
    HostCsrScope &operator=(const HostCsrScope &) = delete;

private:
    bool m_active = false;
    uint32_t m_saved = 0;
};

} // namespace fp_env
