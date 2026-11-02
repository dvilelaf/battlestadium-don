// SPDX-License-Identifier: GPL-3.0-or-later
// T-308, patch 0066: the vibration calls reach the host. A real PS2Runtime with a host pad source that has the
// `actuator` callback; the guest calls (scePadSetActAlign, scePadSetActDirect, ...) are made the way the generated
// code makes them. Run with DISPLAY unset (the test unsets it). No game.
//
// What it proves:
//   * the align table says which byte drives which motor: with {0, 1, 0xFF...} data[0] is the small motor (0 or 1)
//     and data[1] the large one (0..255); an unset table drives nothing;
//   * the callback gets the (port, slot) and the pair of values, only when the pair changed;
//   * the pads are independent (two ports);
//   * a closed port, scePadInit and scePadEnd switch the motors off (one callback with 0, 0);
//   * a pad that is not open or not plugged in answers 0 and changes nothing;
//   * without the callback the calls still answer 1.

#include "host_test_support.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ps2_stubs
{
    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
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

    constexpr uint32_t kDma = 0x00010000u;   // + port * 0x100
    constexpr uint32_t kBytes = 0x00030000u; // the 6-byte tables

    void putBytes(PS2Runtime &rt, const uint8_t (&b)[6])
    {
        for (int i = 0; i < 6; ++i)
            rt.memory().getRDRAM()[kBytes + i] = b[i];
    }

    int32_t setAlign(PS2Runtime &rt, int port, int slot, const uint8_t (&b)[6])
    {
        putBytes(rt, b);
        return call(rt, ps2_stubs::scePadSetActAlign, port, slot, kBytes);
    }

    int32_t setDirect(PS2Runtime &rt, int port, int slot, const uint8_t (&b)[6])
    {
        putBytes(rt, b);
        return call(rt, ps2_stubs::scePadSetActDirect, port, slot, kBytes);
    }

    struct Event
    {
        int port, slot;
        unsigned small, large;
        bool operator==(const Event &o) const
        {
            return port == o.port && slot == o.slot && small == o.small && large == o.large;
        }
    };

    struct Source
    {
        bool connected[2] = {true, true};
        std::vector<Event> events;
        Ps2HostPad iface;

        explicit Source(bool with_callback)
        {
            iface.user = this;
            iface.port_count = 2;
            iface.slots_per_port[0] = 1;
            iface.slots_per_port[1] = 1;
            iface.read = [](void *u, int port, int, Ps2HostPadState *out) {
                out->connected = static_cast<Source *>(u)->connected[port] ? 1 : 0;
            };
            if (with_callback)
            {
                iface.actuator = [](void *u, int port, int slot, uint8_t small, uint8_t large) {
                    static_cast<Source *>(u)->events.push_back({port, slot, small, large});
                };
            }
        }
    };

    std::string show(const std::vector<Event> &v)
    {
        std::string out;
        for (const Event &e : v)
            out += "(" + std::to_string(e.port) + "," + std::to_string(e.slot) + ":" + std::to_string(e.small) + "/" +
                   std::to_string(e.large) + ")";
        return out;
    }

    void openBoth(PS2Runtime &rt)
    {
        call(rt, ps2_stubs::scePadInit);
        for (int p = 0; p < 2; ++p)
            expect(call(rt, ps2_stubs::scePadPortOpen, p, 0, kDma + p * 0x100u) == 1, "PortOpen succeeds");
    }

    void scenarioMotors()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("pad-actuator-test"), "initialize()");
        Source src(true);
        rt.setHostPad(&src.iface);
        openBoth(rt);

        const uint8_t align[6] = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t on[6] = {1, 200, 0, 0, 0, 0};
        const uint8_t large_only[6] = {0, 0x80, 0, 0, 0, 0};
        const uint8_t off[6] = {0, 0, 0, 0, 0, 0};

        // The game never calls SetActAlign: a pad starts with {small, large} (byte 0, byte 1).
        expect(setDirect(rt, 0, 0, on) == 1, "SetActDirect answers 1");
        expect(src.events == std::vector<Event>{{0, 0, 1, 200}}, "the default table: " + show(src.events));
        expect(setAlign(rt, 0, 0, align) == 1, "SetActAlign answers 1");
        expect(src.events == std::vector<Event>{{0, 0, 1, 200}}, "the same table again: " + show(src.events));

        src.events.clear();
        setDirect(rt, 0, 0, on);
        expect(src.events.empty(), "the same pair again is not reported: " + show(src.events));

        setDirect(rt, 0, 0, large_only);
        expect(src.events == std::vector<Event>{{0, 0, 0, 0x80}}, "small off, large 0x80: " + show(src.events));

        src.events.clear();
        setAlign(rt, 1, 0, align);
        setDirect(rt, 1, 0, on);
        expect(src.events == std::vector<Event>{{1, 0, 1, 200}}, "port 1 is its own pad: " + show(src.events));

        // Any non-zero byte drives the small motor on; the large motor takes the byte as is.
        src.events.clear();
        const uint8_t two[6] = {2, 255, 0, 0, 0, 0};
        setDirect(rt, 0, 0, two);
        expect(src.events == std::vector<Event>{{0, 0, 1, 255}}, "small 2 counts as on, large 255: " + show(src.events));

        // A different table: the large motor first, the small one on the third byte.
        src.events.clear();
        const uint8_t align2[6] = {1, 0xFF, 0, 0xFF, 0xFF, 0xFF};
        const uint8_t data2[6] = {77, 99, 1, 0, 0, 0};
        setAlign(rt, 0, 0, align2);
        setDirect(rt, 0, 0, data2);
        expect(!src.events.empty() && src.events.back() == Event{0, 0, 1, 77},
               "align {1,FF,0,...}: large from byte 0, small from byte 2: " + show(src.events));

        // The port closes: the motors go off, once.
        src.events.clear();
        expect(call(rt, ps2_stubs::scePadPortClose, 0, 0) == 1, "PortClose answers 1");
        expect(src.events == std::vector<Event>{{0, 0, 0, 0}}, "a closed port switches the motors off: " + show(src.events));
        src.events.clear();
        call(rt, ps2_stubs::scePadPortClose, 0, 0);
        expect(src.events.empty(), "closing again says nothing: " + show(src.events));

        // Port 1 still holds its values; scePadInit switches it off.
        src.events.clear();
        call(rt, ps2_stubs::scePadInit);
        expect(src.events == std::vector<Event>{{1, 0, 0, 0}}, "scePadInit silences the pad that was shaking: " + show(src.events));

        // After a re-open the align table is the default again: the old table does not survive.
        openBoth(rt);
        setAlign(rt, 0, 0, align2);
        call(rt, ps2_stubs::scePadPortClose, 0, 0);
        call(rt, ps2_stubs::scePadPortOpen, 0, 0, kDma);
        src.events.clear();
        setDirect(rt, 0, 0, on);
        expect(src.events == std::vector<Event>{{0, 0, 1, 200}}, "after a re-open the default table is back: " + show(src.events));

        // scePadEnd silences too.
        setAlign(rt, 1, 0, align);
        setDirect(rt, 1, 0, on);
        src.events.clear();
        call(rt, ps2_stubs::scePadEnd);
        expect(src.events == std::vector<Event>{{0, 0, 0, 0}, {1, 0, 0, 0}}, "scePadEnd silences both pads that were shaking: " + show(src.events));
    }

    void scenarioRefused()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("pad-actuator-test"), "initialize()");
        Source src(true);
        src.connected[1] = false;
        rt.setHostPad(&src.iface);
        call(rt, ps2_stubs::scePadInit);
        const uint8_t align[6] = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t on[6] = {1, 200, 0, 0, 0, 0};

        expect(setAlign(rt, 0, 0, align) == 0 && setDirect(rt, 0, 0, on) == 0, "a pad that is not open answers 0");
        call(rt, ps2_stubs::scePadPortOpen, 1, 0, kDma);
        expect(setAlign(rt, 1, 0, align) == 0 && setDirect(rt, 1, 0, on) == 0, "a pad that is not plugged in answers 0");
        expect(setDirect(rt, 0, 3, on) == 0 && setDirect(rt, 5, 0, on) == 0, "a (port, slot) outside the description answers 0");
        expect(src.events.empty(), "nothing reached the host: " + show(src.events));
    }

    void scenarioNoCallback()
    {
        PS2Runtime rt;
        rt.setHostOwnsWindow(true);
        expect(rt.initialize("pad-actuator-test"), "initialize()");
        Source src(false);
        rt.setHostPad(&src.iface);
        openBoth(rt);
        const uint8_t align[6] = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t on[6] = {1, 200, 0, 0, 0, 0};
        expect(setAlign(rt, 0, 0, align) == 1 && setDirect(rt, 0, 0, on) == 1, "without a callback the calls still answer 1");
        call(rt, ps2_stubs::scePadPortClose, 0, 0);
    }
}

int main()
{
    unsetenv("DISPLAY");
    scenarioMotors();
    scenarioRefused();
    scenarioNoCallback();
    if (g_failures != 0)
    {
        std::fprintf(stderr, "pad_actuator_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("pad_actuator_test: all checks passed\n");
    return 0;
}
