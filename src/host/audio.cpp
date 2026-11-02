// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

#include "don_spu2.h" // DON_SPU2_SAMPLE_RATE: the WAV and the stream use the SPU2's rate

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(DON_AUDIO_SDL)
#include <SDL3/SDL.h>
#endif

static_assert(kAudioSampleRate == DON_SPU2_SAMPLE_RATE, "the WAV and the SPU2 output must use one rate");
static_assert(kAudioChannels == 2, "the SPU2 output is stereo");

namespace
{
    bool env_flag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && value[0] == '1';
    }

    void put_u32le(std::FILE *file, uint32_t value)
    {
        uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
        std::fwrite(bytes, 1, sizeof(bytes), file);
    }

    void put_u16le(std::FILE *file, uint16_t value)
    {
        uint8_t bytes[2] = {uint8_t(value), uint8_t(value >> 8)};
        std::fwrite(bytes, 1, sizeof(bytes), file);
    }

    // The RIFF header of a 16-bit stereo PCM WAV; 36 bytes before the data chunk.
    constexpr uint32_t kWavHeaderBytes = 44;
    // A WAV's sizes are 32-bit; stop well before the file gets that big (and before a 32-bit ftell
    // overflows on Windows).
    constexpr uint64_t kWavMaxDataBytes = 0x7FFF0000u;
} // namespace

int16_t float_to_i16(float value)
{
    if (std::isnan(value))
        value = 0.0f;
    if (value < -1.0f)
        value = -1.0f;
    else if (value > 1.0f)
        value = 1.0f;
    return static_cast<int16_t>(std::lrintf(value * 32767.0f));
}

// ---------------------------------------------------------------------------------------------------
// AudioRing

AudioRing::AudioRing(uint32_t capacity_frames)
    : m_data(size_t(capacity_frames ? capacity_frames : kDefaultRingFrames) * kAudioChannels, 0.0f),
      m_capacity(capacity_frames ? capacity_frames : kDefaultRingFrames)
{
}

uint32_t AudioRing::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_write - m_read);
}

uint32_t AudioRing::free_frames() const { return m_capacity - size(); }

uint64_t AudioRing::dropped() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

uint32_t AudioRing::push(const float *stereo, uint32_t frames)
{
    if (stereo == nullptr || frames == 0)
        return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t capacity = m_capacity;

    if (frames >= capacity)
    {
        // More than the ring can hold: keep only the newest capacity frames.
        const uint32_t skipped = frames - static_cast<uint32_t>(capacity);
        stereo += size_t(skipped) * kAudioChannels;
        frames = static_cast<uint32_t>(capacity);
        m_dropped += (m_write - m_read) + skipped;
        m_read = m_write;
    }
    else
    {
        const uint64_t queued = m_write - m_read;
        if (queued + frames > capacity)
        {
            const uint64_t drop = queued + frames - capacity;
            m_read += drop;
            m_dropped += drop;
        }
    }

    for (uint32_t i = 0; i < frames; ++i)
    {
        const size_t index = size_t(m_write % capacity) * kAudioChannels;
        m_data[index] = stereo[size_t(i) * kAudioChannels];
        m_data[index + 1] = stereo[size_t(i) * kAudioChannels + 1];
        ++m_write;
    }
    return frames;
}

uint32_t AudioRing::pop(float *stereo, uint32_t frames)
{
    if (stereo == nullptr || frames == 0)
        return 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t queued = m_write - m_read;
    const uint32_t count = frames < queued ? frames : static_cast<uint32_t>(queued);
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t index = size_t(m_read % m_capacity) * kAudioChannels;
        stereo[size_t(i) * kAudioChannels] = m_data[index];
        stereo[size_t(i) * kAudioChannels + 1] = m_data[index + 1];
        ++m_read;
    }
    return count;
}

void AudioRing::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_read = m_write;
}

// ---------------------------------------------------------------------------------------------------
// WavWriter

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::filesystem::path &path, std::string &err)
{
    if (m_file != nullptr)
    {
        err = "a WAV is already open";
        return false;
    }
    // Missing parent folders are created (what --shots does for its folder); a failure here surfaces on
    // the fopen below with the path in the message.
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::FILE *file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr)
    {
        err = "cannot create the WAV " + path.string();
        return false;
    }
    m_file = file;
    m_path = path;
    m_frames = 0;
    m_failed = false;
    m_size_warned = false;

    // A complete header with zero sizes; write() and close() keep the sizes right.
    std::fwrite("RIFF", 1, 4, file);
    put_u32le(file, kWavHeaderBytes - 8);
    std::fwrite("WAVEfmt ", 1, 8, file);
    put_u32le(file, 16);
    put_u16le(file, 1); // PCM
    put_u16le(file, kAudioChannels);
    put_u32le(file, kAudioSampleRate);
    put_u32le(file, kAudioSampleRate * kAudioChannels * 2); // byte rate
    put_u16le(file, kAudioChannels * 2);                    // block align
    put_u16le(file, 16);                                    // bits per sample
    std::fwrite("data", 1, 4, file);
    put_u32le(file, 0);
    if (std::ferror(file) || std::fflush(file) != 0)
    {
        std::fclose(file);
        m_file = nullptr;
        err = "cannot write the WAV header of " + path.string();
        return false;
    }
    return true;
}

bool WavWriter::write_header_sizes()
{
    const uint64_t data_bytes = m_frames * kAudioChannels * 2;
    const long end = std::ftell(m_file);
    if (end < 0 || std::fseek(m_file, 4, SEEK_SET) != 0)
        return false;
    put_u32le(m_file, static_cast<uint32_t>(data_bytes + kWavHeaderBytes - 8));
    if (std::fseek(m_file, 40, SEEK_SET) != 0)
        return false;
    put_u32le(m_file, static_cast<uint32_t>(data_bytes));
    if (std::fseek(m_file, end, SEEK_SET) != 0)
        return false;
    return true;
}

void WavWriter::write(const float *stereo, uint32_t frames)
{
    if (m_file == nullptr || stereo == nullptr || frames == 0)
        return;
    const uint64_t data_bytes = m_frames * kAudioChannels * 2;
    if (m_failed)
        return;
    if (data_bytes >= kWavMaxDataBytes)
    {
        if (!m_size_warned)
        {
            m_size_warned = true;
            std::fprintf(stderr, "[don-spu2] the WAV %s reached its 2 GiB size limit; the rest is not written\n",
                         m_path.string().c_str());
        }
        return;
    }

    // Convert to a scratch buffer and write it, then flush the samples before the header is touched:
    // the sizes on disk never describe data that is not there yet.
    int16_t samples[512 * kAudioChannels];
    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t chunk = std::min<uint32_t>(frames - done, 512);
        for (uint32_t i = 0; i < chunk * kAudioChannels; ++i)
            samples[i] = float_to_i16(stereo[size_t(done) * kAudioChannels + i]);
        if (std::fwrite(samples, sizeof(int16_t), chunk * kAudioChannels, m_file) != chunk * kAudioChannels)
        {
            m_failed = true;
            break;
        }
        done += chunk;
    }
    m_frames += done;
    if (std::fflush(m_file) != 0 || !write_header_sizes())
        m_failed = true;
    if (m_failed)
    {
        std::fprintf(stderr, "[don-spu2] the WAV %s stopped at %.3f s (write error)\n", m_path.string().c_str(),
                     double(m_frames) / double(kAudioSampleRate));
    }
    else
    {
        std::fflush(m_file);
    }
}

bool WavWriter::close()
{
    if (m_file == nullptr)
        return true;
    const bool ok = !m_failed && write_header_sizes() && std::fflush(m_file) == 0;
    if (std::fclose(m_file) != 0)
        return false;
    m_file = nullptr;
    return ok;
}

// ---------------------------------------------------------------------------------------------------
// HostAudio

#if defined(DON_AUDIO_SDL)
struct HostAudio::Device
{
    SDL_AudioStream *stream = nullptr;
};
#else
struct HostAudio::Device
{
};
#endif

HostAudio::HostAudio() = default;

HostAudio::~HostAudio() { shutdown(); }

bool HostAudio::init(const AudioOutputParams &params, std::string &err)
{
    m_params = params;
    if (m_params.ring_frames == 0)
        m_params.ring_frames = kDefaultRingFrames;
    if (m_params.prebuffer_frames == 0)
        m_params.prebuffer_frames = kDefaultPrebufferFrames;
    if (m_params.device_queue_frames == 0)
        m_params.device_queue_frames = kDefaultDeviceQueueFrames;
    m_ring = std::make_unique<AudioRing>(m_params.ring_frames);
    m_frames = 0;
    m_dropped = 0;

    if (!m_params.wav_path.empty() && !m_wav.open(m_params.wav_path, err))
        return false;

    if (m_params.open_device)
    {
        std::string device_error;
        if (!open_device(device_error))
        {
            // Not fatal: the game must still run. The frames continue to the WAV if there is one.
            std::fprintf(stderr, "[don-spu2] cannot open the audio device (%s); %s\n", device_error.c_str(),
                         m_wav.is_open() ? "the sound goes to the WAV only" : "running without sound");
        }
    }
    return true;
}

bool HostAudio::open_device(std::string &err)
{
#if defined(DON_AUDIO_SDL)
    m_audio_owned = !SDL_WasInit(SDL_INIT_AUDIO);
    if (m_audio_owned && !SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        err = SDL_GetError();
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, kAudioChannels, int(kAudioSampleRate)};
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
    {
        err = SDL_GetError();
        if (m_audio_owned)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    // The device stays paused (SDL opens a stream's device paused) until the first cushion is queued: feed_device()
    // resumes it. See AudioPrebuffer.
    m_device = std::make_unique<Device>();
    m_device->stream = stream;
    {
        SDL_AudioSpec device_spec{};
        int device_frames = 0;
        const SDL_AudioDeviceID device_id = SDL_GetAudioStreamDevice(stream);
        const bool have_device = SDL_GetAudioDeviceFormat(device_id, &device_spec, &device_frames);
        SDL_AudioSpec stream_src{}, stream_dst{};
        const bool have_stream = SDL_GetAudioStreamFormat(stream, &stream_src, &stream_dst);
        std::fprintf(stderr, "[don] audio device: ");
        if (have_device)
            std::fprintf(stderr, "format=0x%04x channels=%d rate=%d sample_frames=%d", unsigned(device_spec.format),
                         device_spec.channels, device_spec.freq, device_frames);
        else
            std::fprintf(stderr, "format unknown (%s)", SDL_GetError());
        if (have_stream)
            std::fprintf(stderr, "; stream in=0x%04x/%dch/%d Hz out=0x%04x/%dch/%d Hz", unsigned(stream_src.format),
                         stream_src.channels, stream_src.freq, unsigned(stream_dst.format), stream_dst.channels,
                         stream_dst.freq);
        std::fprintf(stderr, "; prebuffer %u ms\n", m_params.prebuffer_frames * 1000u / kAudioSampleRate);
    }
    return true;
#else
    err = "this build has no SDL3 support (compiled without DON_AUDIO_SDL)";
    return false;
#endif
}

void HostAudio::feed_device()
{
#if defined(DON_AUDIO_SDL)
    if (m_device == nullptr || m_device->stream == nullptr || m_ring == nullptr)
        return;
    const int queued_bytes = SDL_GetAudioStreamQueued(m_device->stream);
    if (queued_bytes < 0)
        return;
    const uint32_t bytes_per_frame = kAudioChannels * sizeof(float);
    uint32_t queued = static_cast<uint32_t>(queued_bytes) / bytes_per_frame;
    const uint32_t target = m_params.device_queue_frames;

    // The stream ran dry while playing: a real underrun. Pause until the cushion is rebuilt.
    if (m_prebuffer.check_underrun(queued))
        SDL_PauseAudioStreamDevice(m_device->stream);

    float scratch[512 * kAudioChannels];
    while (queued < target)
    {
        const uint32_t want = std::min<uint32_t>(512, target - queued);
        const uint32_t got = m_ring->pop(scratch, want);
        if (got == 0)
            break;
        if (!SDL_PutAudioStreamData(m_device->stream, scratch, int(got * bytes_per_frame)))
        {
            if (!m_device_warned)
            {
                std::fprintf(stderr, "[don-spu2] cannot feed the audio stream (%s)\n", SDL_GetError());
                m_device_warned = true;
            }
            break;
        }
        queued += got;
    }
    if (m_prebuffer.check_ready(queued + m_ring->size(), m_params.prebuffer_frames) &&
        !SDL_ResumeAudioStreamDevice(m_device->stream) && !m_device_warned)
    {
        std::fprintf(stderr, "[don-spu2] cannot start the audio device (%s)\n", SDL_GetError());
        m_device_warned = true;
    }
#endif
}

void HostAudio::set_rate(double ratio)
{
#if defined(DON_AUDIO_SDL)
    if (m_device != nullptr && m_device->stream != nullptr && SDL_SetAudioStreamFrequencyRatio(m_device->stream, static_cast<float>(ratio)))
        m_rate = ratio;
#else
    (void)ratio;
#endif
}

uint32_t HostAudio::drop_oldest(uint32_t frames)
{
    if (!m_ring || frames == 0)
        return 0;
    std::vector<float> scratch(512 * kAudioChannels);
    uint32_t dropped = 0;
    while (dropped < frames)
    {
        const uint32_t got = m_ring->pop(scratch.data(), std::min<uint32_t>(512, frames - dropped));
        if (got == 0)
            break;
        dropped += got;
    }
    m_dropped += dropped;
    return dropped;
}

uint32_t HostAudio::stream_queued() const
{
#if defined(DON_AUDIO_SDL)
    if (m_device == nullptr || m_device->stream == nullptr)
        return 0;
    const int bytes = SDL_GetAudioStreamQueued(m_device->stream);
    return bytes < 0 ? 0 : static_cast<uint32_t>(bytes) / (kAudioChannels * sizeof(float));
#else
    return 0;
#endif
}

void HostAudio::write(const float *stereo, uint32_t frames)
{
    if (stereo == nullptr || frames == 0)
        return;
    m_frames += frames;
    if (m_wav.is_open())
        m_wav.write(stereo, frames);
    if (m_device != nullptr && m_ring != nullptr)
    {
        m_ring->push(stereo, frames);
        m_dropped = m_ring->dropped();
        feed_device();
    }
}

void HostAudio::shutdown()
{
    if (m_device != nullptr)
    {
#if defined(DON_AUDIO_SDL)
        if (m_device->stream != nullptr)
            SDL_DestroyAudioStream(m_device->stream);
        if (m_audio_owned)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_device->stream = nullptr;
#endif
        m_device.reset();
    }
    m_wav.close();
}

void HostAudio::print_summary() const
{
    if (!env_flag("DON_AUDIO_LOG"))
        return;
    std::fprintf(stderr, "[don-spu2] audio: %llu frames (%.2f s), device %s, %llu frame(s) dropped by the queue\n",
                 static_cast<unsigned long long>(m_frames), seconds_written(),
                 m_device != nullptr ? "open" : "closed", static_cast<unsigned long long>(m_dropped));
}
