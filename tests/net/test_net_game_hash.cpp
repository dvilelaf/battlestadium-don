// game_hash (D14, knowledge 22 section 3): equal folders hash equal, one changed byte changes the hash, a
// missing file fails. Uses a temporary folder of small fake files, never the game's.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.h"
#include "game_hash.h"

using namespace don_net;

namespace
{

void write_file(const std::filesystem::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// A folder with a fake ELF and the four AFS files, in the layout the port expects; returns the ELF path.
std::filesystem::path make_folder(const std::filesystem::path &dir)
{
    const std::filesystem::path elf = dir / "SLPS_256.75";
    write_file(elf, "the ELF's bytes");
    write_file(dir / "DATA.AFS", "data-afs");
    write_file(dir / "BGM.AFS", "bgm-afs-bytes");
    write_file(dir / "SE.AFS", "se");
    write_file(dir / "MOVIE.AFS", "movie-afs");
    return elf;
}

} // namespace

int main()
{
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "don_hash_game_test";
    std::filesystem::remove_all(base);
    const std::filesystem::path a = base / "a";
    const std::filesystem::path b = base / "b";
    std::filesystem::create_directories(a);
    std::filesystem::create_directories(b);

    std::string error;
    const uint64_t ha = hash_game_files(make_folder(a), error);
    CHECK(error.empty());
    CHECK(ha != 0);
    const uint64_t hb = hash_game_files(make_folder(b), error);
    CHECK(error.empty());
    CHECK_EQ(ha, hb); // equal folders -> equal hashes

    // One changed byte changes the hash.
    {
        std::ofstream out(b / "DATA.AFS", std::ios::binary | std::ios::app);
        out.put('!');
    }
    const uint64_t hb2 = hash_game_files(b / "SLPS_256.75", error);
    CHECK(error.empty());
    CHECK(ha != hb2);

    // A missing file fails with an error and returns 0.
    std::filesystem::remove(b / "SE.AFS");
    const uint64_t hmissing = hash_game_files(b / "SLPS_256.75", error);
    CHECK(hmissing == 0);
    CHECK(!error.empty());

    std::filesystem::remove_all(base);
    return check_result("test_net_game_hash");
}
