// SPDX-License-Identifier: GPL-3.0-or-later
// The program's own log, both platforms, decided by the program (not by wrappers): stdout and stderr
// always go to <user>/don.log (appended under a dated header with the pid and the build id, past 10 MB
// the file becomes don.old.log), and are ALSO still written to the original stdout and stderr through a
// pipe + thread (a tee), so a terminal, a test's pipe or a script sees everything exactly as today.
// don_host_run() calls don_log_redirect() first thing, before anything prints; the crash reporter is
// installed right after (crash_report.h).
#pragma once

#include <filesystem>
#include <string>

// May be called once, first thing in don_host_run(). Never throws; on failure the streams are left alone.
void don_log_redirect();

// The log file path (<user>/don.log). Empty before don_log_redirect().
std::filesystem::path don_log_path();

// The log file descriptor (POSIX fd / Windows CRT fd), or -1 when the log could not be opened. The crash
// reporter writes its block here straight (the pipe may not be drained before the process dies).
int don_log_fd();

// The stderr the program was started with (a terminal, a test's pipe), or -1 before don_log_redirect() or
// without one. The crash reporter writes there and to don_log_fd(); writing to fd 2 would go through the
// tee and put its block in the log twice.
int don_log_original_stderr_fd();

// The end-of-game "[don] exit:" line marks itself written; don_host_run writes a short one for any other
// way out, so every start's block ends with one (the next start reads a missing one as a crash).
void don_log_note_exit_written();
bool don_log_exit_written();

// Flushes stdio (the tee keeps running).
void don_log_flush();

// Ends the tee: stdout and stderr go back to the original streams and what the threads still hold is written
// to the log and to them before this returns. don_host_run() calls it on every way out (main.cpp ends with
// _Exit, which would not wait for the threads).
void don_log_finish();
