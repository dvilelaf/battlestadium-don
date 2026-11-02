// T-303: the SDL3 part of src/host/audio.{h,cpp}. Built and run in the build container with SDL's dummy
// driver (never a real device, even on a machine that has one): a headless run (no device, only the WAV),
// a run that opens the stream, feeds one second and closes, and a run that floods the queue so the bound
// and the drop policy are seen.
#include "audio.h"
#include "check.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
std::vector<float> tone(uint32_t frames)
{
    std::vector<float> lr(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float v = 0.25f * std::sin(2.0 * 3.14159265358979 * 440.0 * i / double(kAudioSampleRate));
        lr[size_t(i) * 2] = v;
        lr[size_t(i) * 2 + 1] = v;
    }
    return lr;
}

void feed(HostAudio &audio, const std::vector<float> &lr)
{
    const uint32_t frames = static_cast<uint32_t>(lr.size() / 2);
    for (uint32_t done = 0; done < frames;)
    {
        const uint32_t chunk = std::min<uint32_t>(512, frames - done);
        audio.write(lr.data() + size_t(done) * 2, chunk);
        done += chunk;
    }
}
} // namespace

int main()
{
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    const std::filesystem::path wav = std::filesystem::temp_directory_path() / "don_audio_sdl_test.wav";
    const std::vector<float> second = tone(kAudioSampleRate);

    // 1. Headless: nothing is opened, the frames go to the WAV only.
    {
        std::error_code ec;
        std::filesystem::remove(wav, ec);
        HostAudio audio;
        AudioOutputParams params;
        params.open_device = false;
        params.wav_path = wav;
        std::string err;
        CHECK(audio.init(params, err));
        CHECK(!audio.device_open());
        feed(audio, second);
        CHECK_EQ(audio.frames_written(), kAudioSampleRate);
        CHECK_EQ(audio.frames_dropped(), 0);
        audio.shutdown();
        CHECK(std::filesystem::file_size(wav, ec) == 44 + uint64_t(kAudioSampleRate) * 4);
    }

    // 2. With the device: the stream opens, takes one second and closes.
    {
        HostAudio audio;
        AudioOutputParams params;
        params.open_device = true;
        std::string err;
        CHECK(audio.init(params, err));
        CHECK(audio.device_open());
        feed(audio, second);
        CHECK_EQ(audio.frames_written(), kAudioSampleRate);
        audio.shutdown();
        CHECK(!audio.device_open());
    }

    // 3. Faster than real time: the queue is bounded and the oldest frames are dropped, never unbounded
    //    growth (the ring holds 1024 frames, SDL's queue is fed up to 256).
    {
        HostAudio audio;
        AudioOutputParams params;
        params.open_device = true;
        params.ring_frames = 1024;
        params.device_queue_frames = 256;
        std::string err;
        CHECK(audio.init(params, err));
        CHECK(audio.device_open());
        // 10 seconds of audio as fast as the loop can produce it.
        for (int i = 0; i < 10; ++i)
            feed(audio, second);
        CHECK_EQ(audio.frames_written(), 10 * uint64_t(kAudioSampleRate));
        CHECK(audio.frames_dropped() > 1000);
        audio.shutdown();
    }

    // 4. A WAV that cannot be created is the one fatal error (the caller has to notice a bad --wav): the
    //    parent folder is created by the writer, so a path *under a regular file* is what cannot work.
    {
        const std::filesystem::path blocker = std::filesystem::temp_directory_path() / "don_audio_sdl_blocker";
        { std::FILE *f = std::fopen(blocker.string().c_str(), "wb"); if (f) std::fclose(f); }
        HostAudio audio;
        AudioOutputParams params;
        params.wav_path = blocker / "audio.wav";
        std::string err;
        CHECK(!audio.init(params, err));
        CHECK(!err.empty());
        audio.shutdown();
        std::error_code ec;
        std::filesystem::remove(blocker, ec);
    }

    std::error_code ec;
    std::filesystem::remove(wav, ec);
    return check_result("test_audio_sdl");
}
