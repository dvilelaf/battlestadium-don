// game_hash (D14, knowledge 22 section 3): FNV-1a 64 over the game files, in the fixed order the HELLO
// handshake compares. No SDL, no runtime: the unit tests build it alone.
#include "game_hash.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace don_net
{
namespace
{

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kBlockSize = 1u << 20; // 1 MiB

void fnv_update(uint64_t &hash, const uint8_t *data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
}

// Hashes the bytes of path (no size prefix) into hash. False with error set when the file cannot be opened.
bool hash_bytes(const std::filesystem::path &path, uint64_t &hash, std::string &error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path.string();
        return false;
    }
    std::vector<uint8_t> buffer(kBlockSize);
    while (in)
    {
        in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
            fnv_update(hash, buffer.data(), static_cast<std::size_t>(got));
    }
    return true;
}

} // namespace

uint64_t hash_game_files(const std::filesystem::path &elf, std::string &error)
{
    error.clear();
    uint64_t hash = kFnvOffset;

    if (!hash_bytes(elf, hash, error))
        return 0;

    const std::filesystem::path dir = elf.parent_path();
    for (const char *name : {"DATA.AFS", "BGM.AFS", "SE.AFS", "MOVIE.AFS"})
    {
        const std::filesystem::path path = dir / name;
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            error = "cannot read the size of " + path.string();
            return 0;
        }
        uint8_t size_bytes[8];
        for (int i = 0; i < 8; ++i)
            size_bytes[i] = static_cast<uint8_t>((size >> (8 * i)) & 0xFF);
        fnv_update(hash, size_bytes, sizeof(size_bytes));
        if (!hash_bytes(path, hash, error))
            return 0;
    }
    return hash;
}

} // namespace don_net
