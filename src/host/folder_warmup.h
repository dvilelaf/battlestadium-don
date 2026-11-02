#pragma once
// T-306 (journal 038, follow-up 2026-10-15): the game reads its big packed files (DATA.AFS, BGM.AFS, SE.AFS) in
// pieces of 8 KiB to 1 MiB, opening and closing them over and over; on a hard disk each first read of a piece waited
// 20 to 150 ms (strace on the game thread), which is the "NOW LOADING" and title Start hitch. Reading the files of the
// game's folder once, front to back, at start-up puts them in the host's page cache before the game asks.
// DON_NO_WARMUP=1 turns it off. Nothing the guest sees changes; only when the disk is touched.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace don_host
{
    inline void warm_game_folder(const std::filesystem::path &elf_path)
    {
        if (const char *off = std::getenv("DON_NO_WARMUP"); off && off[0] == '1')
            return;
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path folder = fs::absolute(elf_path, ec).parent_path();
        if (ec || folder.empty())
            return;
        std::thread([folder]() {
            const auto t0 = std::chrono::steady_clock::now();
            struct Item { fs::path path; uint64_t size; };
            std::vector<Item> items;
            std::error_code error;
            for (fs::recursive_directory_iterator it(folder, fs::directory_options::follow_directory_symlink, error), end;
                 !error && it != end; it.increment(error))
            {
                std::error_code e2;
                if (!it->is_regular_file(e2))
                    continue; // follows file symlinks (the variant folders are made of them)
                const uint64_t size = fs::file_size(it->path(), e2);
                if (!e2)
                    items.push_back({it->path(), size});
            }
            // Movies are only read when they are played: last. The rest, smallest first, so what the game needs
            // soonest (menus, sounds) is there before the big streams.
            auto movie = [](const Item &i) { return i.path.filename() == "MOVIE.AFS"; };
            std::sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
                if (movie(a) != movie(b)) return movie(b);
                return a.size < b.size;
            });
            std::vector<char> chunk(1u << 20);
            uint64_t total = 0;
            for (const Item &item : items)
            {
                std::FILE *stream = std::fopen(item.path.string().c_str(), "rb");
                if (!stream)
                    continue;
                for (;;)
                {
                    const size_t got = std::fread(chunk.data(), 1u, chunk.size(), stream);
                    total += got;
                    if (got != chunk.size())
                        break;
                }
                std::fclose(stream);
            }
            // T-520: the warm-up's duration and bytes, always (a slow warm-up points at a virus scanner or a
            // slow disk, not at the game). The old DON_HOST_LOG-gated line is folded into this one.
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "[don] warmup %.1f s, %zu files, %llu bytes\n", seconds, items.size(),
                         static_cast<unsigned long long>(total));
        }).detach();
    }
} // namespace don_host
