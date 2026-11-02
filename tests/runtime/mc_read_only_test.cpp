// SPDX-License-Identifier: GPL-3.0-or-later
// T-307: a card folder the host cannot write to must read as a card with no free space.
//
// The game's save script asks sceMcGetInfo for the free blocks before it writes; with less than the 65 KB it
// needs it shows "There is not enough space on the memory card ... (you need 65KB or more of free space)". Its
// mkdir/open/write steps ignore the result code of a failure (every code only sets the same flag and the
// script then asks again), so a read-only folder was answered with the save question coming back and nothing
// said. The runtime used to report 8192 free blocks whatever the folder allowed.
//
// Checks: a writable card folder reports 8192 free blocks (and leaves no probe file behind); the same folder
// made read-only reports 0 with result 0 (formatted, so the dialog is the space one, not "unformatted"); once
// writable again it reports 8192. A temporary card folder; no game, no window. Skipped where the folder
// stays writable after the permissions are taken away (running as root, or Windows directories).

#include "host_test_support.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace ps2_stubs
{
    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
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

    constexpr uint32_t kTypeAddr = 0x001F0000u;
    constexpr uint32_t kFreeAddr = 0x001F0004u;
    constexpr uint32_t kFormatAddr = 0x001F0008u;
    constexpr uint32_t kResultAddr = 0x001F1000u;

    void setArg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi32(0, 0, 0, static_cast<int>(value));
    }

    // sceMcGetInfo(0, 0, &type, &free, &format) then the sync the script does; returns the free blocks.
    int32_t freeBlocks(PS2Runtime &rt, int32_t &syncResult)
    {
        R5900Context ctx;
        setArg(ctx, 4, 0);
        setArg(ctx, 5, 0);
        setArg(ctx, 6, kTypeAddr);
        setArg(ctx, 7, kFreeAddr);
        setArg(ctx, 8, kFormatAddr);
        ps2_stubs::sceMcGetInfo(rt.memory().getRDRAM(), &ctx, &rt);

        R5900Context sync;
        setArg(sync, 4, 1);
        setArg(sync, 5, 0);
        setArg(sync, 6, kResultAddr);
        ps2_stubs::sceMcSync(rt.memory().getRDRAM(), &sync, &rt);
        std::memcpy(&syncResult, rt.memory().getRDRAM() + kResultAddr, sizeof(syncResult));

        int32_t blocks = -1;
        std::memcpy(&blocks, rt.memory().getRDRAM() + kFreeAddr, sizeof(blocks));
        return blocks;
    }

    void testReadOnlyCard()
    {
        unsetenv("DISPLAY");
        namespace fs = std::filesystem;
        const fs::path card = fs::temp_directory_path() / "don-mc-read-only-test";
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
        if (!expect(rt.initialize("mc-read-only-test"), "PS2Runtime::initialize"))
            return;

        R5900Context init;
        ps2_stubs::sceMcInit(rt.memory().getRDRAM(), &init, &rt);

        int32_t result = 99;
        int32_t blocks = freeBlocks(rt, result);
        expect(blocks == 0x2000 && result == 0,
               "a writable card has 8192 free blocks, result 0 (got " + std::to_string(blocks) + ", " + std::to_string(result) + ")");
        expect(!fs::exists(card / "mc0" / ".don_write_probe", ec), "the probe file is removed");

        const fs::path mc0 = card / "mc0";
        fs::permissions(mc0, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                        fs::perm_options::remove, ec);
        const fs::path probe = mc0 / "don-test-probe";
        if (FILE *f = std::fopen(probe.string().c_str(), "wb"))
        {
            std::fclose(f);
            fs::remove(probe, ec);
            std::printf("mc_read_only_test: skipped (the folder stays writable here)\n");
            fs::permissions(mc0, fs::perms::owner_all, fs::perm_options::add, ec);
            fs::remove_all(card, ec);
            return;
        }

        // The probe result is kept for a second.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        blocks = freeBlocks(rt, result);
        expect(blocks == 0 && result == 0,
               "a read-only card has 0 free blocks, result 0 (got " + std::to_string(blocks) + ", " + std::to_string(result) + ")");

        fs::permissions(mc0, fs::perms::owner_all, fs::perm_options::add, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        blocks = freeBlocks(rt, result);
        expect(blocks == 0x2000, "writable again: 8192 free blocks (got " + std::to_string(blocks) + ")");

        fs::remove_all(card, ec);
    }
} // namespace

int main()
{
    testReadOnlyCard();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "mc_read_only_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("mc_read_only_test: passed\n");
    return 0;
}
