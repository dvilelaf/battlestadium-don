// SPDX-License-Identifier: GPL-3.0-or-later
// Window effects of the setup screens (A3, T-644): the window title follows the progress, and when preparing ends
// the window flashes once and a short chime plays. The decisions are plain code (no SDL), so a test drives them with
// a sequence of views; the chime is generated in code, no asset file.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "setup_screen_model.h"

struct SDL_Window;

namespace don_host
{
// What to do after this frame's view.
struct EffectsDecision
{
    bool set_title = false;     // call SDL_SetWindowTitle(title)
    std::string title;
    bool finished = false;      // flash the window and play the chime, once
};

// Remembers the last title and the last screen. The title is set only when it changes; "finished" is the
// transition from Preparing to Ready, once per transition.
class SetupEffects
{
public:
    EffectsDecision update(const don_setup::View &v)
    {
        EffectsDecision d;
        if (!has_title_ || v.window_title != title_)
        {
            title_ = v.window_title;
            has_title_ = true;
            d.set_title = true;
            d.title = title_;
        }
        d.finished = screen_ == don_setup::Screen::Preparing && v.screen == don_setup::Screen::Ready;
        screen_ = v.screen;
        return d;
    }

private:
    std::string title_;
    bool has_title_ = false;
    don_setup::Screen screen_ = don_setup::Screen::Welcome;
};

constexpr int kChimeRate = 48000;

// The chime as mono 16-bit samples at `rate` Hz: a few sine tones, each with a fade in and out, at a modest volume.
inline std::vector<std::int16_t> chime_samples(int rate)
{
    // Three rising notes (C6, E6, G6: a major triad), each 0.18 s, the last one ringing 0.3 s; a raised-cosine
    // fade over the first and last 20 ms of each note keeps the joins click-free.
    struct Note { double hz, seconds; };
    constexpr Note notes[] = {{1046.50, 0.18}, {1318.51, 0.18}, {1567.98, 0.30}};
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kFadeSeconds = 0.02;
    constexpr double kAmplitude = 0.2 * 32767.0;  // 20 % of full scale: modest
    std::vector<std::int16_t> out;
    for (const Note &n : notes)
    {
        const int count = static_cast<int>(n.seconds * rate), fade = static_cast<int>(kFadeSeconds * rate);
        for (int i = 0; i < count; ++i)
        {
            const int edge = std::min(i, count - 1 - i);
            const double gain = edge >= fade ? 1.0 : 0.5 * (1.0 - std::cos(kPi * edge / fade));
            out.push_back(static_cast<std::int16_t>(kAmplitude * gain * std::sin(2.0 * kPi * n.hz * i / rate)));
        }
    }
    return out;
}

// The SDL audio calls the chime makes. setup_chime.cpp implements them with SDL; the tests use a fake, because
// tests/setup does not link SDL.
class ChimeAudio
{
public:
    virtual ~ChimeAudio() = default;
    virtual bool audio_running() = 0;                                          // SDL_WasInit(SDL_INIT_AUDIO)
    virtual bool init_audio() = 0;                                             // SDL_InitSubSystem(SDL_INIT_AUDIO)
    virtual void quit_audio() = 0;                                             // SDL_QuitSubSystem(SDL_INIT_AUDIO)
    virtual bool open_stream() = 0;                                            // default playback stream, resumed
    virtual void close_stream() = 0;                                           // destroys it
    virtual void queue(const std::vector<std::int16_t> &samples) = 0;
    virtual int queued_bytes() = 0;                                            // bytes not yet played
};

// What the chime holds from SDL: the audio subsystem it started itself (never one another part started) and its
// stream. The stream is opened on the first chime and closed once its samples have drained, so an idle device is
// not held; shutdown() gives back both.
class Chime
{
public:
    explicit Chime(ChimeAudio &audio) : audio_(audio) {}

    // Plays one chime. Without audio it does nothing, and says nothing.
    void play()
    {
        if (!stream_open_)
        {
            if (!audio_.audio_running())
            {
                if (!audio_.init_audio())
                    return;
                ++audio_inits_;
            }
            if (!audio_.open_stream())
            {
                release_audio();
                return;
            }
            stream_open_ = true;
        }
        audio_.queue(chime_samples(kChimeRate));
    }

    // Once per frame: closes the stream when its samples have drained, and gives back the audio if this chime started it.
    void poll()
    {
        if (stream_open_ && audio_.queued_bytes() == 0)
        {
            audio_.close_stream();
            stream_open_ = false;
            release_audio();
        }
    }

    // Closes the stream and gives back the audio if this chime started it.
    void shutdown()
    {
        if (stream_open_)
        {
            audio_.close_stream();
            stream_open_ = false;
        }
        release_audio();
    }

    int audio_inits() const { return audio_inits_; }
    bool stream_open() const { return stream_open_; }

private:
    void release_audio()
    {
        if (audio_inits_ > 0)
        {
            audio_.quit_audio();
            audio_inits_ = 0;
        }
    }

    ChimeAudio &audio_;
    int audio_inits_ = 0;
    bool stream_open_ = false;
};

// Plays the chime through SDL audio when a device opens; does nothing, and says nothing, when audio is missing.
void chime_play();
// Once per frame: closes the chime's stream when its samples have drained.
void chime_poll();
// The setup screens are over: closes the chime's stream and gives back the audio the chime started.
void chime_shutdown();

// Once per frame with the view being drawn: sets the window title when it changed and, on the transition to done,
// flashes the window until focused and plays the chime. With DON_TRACE_SETUP=1 it prints "[don] window title: ..."
// and "[don] flash".
void setup_screen_effects(SDL_Window *window, const don_setup::View &view);

// The setup screens end (setup_screen.cpp): gives the chime's audio back to the game.
void setup_screen_end();

} // namespace don_host
