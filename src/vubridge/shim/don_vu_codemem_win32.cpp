// SPDX-License-Identifier: GPL-3.0-or-later
// The code cache's memory on Windows (see don_vu_codemem.h).
#include "don_vu_codemem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void* donvu::codemem::AllocExecutable(void* hint, std::size_t bytes, std::string* err)
{
	// With a hint VirtualAlloc places the block exactly there (rounded down to the 64 KiB allocation
	// granularity) or fails; it never moves it, unlike mmap.
	void* const p = VirtualAlloc(hint, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (!p)
	{
		if (err)
			*err = std::string("VirtualAlloc of the microVU code cache failed: error ") + std::to_string(GetLastError());
		return nullptr;
	}
	return p;
}

void donvu::codemem::FreeExecutable(void* block, std::size_t /*bytes*/)
{
	VirtualFree(block, 0, MEM_RELEASE);
}
