// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one operating-system call the VU library makes for its code cache: a block of read-write-execute
// memory, optionally at a requested address. The search for a block near the library's image is
// platform independent and lives in don_vu_shims.cpp (AllocCodeCache); this is what it calls.
// Implemented by don_vu_codemem_posix.cpp (mmap, Linux and macOS) and don_vu_codemem_win32.cpp
// (VirtualAlloc); exactly one of them is in the build (CMakeLists.txt).
#pragma once

#include <cstddef>
#include <string>

namespace donvu::codemem
{
	// Reserves and commits `bytes` of read-write-execute memory. `hint` is where the block should be
	// (nullptr: anywhere). With a hint the block is placed there only if the system grants that range:
	// it may return a block elsewhere (POSIX treats the hint as advice) or fail; the caller checks the
	// address. Returns nullptr on failure and, when `err` is not null, says why in `*err`.
	void* AllocExecutable(void* hint, std::size_t bytes, std::string* err);

	// Releases a block from AllocExecutable (`bytes` as it was asked for).
	void FreeExecutable(void* block, std::size_t bytes);
}
