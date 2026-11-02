// T-308: vibration on the host side without SDL: the game's two motors as SDL's command, the schedule that renews
// it while the motors hold, and the pad table's store for what the runtime's actuator callback reports.
#include "check.h"
#include "input.h"
#include "pad_table.h"

using namespace input;

int main()
{
    // ---- the command ------------------------------------------------------------------------------------
    RumbleCommand c = rumble_command(0, 0);
    CHECK_EQ(c.low_frequency, 0);
    CHECK_EQ(c.high_frequency, 0);
    CHECK_EQ(c.duration_ms, 0);

    c = rumble_command(1, 0);
    CHECK_EQ(c.low_frequency, 0);
    CHECK_EQ(c.high_frequency, 0xFFFF);
    CHECK_EQ(c.duration_ms, RumbleSchedule::kDurationMs);

    c = rumble_command(0, 255);
    CHECK_EQ(c.low_frequency, 0xFFFF);
    CHECK_EQ(c.high_frequency, 0);
    CHECK(c.duration_ms != 0);

    c = rumble_command(0, 1);
    CHECK_EQ(c.low_frequency, 257);
    CHECK(c.duration_ms != 0); // the weakest large motor still runs

    c = rumble_command(2, 128); // any non-zero small byte is "on"
    CHECK_EQ(c.high_frequency, 0xFFFF);
    CHECK_EQ(c.low_frequency, 128 * 257);

    // ---- the schedule -----------------------------------------------------------------------------------
    RumbleSchedule s;
    RumbleCommand out;
    CHECK(!s.update(0, 0, 0, out));      // nothing set: nothing sent
    CHECK(!s.update(5000, 0, 0, out));
    CHECK(s.update(5010, 0, 200, out)); // a change is sent at once
    CHECK_EQ(out.low_frequency, 200 * 257);
    CHECK_EQ(out.duration_ms, RumbleSchedule::kDurationMs);
    CHECK(!s.update(5011, 0, 200, out)); // held: not again before the renewal
    CHECK(!s.update(5010 + RumbleSchedule::kRenewMs - 1, 0, 200, out));
    CHECK(s.update(5010 + RumbleSchedule::kRenewMs, 0, 200, out)); // renewed before the 1000 ms run out
    CHECK_EQ(out.low_frequency, 200 * 257);
    CHECK(RumbleSchedule::kRenewMs < RumbleSchedule::kDurationMs);
    CHECK(s.update(6000, 1, 200, out)); // the small motor joins: a new command with both
    CHECK_EQ(out.high_frequency, 0xFFFF);
    CHECK_EQ(out.low_frequency, 200 * 257);
    CHECK(!s.update(6001, 2, 200, out)); // small 2 is still "on": no change
    CHECK(s.update(6002, 0, 0, out));    // the stop is a command: both 0, duration 0
    CHECK_EQ(out.low_frequency, 0);
    CHECK_EQ(out.high_frequency, 0);
    CHECK_EQ(out.duration_ms, 0);
    CHECK(!s.update(9000, 0, 0, out)); // a stopped pad is not poked again
    CHECK(!s.update(20000, 0, 0, out));

    s.update(30000, 1, 0, out);
    s.reset(); // a player's gamepad changed: the schedule starts again
    CHECK(!s.update(30001, 0, 0, out));
    CHECK(s.update(30002, 1, 0, out));

    // ---- the pad table's store -----------------------------------------------------------------------------
    PadTable t(2u, 1u);
    uint8_t sm = 9, lg = 9;
    t.actuators(0, 0, sm, lg);
    CHECK_EQ(sm, 0);
    CHECK_EQ(lg, 0);

    // Through the runtime's callback, the way the runtime calls it.
    const Ps2HostPad &src = t.source();
    CHECK(src.actuator != nullptr);
    src.actuator(src.user, 0, 0, 1, 200);
    src.actuator(src.user, 1, 0, 0, 77);
    t.actuators(0, 0, sm, lg);
    CHECK_EQ(sm, 1);
    CHECK_EQ(lg, 200);
    t.actuators(1, 0, sm, lg);
    CHECK_EQ(sm, 0);
    CHECK_EQ(lg, 77);
    src.actuator(src.user, 0, 0, 0, 0);
    t.actuators(0, 0, sm, lg);
    CHECK_EQ(sm, 0);
    CHECK_EQ(lg, 0);

    // A (port, slot) outside the description is ignored and reads as 0.
    src.actuator(src.user, 0, 1, 1, 255);
    src.actuator(src.user, 2, 0, 1, 255);
    t.actuators(0, 1, sm, lg);
    CHECK_EQ(lg, 0);
    t.actuators(2, 0, sm, lg);
    CHECK_EQ(lg, 0);

    // The multitap shape: four slots on port 0.
    PadTable m(1u, 4u);
    m.source().actuator(m.source().user, 0, 3, 1, 10);
    m.actuators(0, 3, sm, lg);
    CHECK_EQ(sm, 1);
    CHECK_EQ(lg, 10);
    m.actuators(0, 2, sm, lg);
    CHECK_EQ(lg, 0);

    return check_result("test_input_rumble");
}
