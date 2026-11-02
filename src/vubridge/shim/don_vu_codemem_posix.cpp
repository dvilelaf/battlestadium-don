// SPDX-License-Identifier: GPL-3.0-or-later
// The code cache's memory on POSIX systems (see don_vu_codemem.h).
#include "don_vu_codemem.h"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>

void* donvu::codemem::AllocExecutable(void* hint, std::size_t bytes, std::string* err)
{
	void* const p = mmap(hint, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
	{
		if (err)
			*err = std::string("mmap of the microVU code cache failed: ") + std::strerror(errno);
		return nullptr;
	}
	return p;
}

void donvu::codemem::FreeExecutable(void* block, std::size_t bytes)
{
	munmap(block, bytes);
}
