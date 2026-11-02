// build_id (D14): returns DON_BUILD_ID, set by src/host/CMakeLists.txt as this file's compile definition
// (see build_id.h). "unknown" when the definition was not set (a build outside the host's CMake).
#include "build_id.h"

#ifndef DON_BUILD_ID
#define DON_BUILD_ID "unknown"
#endif

const char *don_build_id()
{
    return DON_BUILD_ID;
}
