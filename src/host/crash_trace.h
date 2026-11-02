// SPDX-License-Identifier: GPL-3.0-or-later
// T-705: one line for any fatal end of the program, from its first instruction: before the log redirect and the
// crash report of host.cpp exist (the setup screens and --prepare run before them) a crash left nothing.
//   [don] fatal: <SIGSEGV|SIGABRT|SIGBUS|SIGFPE|SIGILL | what() of the uncaught exception> during <phase>
// is written to the stderr the program started with and appended to <user>/don.log, then the default action
// happens (the signal is re-raised; an uncaught exception ends in abort()). The signal path only uses write(),
// descriptors opened at install time and static buffers.
//
// Diagnostic: DON_DIAG_CRASH=<phase name> makes don_crash_trace_diag(<that phase>) raise SIGSEGV (off unless set).
#pragma once

namespace don_host
{
    // Phase names as printed in the line.
    inline constexpr const char *kPhaseSetup = "setup screens";
    inline constexpr const char *kPhasePrepare = "preparing";
    inline constexpr const char *kPhaseGame = "game";

    // Installs the handlers. Call once, first thing in main(). Never throws.
    void crash_trace_install();

    // The phase named in the line. `phase` must be a string with static storage.
    void crash_trace_set_phase(const char *phase);

    // Raises SIGSEGV when DON_DIAG_CRASH names `which` ("setup", "prepare" or "game"); otherwise does nothing.
    void crash_trace_diag(const char *which);
}
