// Hash of the game files (D14, docs/knowledge/22-netplay.md section 3 HELLO): the same data hash on every
// copy lets the host reject a player whose game data differ. FNV-1a 64, fixed order, no allocations to
// speak of; reads in 1 MiB blocks so a huge file does not need to be held in memory.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace don_net
{

// FNV-1a 64 over: the ELF's bytes, then DATA.AFS, BGM.AFS, SE.AFS, MOVIE.AFS in that order, each (from the
// ELF's folder, the case as on disk) as its size in 8 little-endian bytes then its bytes. A file that cannot
// be opened (a missing AFS file, for example) sets error and returns 0.
uint64_t hash_game_files(const std::filesystem::path &elf, std::string &error);

} // namespace don_net
