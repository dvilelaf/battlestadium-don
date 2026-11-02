// T-112: parser and player of the input scripts.
#include "check.h"
#include "input_script.h"

#include <algorithm>
#include <vector>

#include <string>

using namespace input;

namespace
{
bool pressed(const Ps2HostPadState &s, PadButton b) { return is_pad_button_pressed(s, b); }

ParseResult parse_ok(const std::string &text)
{
    ParseResult r = parse_script(text);
    if (!r.ok)
        for (const auto &e : r.errors)
            std::fprintf(stderr, "unexpected parse error: %s\n", e.to_string().c_str());
    CHECK(r.ok);
    return r;
}

// Parses text that must be wrong; returns the first error (and checks there is nothing to execute).
ScriptError parse_bad(const std::string &text)
{
    ParseResult r = parse_script(text);
    CHECK(!r.ok);
    CHECK(r.script.empty());
    CHECK(!r.errors.empty());
    return r.errors.empty() ? ScriptError{} : r.errors[0];
}

void test_every_command()
{
    const ParseResult r = parse_ok("600 tap START 6\n"
                                   "900 press DOWN\n"
                                   "906 release DOWN\n"
                                   "1200 shot menu\n"
                                   "1300 stick LEFT 1 -0.5\n"
                                   "1500 quit\n");
    const auto &c = r.script.commands();
    CHECK_EQ(c.size(), 6);
    if (c.size() != 6)
        return;
    CHECK(c[0].kind == ScriptCommand::Kind::Tap && c[0].frame == 600 && c[0].button == PadButton::Start &&
          c[0].frames == 6 && c[0].player == 0 && c[0].line == 1);
    CHECK(c[1].kind == ScriptCommand::Kind::Press && c[1].button == PadButton::Down && c[1].frame == 900);
    CHECK(c[2].kind == ScriptCommand::Kind::Release && c[2].button == PadButton::Down && c[2].frame == 906);
    CHECK(c[3].kind == ScriptCommand::Kind::Shot && c[3].name == "menu" && c[3].frame == 1200);
    CHECK(c[4].kind == ScriptCommand::Kind::Stick && !c[4].right_stick && c[4].x == 1.f && c[4].y == -0.5f);
    CHECK(c[5].kind == ScriptCommand::Kind::Quit && c[5].frame == 1500);
    CHECK_EQ(r.script.last_frame(), 1500);
    CHECK_EQ(r.script.players_used(), 1);
}

void test_comments_and_blanks()
{
    const ParseResult r = parse_ok("# header\n"
                                   "\n"
                                   "   \t\n"
                                   "  10   tap   cross   3   # trailing comment\r\n"
                                   "      # indented comment\n"
                                   "20 Quit");
    CHECK_EQ(r.script.commands().size(), 2);
    CHECK(r.script.commands()[0].button == PadButton::Cross); // names are case-insensitive
    CHECK_EQ(r.script.commands()[0].line, 4);                 // line numbers count comments and blanks
    CHECK_EQ(r.script.commands()[1].line, 6);
    CHECK(parse_script("").ok);
    CHECK(parse_script("# only a comment\n").ok);
}

void test_player_prefix()
{
    // A prefix anywhere but right after the frame is an error.
    CHECK_EQ(parse_bad("40 stick P2: RIGHT 0 1\n").line, 1);
    ParseResult r2 = parse_ok("10 P2: tap UP 4\n10 P3:press DOWN\n20 p4: release DOWN\n30 P1: tap CIRCLE 2\n");
    const auto &c = r2.script.commands();
    CHECK_EQ(c.size(), 4);
    if (c.size() == 4)
    {
        CHECK_EQ(c[0].player, 1);
        CHECK_EQ(c[1].player, 2);
        CHECK_EQ(c[2].player, 3);
        CHECK_EQ(c[3].player, 0);
    }
    CHECK_EQ(r2.script.players_used(), 0xF);
}

void test_relative_frames()
{
    ParseResult r2 = parse_ok("+10 press UP\n"   // 10
                              "+5 release UP\n"  // 15
                              "+0 shot same\n"   // 15
                              "200 tap CROSS 2\n"
                              "+30 quit\n"       // 230
                              "50 shot absolute_after_relative\n"
                              "+1 shot rel_to_50\n"); // 51
    const auto &c = r2.script.commands();
    CHECK_EQ(c.size(), 7);
    // Sorted by frame (stable).
    uint64_t prev = 0;
    for (const auto &x : c)
    {
        CHECK(x.frame >= prev);
        prev = x.frame;
    }
    CHECK_EQ(c[0].frame, 10);
    CHECK_EQ(c[1].frame, 15);
    CHECK_EQ(c[2].frame, 15);
    CHECK_EQ(c[2].name == "same", 1);
    CHECK_EQ(c[3].frame, 50);
    CHECK_EQ(c[4].frame, 51);
    CHECK_EQ(c[5].frame, 200);
    CHECK_EQ(c[6].frame, 230);
}

void test_errors()
{
    ScriptError e;

    e = parse_bad("10 quit\n20 jump UP\n");
    CHECK_EQ(e.line, 2);
    CHECK(e.message.find("unknown command 'jump'") != std::string::npos);
    CHECK(e.to_string().rfind("line 2: ", 0) == 0);

    e = parse_bad("\n# c\n10 tap FOO 5\n");
    CHECK_EQ(e.line, 3);
    CHECK(e.message.find("unknown button 'FOO'") != std::string::npos);

    e = parse_bad("tap START 6\n");
    CHECK_EQ(e.line, 1);
    CHECK(e.message.find("frame") != std::string::npos);

    e = parse_bad("10 tap START\n");
    CHECK_EQ(e.line, 1);
    CHECK(e.message.find("needs") != std::string::npos);

    e = parse_bad("10 tap START 6 7\n");
    CHECK(e.message.find("extra argument '7'") != std::string::npos);

    e = parse_bad("10 tap START 0\n");
    CHECK(e.message.find("at least 1") != std::string::npos);
    e = parse_bad("10 tap START -3\n");
    CHECK(e.message.find("at least 1") != std::string::npos);
    e = parse_bad("10 tap START six\n");
    CHECK(e.message.find("at least 1") != std::string::npos);

    e = parse_bad("10 press\n");
    CHECK(e.message.find("needs a button") != std::string::npos);
    e = parse_bad("10 quit now\n");
    CHECK(e.message.find("extra argument") != std::string::npos);
    e = parse_bad("10\n");
    CHECK(e.message.find("missing command") != std::string::npos);
    e = parse_bad("1x0 quit\n");
    CHECK(e.message.find("frame") != std::string::npos);
    e = parse_bad("-5 quit\n");
    CHECK(e.message.find("frame") != std::string::npos);
    e = parse_bad("10 P5: tap UP 2\n");
    CHECK(e.message.find("P5:") != std::string::npos);
    e = parse_bad("10 P0: tap UP 2\n");
    CHECK(e.message.find("P0:") != std::string::npos);
    e = parse_bad("10 P2:\n");
    CHECK(e.message.find("missing command") != std::string::npos);
    e = parse_bad("10 P2: shot a\n");
    CHECK(e.message.find("not per player") != std::string::npos);
    e = parse_bad("10 P2: quit\n");
    CHECK(e.message.find("not per player") != std::string::npos);
    e = parse_bad("10 shot ../x\n");
    CHECK(e.message.find("invalid shot name") != std::string::npos);
    e = parse_bad("10 shot .hidden\n");
    CHECK(e.message.find("invalid shot name") != std::string::npos);
    e = parse_bad("10 shot\n");
    CHECK(e.message.find("needs a name") != std::string::npos);
    e = parse_bad("10 stick MIDDLE 0 0\n");
    CHECK(e.message.find("LEFT or RIGHT") != std::string::npos);
    e = parse_bad("10 stick LEFT 2 0\n");
    CHECK(e.message.find("between -1 and 1") != std::string::npos);
    e = parse_bad("10 stick LEFT 0\n");
    CHECK(e.message.find("needs") != std::string::npos);
    e = parse_bad("10 stick LEFT a b\n");
    CHECK(e.message.find("between -1 and 1") != std::string::npos);

    // Every error is reported, with its own line, and a script with errors yields nothing to run.
    ParseResult r = parse_script("10 quit\n20 bad\n30 tap UP\n40 tap UP 2\n50 press NOPE\n");
    CHECK(!r.ok);
    CHECK(r.script.empty());
    CHECK_EQ(r.errors.size(), 3);
    if (r.errors.size() == 3)
    {
        CHECK_EQ(r.errors[0].line, 2);
        CHECK_EQ(r.errors[1].line, 3);
        CHECK_EQ(r.errors[2].line, 5);
    }

    // Unreadable file.
    r = parse_script_file("this/file/does/not/exist.txt");
    CHECK(!r.ok);
    CHECK_EQ(r.errors.size(), 1);
}

void test_player_sequence()
{
    // The example of the task card, shortened.
    const ParseResult r = parse_ok("2 tap START 3\n"
                                   "6 press DOWN\n"
                                   "9 release DOWN\n"
                                   "10 shot menu\n"
                                   "12 quit\n");
    ScriptPlayer p(r.script);
    const bool start[] = {false, false, true, true, true, false, false, false, false, false, false, false, false};
    const bool down[] = {false, false, false, false, false, false, true, true, true, false, false, false, false};
    for (uint64_t f = 1; f <= 12; ++f)
    {
        const FrameActions a = p.step(f);
        if (pressed(a.pads[0], PadButton::Start) != start[f] || pressed(a.pads[0], PadButton::Down) != down[f])
            std::fprintf(stderr, "frame %llu: start=%d down=%d\n", static_cast<unsigned long long>(f),
                         pressed(a.pads[0], PadButton::Start), pressed(a.pads[0], PadButton::Down));
        CHECK_EQ(pressed(a.pads[0], PadButton::Start), start[f]);
        CHECK_EQ(pressed(a.pads[0], PadButton::Down), down[f]);
        CHECK_EQ(a.shots.size(), f == 10 ? 1 : 0);
        if (f == 10 && a.shots.size() == 1)
            CHECK(a.shots[0] == "menu");
        CHECK_EQ(a.quit, f >= 12);
        // The pressure byte follows the button.
        CHECK_EQ(a.pads[0].pressure[pressure_index(PadButton::Down)], down[f] ? 0xFF : 0);
        CHECK_EQ(a.pads[0].has_pressure, 1);
        CHECK_EQ(a.pads[0].connected, 1);
        // Players that the script does not mention are not plugged in.
        CHECK_EQ(a.pads[1].connected, 0);
        CHECK_EQ(a.pads[3].connected, 0);
    }
    // quit stays true afterwards.
    CHECK(p.step(13).quit);
}

void test_overlapping_taps_and_release_without_press()
{
    // Two taps of the same button overlap: held from 5 to 14 without a gap.
    const ParseResult r = parse_ok("5 tap CROSS 5\n"
                                   "8 tap CROSS 7\n"
                                   "20 release UP\n" // release without a press: harmless
                                   "22 press UP\n"
                                   "22 release UP\n"  // press and release on the same frame: file order wins
                                   "30 press UP\n"
                                   "31 tap UP 2\n"    // a tap does not end a press
                                   "40 release UP\n");
    ScriptPlayer p(r.script);
    for (uint64_t f = 1; f <= 45; ++f)
    {
        const FrameActions a = p.step(f);
        const bool cross = f >= 5 && f < 15;
        const bool up = (f >= 30 && f < 40);
        if (pressed(a.pads[0], PadButton::Cross) != cross || pressed(a.pads[0], PadButton::Up) != up)
            std::fprintf(stderr, "frame %llu\n", static_cast<unsigned long long>(f));
        CHECK_EQ(pressed(a.pads[0], PadButton::Cross), cross);
        CHECK_EQ(pressed(a.pads[0], PadButton::Up), up);
    }
}

void test_four_players()
{
    const ParseResult r = parse_ok("2 P3: tap CIRCLE 2\n5 P4: press START\n");
    CHECK_EQ(r.script.players_used(), 12);
    ScriptPlayer p(r.script);
    for (uint64_t f = 1; f <= 7; ++f)
    {
        const FrameActions a = p.step(f);
        CHECK_EQ(pressed(a.pads[2], PadButton::Circle), f == 2 || f == 3);
        CHECK_EQ(pressed(a.pads[3], PadButton::Start), f >= 5);
        CHECK_EQ(pressed(a.pads[2], PadButton::Start), 0);
        CHECK_EQ(pressed(a.pads[3], PadButton::Circle), 0);
        CHECK_EQ(a.pads[0].connected, 0);
        CHECK_EQ(a.pads[1].connected, 0);
        CHECK_EQ(a.pads[2].connected, 1);
        CHECK_EQ(a.pads[3].connected, 1);
    }
}

void test_two_players_and_sticks()
{
    const ParseResult r = parse_ok("3 P2: tap CIRCLE 2\n"
                                   "3 tap CROSS 1\n"
                                   "4 P2: stick LEFT 1 -1\n"
                                   "4 stick RIGHT 0 1\n"
                                   "6 P2: stick LEFT 0 0\n");
    CHECK_EQ(r.script.players_used(), 3);
    ScriptPlayer p(r.script);
    for (uint64_t f = 1; f <= 7; ++f)
    {
        const FrameActions a = p.step(f);
        CHECK_EQ(pressed(a.pads[1], PadButton::Circle), f == 3 || f == 4);
        CHECK_EQ(pressed(a.pads[0], PadButton::Circle), 0);
        CHECK_EQ(pressed(a.pads[0], PadButton::Cross), f == 3);
        CHECK_EQ(pressed(a.pads[1], PadButton::Cross), 0);
        CHECK_EQ(a.pads[1].connected, 1);
        CHECK_EQ(a.pads[2].connected, 0);
        if (f < 4)
        {
            CHECK_EQ(a.pads[1].lx, 0x80);
        }
        else if (f < 6)
        {
            CHECK_EQ(a.pads[1].lx, 0xFF);
            CHECK_EQ(a.pads[1].ly, 0x00);
            CHECK_EQ(a.pads[0].ry, 0xFF);
            CHECK_EQ(a.pads[0].rx, 0x80);
        }
        else
        {
            CHECK_EQ(a.pads[1].lx, 0x80);
            CHECK_EQ(a.pads[1].ly, 0x80);
            CHECK_EQ(a.pads[0].ry, 0xFF); // P1's right stick stays until changed
        }
    }
}

void test_relative_sequence()
{
    const ParseResult r = parse_ok("+10 tap START 2\n"
                                   "+10 shot a\n"
                                   "+10 quit\n"); // 10, 20, 30
    ScriptPlayer p(r.script);
    for (uint64_t f = 1; f <= 31; ++f)
    {
        const FrameActions a = p.step(f);
        CHECK_EQ(pressed(a.pads[0], PadButton::Start), f == 10 || f == 11);
        CHECK_EQ(a.shots.size(), f == 20 ? 1 : 0);
        CHECK_EQ(a.quit, f >= 30);
    }
}

void test_skipped_frames_and_repeats()
{
    const ParseResult r = parse_ok("5 shot a\n7 shot b\n9 press UP\n");
    ScriptPlayer p(r.script);
    FrameActions a = p.step(8); // skips 5 and 7: both shots are reported now, once
    CHECK_EQ(a.shots.size(), 2);
    CHECK(!pressed(a.pads[0], PadButton::Up));
    a = p.step(8);
    CHECK_EQ(a.shots.size(), 0);
    a = p.step(3); // going back is the same frame as the last
    CHECK_EQ(a.shots.size(), 0);
    a = p.step(10);
    CHECK(pressed(a.pads[0], PadButton::Up));
}

void test_merge_with_device()
{
    const ParseResult r = parse_ok("1 press CIRCLE\n");
    ScriptPlayer p(r.script);
    const FrameActions a = p.step(1);
    KeySet keys;
    keys.set(Key::ArrowDown);
    const Ps2HostPadState m = merge_pad_states(a.pads[0], keyboard_to_pad_state(keys));
    CHECK(pressed(m, PadButton::Circle));
    CHECK(pressed(m, PadButton::Down));
    CHECK(!pressed(m, PadButton::Cross));
    // A script pad that is not connected adds nothing.
    const Ps2HostPadState m2 = merge_pad_states(a.pads[2], keyboard_to_pad_state(keys));
    CHECK(pressed(m2, PadButton::Down));
    CHECK(!pressed(m2, PadButton::Circle));
}

void test_example_script_file()
{
    const std::filesystem::path path = std::filesystem::path(DON_REPO_ROOT) / "tools" / "run" / "scripts" / "to_main_menu.txt";
    const ParseResult r = parse_script_file(path);
    if (!r.ok)
        for (const auto &e : r.errors)
            std::fprintf(stderr, "%s: %s\n", path.string().c_str(), e.to_string().c_str());
    CHECK(r.ok);
    if (!r.ok)
        return;
    // What the route is meant to be, without pinning its numbers (it has been retimed twice already):
    // it parses, it takes the two shots of that screen in that order, it ends with quit, it holds at
    // least three buttons, and it ends somewhere sensible in game time (60 frames are one console
    // second: the route used to end at 2-5 minutes and now ends at ~55 s, because the port no longer
    // waits out the logos and the video).
    int taps = 0;
    std::vector<std::string> shots;
    for (const auto &c : r.script.commands())
    {
        if (c.kind == ScriptCommand::Kind::Tap)
            ++taps;
        else if (c.kind == ScriptCommand::Kind::Shot)
            shots.push_back(c.name);
    }
    CHECK_EQ(r.script.players_used(), 1);
    CHECK(taps >= 3);
    CHECK(!r.script.commands().empty());
    CHECK(r.script.commands().back().kind == ScriptCommand::Kind::Quit);
    const auto title = std::find(shots.begin(), shots.end(), "title");
    const auto menu = std::find(shots.begin(), shots.end(), "main_menu");
    CHECK(title != shots.end());
    CHECK(menu != shots.end());
    CHECK(title < menu);
    CHECK(r.script.last_frame() >= 1800 && r.script.last_frame() <= 18000);
}
} // namespace

int main()
{
    test_every_command();
    test_comments_and_blanks();
    test_player_prefix();
    test_relative_frames();
    test_errors();
    test_player_sequence();
    test_overlapping_taps_and_release_without_press();
    test_two_players_and_sticks();
    test_four_players();
    test_relative_sequence();
    test_skipped_frames_and_repeats();
    test_merge_with_device();
    test_example_script_file();
    return check_result("test_input_script");
}
