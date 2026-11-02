// Build id of the port (D14, docs/knowledge/22-netplay.md section 3 HELLO): the short string every player
// sends in the handshake, so the host can reject a player with another build. At most 47 characters:
// "<12-hex commit>[-dirty]-<ffmpeg|noffmpeg>". src/host/CMakeLists.txt computes it at configure time and
// passes it as the compile definition DON_BUILD_ID of build_id.cpp only, so a new commit rebuilds just this file.
#pragma once

// The build id, owned by build_id.cpp; the pointer stays valid for the life of the process.
const char *don_build_id();
