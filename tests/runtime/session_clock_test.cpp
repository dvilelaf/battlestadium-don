// SPDX-License-Identifier: GPL-3.0-or-later
// T-506, runtime patch 0074: once PS2Runtime::setSessionClock(epoch) is set, every host time the guest can
// see comes from epoch + vsync_tick / 60 in UTC, never from the PC. The three places of knowledge 22 section 8:
// sceCdReadClock, the OSD time zone, and the memory card's GetDir dates (plus fioGetstat's file times). Unset,
// nothing changes.
//
// The test forces a non-UTC time zone (Asia/Tokyo) so a guest time that still used localtime or std::time(nullptr)
// would differ from UTC: sceCdReadClock must answer the epoch's UTC hour (22), not the local one (7).

#include "host_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

namespace ps2_stubs
{
    void sceCdReadClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}

namespace ps2_syscalls
{
    void GetOsdConfigParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
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

    void setArg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi32(0, 0, 0, static_cast<int>(value));
    }

    uint8_t toBcd(uint32_t value)
    {
        return static_cast<uint8_t>(((value / 10u) % 10u) << 4 | (value % 10u));
    }

    // 2023-11-14 22:13:20 UTC.
    constexpr std::time_t kEpoch = 1700000000;

    // Writes "path" (a NUL-terminated string) into guest RAM and returns the address.
    uint32_t writePath(PS2Runtime &rt, const std::string &path, uint32_t addr)
    {
        uint8_t *ram = rt.memory().getRDRAM();
        std::memcpy(ram + addr, path.c_str(), path.size() + 1u);
        return addr;
    }

    void testUnsetIsUnchanged(PS2Runtime &rt)
    {
        expect(!rt.hasSessionClock(), "unset: hasSessionClock() is false");
        expect(rt.sessionEpoch() == 0u, "unset: sessionEpoch() is 0");
        expect(rt.sessionTimeNow() == 0, "unset: sessionTimeNow() is 0");
    }

    void testCdReadClock(PS2Runtime &rt)
    {
        constexpr uint32_t kClockAddr = 0x001F0000u;
        R5900Context ctx;
        setArg(ctx, 4, kClockAddr);
        ps2_stubs::sceCdReadClock(rt.memory().getRDRAM(), &ctx, &rt);

        const uint8_t *clock = rt.memory().getRDRAM() + kClockAddr;

        std::tm utc{};
        gmtime_r(&kEpoch, &utc);

        expect(clock[1] == toBcd(static_cast<uint32_t>(utc.tm_sec)),
               "sceCdReadClock: seconds BCD (got 0x" + std::to_string(clock[1]) + ")");
        expect(clock[2] == toBcd(static_cast<uint32_t>(utc.tm_min)), "sceCdReadClock: minutes BCD");
        expect(clock[3] == toBcd(static_cast<uint32_t>(utc.tm_hour)),
               "sceCdReadClock: hour is the epoch's UTC hour, not the PC's local time (got 0x" +
                   std::to_string(clock[3]) + ")");
        expect(clock[5] == toBcd(static_cast<uint32_t>(utc.tm_mday)), "sceCdReadClock: day BCD");
        expect(clock[6] == toBcd(static_cast<uint32_t>(utc.tm_mon + 1)), "sceCdReadClock: month BCD");
        expect(clock[7] == toBcd(static_cast<uint32_t>((utc.tm_year + 1900) % 100)), "sceCdReadClock: year BCD");
        // The sanity values, hard-coded so a wrong epoch is caught even if gmtime changes.
        expect(clock[3] == toBcd(22) && clock[6] == toBcd(11) && clock[7] == toBcd(23),
               "sceCdReadClock: epoch 1700000000 is 2023-11-14 22:13:20 UTC");
    }

    void testOsdTimeZone(PS2Runtime &rt)
    {
        constexpr uint32_t kParamAddr = 0x001F0100u;
        R5900Context ctx;
        setArg(ctx, 4, kParamAddr);
        ps2_syscalls::GetOsdConfigParam(rt.memory().getRDRAM(), &ctx, &rt);

        uint32_t raw = 0;
        std::memcpy(&raw, rt.memory().getRDRAM() + kParamAddr, sizeof(raw));
        const uint32_t tz = (raw >> 21) & 0x7FFu;
        expect(tz == 0u, "GetOsdConfigParam: time-zone offset is 0 with a session clock (got " + std::to_string(tz) + ")");
    }

    void testGetDirDates(PS2Runtime &rt, const std::filesystem::path &card)
    {
        constexpr uint32_t kPathAddr = 0x001F0200u;
        constexpr uint32_t kTableAddr = 0x001F0300u;

        R5900Context init;
        ps2_stubs::sceMcInit(rt.memory().getRDRAM(), &init, &rt);

        R5900Context ctx;
        setArg(ctx, 4, 0);                       // port 0
        setArg(ctx, 5, 0);                       // slot 0
        setArg(ctx, 6, writePath(rt, ".", kPathAddr)); // path "."
        setArg(ctx, 7, 0);                       // attr
        setArg(ctx, 8, 8);                       // max entries
        setArg(ctx, 9, kTableAddr);              // table
        ps2_stubs::sceMcGetDir(rt.memory().getRDRAM(), &ctx, &rt);

        const uint8_t *table = rt.memory().getRDRAM() + kTableAddr;
        std::tm utc{};
        gmtime_r(&kEpoch, &utc);

        // The "." entry's _Create date (SceMcTblGetDir: _Create at offset 0; Sec=1, Min=2, Hour=3, Day=4,
        // Month=5, Year=6..7 little-endian, all binary, not BCD).
        expect(table[1] == static_cast<uint8_t>(utc.tm_sec),
               "sceMcGetDir: \".\" seconds equal the session time (got " + std::to_string(table[1]) + ")");
        expect(table[3] == static_cast<uint8_t>(utc.tm_hour),
               "sceMcGetDir: \".\" hour is the session's UTC hour (got " + std::to_string(table[3]) + ")");
        expect(table[5] == static_cast<uint8_t>(utc.tm_mon + 1), "sceMcGetDir: \".\" month");
        uint16_t year = 0;
        std::memcpy(&year, table + 6, sizeof(year));
        expect(year == static_cast<uint16_t>(utc.tm_year + 1900), "sceMcGetDir: \".\" year");
    }

    void run()
    {
        // A non-UTC zone, so "UTC" and "the PC's local time" are observably different.
        setenv("TZ", "Asia/Tokyo", 1);
        tzset();

        namespace fs = std::filesystem;
        const fs::path card = fs::temp_directory_path() / "don-session-clock-test";
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
        if (!expect(rt.initialize("session-clock-test"), "PS2Runtime::initialize"))
            return;

        testUnsetIsUnchanged(rt);

        rt.setSessionClock(static_cast<uint64_t>(kEpoch));
        expect(rt.hasSessionClock(), "set: hasSessionClock() is true");
        expect(rt.sessionEpoch() == static_cast<uint64_t>(kEpoch), "set: sessionEpoch()");
        expect(rt.sessionTimeNow() == kEpoch, "set: sessionTimeNow() is epoch + 0 ticks");

        testCdReadClock(rt);
        testOsdTimeZone(rt);
        testGetDirDates(rt, card);

        fs::remove_all(card, ec);
    }
} // namespace

int main()
{
    run();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "session_clock_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("session_clock_test: passed\n");
    return 0;
}
