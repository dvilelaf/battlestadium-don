// SPDX-License-Identifier: GPL-3.0-or-later
// The setup screens' end-of-preparing chime: see setup_chime.h. Setup screens only; the game's audio path is not used.
#include "setup_chime.h"

#include <SDL3/SDL.h>

namespace don_host
{
namespace
{
// The chime's SDL calls. The stream is destroyed once its samples have drained (see Chime::poll).
class SdlChimeAudio final : public ChimeAudio
{
public:
    bool audio_running() override { return SDL_WasInit(SDL_INIT_AUDIO) != 0; }
    bool init_audio() override { return SDL_InitSubSystem(SDL_INIT_AUDIO); }
    void quit_audio() override { SDL_QuitSubSystem(SDL_INIT_AUDIO); }

    bool open_stream() override
    {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kChimeRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_)
            return false;
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    void close_stream() override
    {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }

    void queue(const std::vector<std::int16_t> &samples) override
    {
        SDL_PutAudioStreamData(stream_, samples.data(), static_cast<int>(samples.size() * sizeof(std::int16_t)));
    }

    // SDL returns -1 on error; an invalid stream has nothing queued either.
    int queued_bytes() override { return std::max(0, SDL_GetAudioStreamQueued(stream_)); }

private:
    SDL_AudioStream *stream_ = nullptr;
};

SdlChimeAudio g_audio;
Chime g_chime(g_audio);
} // namespace

void chime_play()
{
    g_chime.play();
}

void chime_poll()
{
    g_chime.poll();
}

void chime_shutdown()
{
    g_chime.shutdown();
}

} // namespace don_host
