// SPDX-License-Identifier: GPL-3.0-or-later
// T-307: the memory-card open must hand out file numbers the way the console's module does.
//
// The game's save script opens `/BISLPS-25675game/BISLPS-25675game` (flags 0x202), takes the file number from
// the sync that follows the open, and writes 992 bytes to it. On the console the first open of a fresh
// memory-card module gives file number 0 (MCMAN.IRX keeps three entries of 0x30 bytes and returns the index of
// the lowest free one); the game writes to 0. The runtime used to start at 1 and only count up, so that write
// went to a file number that did not exist and answered -4.
//
// Checks: the first open gives 0 and the write of 992 bytes succeeds and leaves 992 bytes in the file; a second
// file opened while the first is open gives 1; a number is reused after a close; a fourth open with three in use
// fails with -7. A temporary card folder; no game, no window.

#include "host_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace ps2_stubs
{
    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

namespace
{
    int g_failures = 0;

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        }
        return condition;
    }

    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    int32_t call(PS2Runtime &rt, Handler fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
    {
        R5900Context ctx;
        ctx.r[4] = _mm_set_epi32(0, 0, 0, static_cast<int>(a0));
        ctx.r[5] = _mm_set_epi32(0, 0, 0, static_cast<int>(a1));
        ctx.r[6] = _mm_set_epi32(0, 0, 0, static_cast<int>(a2));
        ctx.r[7] = _mm_set_epi32(0, 0, 0, static_cast<int>(a3));
        fn(rt.memory().getRDRAM(), &ctx, &rt);
        return static_cast<int32_t>(_mm_extract_epi32(ctx.r[2], 0));
    }

    constexpr uint32_t kPathAddr = 0x001F0000u;
    constexpr uint32_t kResultAddr = 0x001F1000u;
    constexpr uint32_t kBufAddr = 0x001F2000u;

    void putString(PS2Runtime &rt, const char *text)
    {
        std::memcpy(rt.memory().getRDRAM() + kPathAddr, text, std::strlen(text) + 1);
    }

    // sceMcSync(1, 0, &result): the result of the command that ran, as the game's script reads it.
    int32_t syncResult(PS2Runtime &rt)
    {
        const int32_t finished = call(rt, ps2_stubs::sceMcSync, 1, 0, kResultAddr);
        expect(finished == 1, "sceMcSync reports the command as finished");
        int32_t result = 0;
        std::memcpy(&result, rt.memory().getRDRAM() + kResultAddr, sizeof(result));
        return result;
    }

    int32_t openFile(PS2Runtime &rt, const char *path)
    {
        putString(rt, path);
        call(rt, ps2_stubs::sceMcOpen, 0, 0, kPathAddr, 0x202u);
        return syncResult(rt);
    }

    int32_t writeFile(PS2Runtime &rt, int32_t fd, uint32_t size)
    {
        call(rt, ps2_stubs::sceMcWrite, static_cast<uint32_t>(fd), kBufAddr, size);
        return syncResult(rt);
    }

    int32_t closeFile(PS2Runtime &rt, int32_t fd)
    {
        call(rt, ps2_stubs::sceMcClose, static_cast<uint32_t>(fd));
        return syncResult(rt);
    }

    void testFileNumbers()
    {
        unsetenv("DISPLAY");
        namespace fs = std::filesystem;
        const fs::path card = fs::temp_directory_path() / "don-mc-file-numbers-test";
        std::error_code ec;
        fs::remove_all(card, ec);
        fs::create_directories(card, ec);

        PS2Runtime::IoPaths paths;
        paths.elfDirectory = card;
        paths.hostRoot = card;
        paths.cdRoot = card;
        paths.mcRoot = card / "mc0";
        PS2Runtime::setIoPaths(paths);

        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        if (!expect(rt.initialize("mc-file-numbers-test"), "PS2Runtime::initialize"))
            return;
        std::memset(rt.memory().getRDRAM() + kBufAddr, 0x5A, 0x400);

        call(rt, ps2_stubs::sceMcInit);

        putString(rt, "/BISLPS-25675game");
        call(rt, ps2_stubs::sceMcMkdir, 0, 0, kPathAddr);
        expect(syncResult(rt) == 0, "mkdir /BISLPS-25675game succeeds");
        call(rt, ps2_stubs::sceMcChdir, 0, 0, kPathAddr);
        expect(syncResult(rt) == 0, "chdir /BISLPS-25675game succeeds");

        const int32_t fd0 = openFile(rt, "/BISLPS-25675game/BISLPS-25675game");
        expect(fd0 == 0, "the first open gives file number 0 (got " + std::to_string(fd0) + ")");

        const int32_t written = writeFile(rt, 0, 992);
        expect(written == 992, "the write of 992 bytes to file number 0 returns 992 (got " + std::to_string(written) + ")");

        const fs::path saved = card / "mc0" / "BISLPS-25675game" / "BISLPS-25675game";
        const auto size = fs::exists(saved, ec) ? fs::file_size(saved, ec) : 0u;
        expect(size == 992, "the file in the card has 992 bytes (has " + std::to_string(size) + ")");

        const int32_t fd1 = openFile(rt, "/BISLPS-25675game/icon.sys");
        expect(fd1 == 1, "a second file opened while the first is open gives 1 (got " + std::to_string(fd1) + ")");
        const int32_t fd2 = openFile(rt, "/BISLPS-25675game/battle_logo.ico");
        expect(fd2 == 2, "a third gives 2 (got " + std::to_string(fd2) + ")");
        const int32_t fd3 = openFile(rt, "/BISLPS-25675game/fourth");
        expect(fd3 == -7, "a fourth with three in use fails with -7 (got " + std::to_string(fd3) + ")");

        expect(closeFile(rt, 1) == 0, "close of file number 1 succeeds");
        const int32_t reused = openFile(rt, "/BISLPS-25675game/icon.sys");
        expect(reused == 1, "a number is reused after a close (got " + std::to_string(reused) + ")");

        expect(closeFile(rt, 0) == 0, "close of file number 0 succeeds");
        expect(closeFile(rt, 1) == 0, "close of file number 1 succeeds again");
        expect(closeFile(rt, 2) == 0, "close of file number 2 succeeds");
        const int32_t again = openFile(rt, "/BISLPS-25675game/BISLPS-25675game");
        expect(again == 0, "after closing everything the next open gives 0 (got " + std::to_string(again) + ")");
        closeFile(rt, again);

        fs::remove_all(card, ec);
    }
} // namespace

int main()
{
    testFileNumbers();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "mc_file_numbers_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("mc_file_numbers_test: all checks passed\n");
    return 0;
}
