// SPDX-License-Identifier: GPL-3.0-or-later
// T-644: when the window title is set, when the window flashes and the chime plays, the chime samples, and the
// battery state read from a fake sysfs. No SDL window: the decision code is plain C++.
#include "../../src/host/setup_screen_model.cpp"
#include "../../src/host/setup_flow_model.cpp"
#include "../../src/host/setup_chime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "platform/machine.h"

namespace
{
int failures = 0;
void check(bool ok, const char *what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

don_setup::View view(don_setup::Screen s, const char *title)
{
    don_setup::View v;
    v.screen = s;
    v.window_title = title;
    return v;
}

void write(const std::filesystem::path &p, const char *text)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text << "\n";
}
} // namespace

int main()
{
    using don_setup::Screen;
    {
        don_host::SetupEffects fx;
        int titles = 0, finishes = 0;
        const char *seq[][2] = {{"w", "Battle Stadium D.O.N"}, {"p", "1 % · Preparing — Battle Stadium D.O.N"},
                                {"p", "1 % · Preparing — Battle Stadium D.O.N"}, {"p", "2 % · Preparing — Battle Stadium D.O.N"},
                                {"p", "2 % · Preparing — Battle Stadium D.O.N"}, {"r", "Battle Stadium D.O.N"},
                                {"r", "Battle Stadium D.O.N"}, {"r", "Battle Stadium D.O.N"}};
        for (const auto &s : seq)
        {
            const Screen sc = s[0][0] == 'w' ? Screen::Welcome : s[0][0] == 'p' ? Screen::Preparing : Screen::Ready;
            const auto d = fx.update(view(sc, s[1]));
            titles += d.set_title;
            finishes += d.finished;
        }
        check(titles == 4, "the title is set only when it changes (welcome, 1 %, 2 %, ready)");
        check(finishes == 1, "flash and chime once, on the transition Preparing -> Ready");
    }
    {
        don_host::SetupEffects fx;
        fx.update(view(Screen::Welcome, "a"));
        check(!fx.update(view(Screen::Ready, "a")).finished, "Ready without preparing is not a finish");
    }
    {
        const auto s = don_host::chime_samples(48000);
        int peak = 0;
        for (auto x : s) peak = std::max(peak, std::abs(static_cast<int>(x)));
        check(s.size() > 4800 && s.size() < 48000 * 2, "the chime is short");
        check(peak > 1000 && peak < 16000, "the chime volume is modest");
        check(s.front() == 0 && std::abs(s.back()) < 50, "the chime fades in and out");
    }
#ifndef _WIN32   // read_power reads a fake sysfs tree on Linux; Windows asks GetSystemPowerStatus (test_platform_win32)
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "t644-sysfs";
        std::filesystem::remove_all(root);
        write(root / "sys/class/power_supply/BAT0/type", "Battery");
        write(root / "sys/class/power_supply/BAT0/status", "Discharging");
        write(root / "sys/class/power_supply/BAT0/capacity", "63");
        write(root / "sys/class/power_supply/AC/type", "Mains");
        write(root / "sys/class/power_supply/AC/online", "0");
        const auto p = don_setup::read_power(root.string());
        check(p.on_battery && p.battery_present && p.percent == 63, "a discharging battery gives on_battery");
        write(root / "sys/class/power_supply/AC/online", "1");
        check(!don_setup::read_power(root.string()).on_battery, "mains online is not on battery");
        std::filesystem::remove_all(root);
    }
#endif
    {
        // The chime's SDL calls, faked: what it opens, starts and gives back.
        struct FakeAudio : don_host::ChimeAudio
        {
            bool running = false, audio_ok = true, stream_ok = true;
            int inits = 0, quits = 0, opens = 0, closes = 0, bytes = 0;
            bool audio_running() override { return running; }
            bool init_audio() override { ++inits; running = audio_ok; return audio_ok; }
            void quit_audio() override { ++quits; running = false; }
            bool open_stream() override { ++opens; return stream_ok; }
            void close_stream() override { ++closes; }
            void queue(const std::vector<std::int16_t> &s) override { bytes += static_cast<int>(s.size() * 2); }
            int queued_bytes() override { return bytes; }
        };
        {
            FakeAudio a;
            don_host::Chime c(a);
            c.play();
            c.play();
            check(a.inits == 1 && a.opens == 1, "the first chime opens the audio and one stream, the second reuses them");
            check(c.audio_inits() == 1 && c.stream_open(), "the chime holds its audio init and its stream");
            c.shutdown();
            check(c.audio_inits() == 0 && !c.stream_open(), "shutdown gives back the audio init and the stream");
            check(a.quits == 1 && a.closes == 1, "shutdown quits the audio it started and closes the stream");
        }
        {
            FakeAudio a;
            a.running = true;  // someone else started the audio
            don_host::Chime c(a);
            c.play();
            c.shutdown();
            check(a.inits == 0 && a.quits == 0, "audio another part started is never initialised or quit by the chime");
        }
        {
            FakeAudio a;
            a.audio_ok = false;
            don_host::Chime c(a);
            c.play();
            check(a.opens == 0 && !c.stream_open() && c.audio_inits() == 0, "no audio: no stream, no error");
        }
        {
            FakeAudio a;
            a.stream_ok = false;
            don_host::Chime c(a);
            c.play();
            check(c.audio_inits() == 0 && a.quits == 1, "a stream that cannot open gives the audio back");
        }
        {
            FakeAudio a;
            don_host::Chime c(a);
            c.play();
            c.poll();
            check(c.stream_open(), "the stream stays open while its samples play");
            a.bytes = 0;  // drained
            c.poll();
            check(!c.stream_open() && c.audio_inits() == 0 && a.quits == 1,
                  "once drained, the stream closes and the audio the chime started is given back");
            c.play();
            check(a.inits == 2 && c.stream_open(), "a chime after a drain starts the audio again");
        }
    }
    if (failures == 0) std::printf("ok\n");
    return failures ? 1 : 0;
}
