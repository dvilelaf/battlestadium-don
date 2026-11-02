// SPDX-License-Identifier: GPL-3.0-or-later
// T-305, patch 0067: the disc's files are read once in the background so the game thread's reads hit the page
// cache. Header-only helper, no game. Proves: a file of 3 MiB + 5 bytes is read completely by the thread; asking
// again for the same path does not read it twice; a file under 256 KiB is left alone; DON_NO_PREFETCH=1 turns it off.

#include "runtime/ps2_file_prefetch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    int g_failures = 0;
    void check(bool ok, const char *what)
    {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }
    void makeFile(const std::filesystem::path &path, size_t size)
    {
        std::ofstream out(path, std::ios::binary);
        std::string block(size, 'x');
        out.write(block.data(), static_cast<std::streamsize>(size));
    }
    bool waitFor(uint64_t bytes)
    {
        for (int i = 0; i < 500; ++i)
        {
            if (ps2_file_prefetch::bytesRead().load() >= bytes)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
}

int main()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "don_prefetch_test";
    std::filesystem::create_directories(dir);
    const size_t big = 3u * 1024u * 1024u + 5u;
    makeFile(dir / "big.bin", big);
    makeFile(dir / "small.bin", 1000u);

    ps2_file_prefetch::start(dir / "small.bin");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(ps2_file_prefetch::bytesRead().load() == 0u, "a small file is left alone");

    ps2_file_prefetch::start(dir / "big.bin");
    check(waitFor(big), "the background thread reads the whole big file");
    ps2_file_prefetch::start(dir / "big.bin");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(ps2_file_prefetch::bytesRead().load() == big, "the same path is read once");

    makeFile(dir / "big2.bin", big);
    setenv("DON_NO_PREFETCH", "1", 1);
    ps2_file_prefetch::start(dir / "big2.bin");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(ps2_file_prefetch::bytesRead().load() == big, "DON_NO_PREFETCH=1 turns it off");

    std::filesystem::remove_all(dir);
    return g_failures == 0 ? 0 : 1;
}
