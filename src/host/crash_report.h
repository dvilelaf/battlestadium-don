// SPDX-License-Identifier: GPL-3.0-or-later
// The crash report: what the program leaves behind when it dies, so that a black screen on a friend's
// PC can be diagnosed from the log alone. The block is written to stderr and straight to don_log_fd()
// (<user>/don.log), because after a crash the pipe tee thread may never run again.
//
// One reporter per platform, both compiled everywhere and guarding themselves (like the log_redirect
// twins):
//   crash_report_win32.cpp   SetUnhandledExceptionFilter + a first-chance vectored handler for the
//                            fatal codes + std::set_terminate
//   crash_report_posix.cpp   sigaction on an alternate stack for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT
// and, Windows only, the display-driver / power events since the previous start
//   driver_events_win32.cpp  queries the System event log with wevtapi, loaded dynamically
#pragma once

// May be called once, first thing in don_host_run() right after don_log_redirect(). Never throws.
void don_crash_report_install();

#ifdef _WIN32
#include <cstdint>

// Parses the log's previous dated header line
//     "===== Battle Stadium D.O.N started YYYY-MM-DD HH:MM:SS (pid N, build X) ====="
// (local time, written by log_redirect.cpp) into *out_utc, a Windows FILETIME value (100 ns since
// 1601-01-01) converted to UTC. The "(pid N, build X)" tail is ignored. Returns false when the line is
// not such a header or a field is out of range.
bool don_parse_previous_start_header(const char *line, std::int64_t *out_utc);

// Reads don_log_path() (<user>/don.log) and returns, in *out_utc, the start time of the run before this
// one (the header line before the current run's own header). Returns false when there is none (first
// run) or the log cannot be read.
bool don_previous_start_from_log(std::int64_t *out_utc);

// Prints the display-driver resets and unexpected power events of the System event log since
// previous_start_utc (a FILETIME) to stderr: one "[don] windows event: ..." line per event, at most 10,
// or the "[don] windows event: no display driver resets since <time>" line. Never throws; when the event
// API cannot be reached, nothing is printed.
void don_driver_events_report(std::int64_t previous_start_utc);
#endif // _WIN32
