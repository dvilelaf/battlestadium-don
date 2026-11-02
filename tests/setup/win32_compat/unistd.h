// The few POSIX calls the setup tests make besides fork/exec/signals, for the Windows cross-build (T-741).
// Used only on WIN32, through an include directory of tests/setup/CMakeLists.txt; the tests that need fork, waitpid,
// kill or /proc are not built for Windows (see the SETUP_LINUX_ONLY list there).
#pragma once
#include <process.h>
#include <stdlib.h>

#include <string>

inline int getpid() { return _getpid(); }
inline int geteuid() { return 1000; }  // never root: the tests' "root ignores permissions" guards stay armed
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }  // an empty value removes it in the CRT
