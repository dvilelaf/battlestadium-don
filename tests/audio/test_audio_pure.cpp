// T-303: the pure part of src/host/audio.{h,cpp}: the ring, the float to 16-bit conversion and the WAV
// writer. Needs nothing (no game, no IOP, no SDL); the WAV is written and parsed back here.
#include "audio.h"
#include "check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kFrames = kAudioSampleRate; // one second

// The test writes its WAV files into the system's temporary folder, so nothing is left in the repository.
std::string temp_path(const char *name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<float> pattern(uint32_t frames)
{
    std::vector<float> lr(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i)
    {
        // +0.5, then two values that clamp, then a ramp: exercises ordering and clamping.
        float l = 0.0f;
        switch (i % 4)
        {
        case 0: l = 0.5f; break;
        case 1: l = 2.0f; break;
        case 2: l = -2.0f; break;
        default: l = -1.0f; break;
        }
        lr[size_t(i) * 2] = l;
        lr[size_t(i) * 2 + 1] = -l;
    }
    return lr;
}

struct Wav
{
    bool ok = false;
    uint32_t riff_size = 0;
    uint32_t fmt_size = 0;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits = 0;
    uint32_t data_bytes = 0;
    uint64_t file_bytes = 0;
    std::vector<int16_t> samples;
};

uint32_t read_u32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint16_t read_u16(const uint8_t *p) { return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }

Wav parse_wav(const std::string &path)
{
    Wav wav;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return wav;
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
        bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(f);
    wav.file_bytes = bytes.size();
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0 ||
        std::memcmp(bytes.data() + 12, "fmt ", 4) != 0 || std::memcmp(bytes.data() + 36, "data", 4) != 0)
        return wav;
    wav.riff_size = read_u32(bytes.data() + 4);
    wav.fmt_size = read_u32(bytes.data() + 16);
    wav.format = read_u16(bytes.data() + 20);
    wav.channels = read_u16(bytes.data() + 22);
    wav.rate = read_u32(bytes.data() + 24);
    wav.byte_rate = read_u32(bytes.data() + 28);
    wav.block_align = read_u16(bytes.data() + 32);
    wav.bits = read_u16(bytes.data() + 34);
    wav.data_bytes = read_u32(bytes.data() + 40);
    if (wav.data_bytes > bytes.size() - 44)
        return wav;
    for (uint32_t i = 0; i + 1 < wav.data_bytes; i += 2)
        wav.samples.push_back(static_cast<int16_t>(read_u16(bytes.data() + 44 + i)));
    wav.ok = true;
    return wav;
}

void test_conversion()
{
    std::printf("-- float to 16-bit\n");
    CHECK_EQ(float_to_i16(-1.0f), -32767);
    CHECK_EQ(float_to_i16(0.0f), 0);
    CHECK_EQ(float_to_i16(1.0f), 32767);
    CHECK_EQ(float_to_i16(-2.0f), -32767); // clamped low
    CHECK_EQ(float_to_i16(2.0f), 32767);   // clamped high
    CHECK_EQ(float_to_i16(-0.5f), -16384); // sign kept
    CHECK_EQ(float_to_i16(0.5f), 16384);
    CHECK_EQ(float_to_i16(std::nanf("")), 0); // NaN (a broken voice) gives 0, not a crash
    CHECK_EQ(float_to_i16(-1e-8f), 0);
    CHECK(float_to_i16(0.999f) > 32000);
}

void test_ring()
{
    std::printf("-- ring\n");
    AudioRing ring(4);
    CHECK_EQ(ring.capacity(), 4);
    CHECK_EQ(ring.size(), 0);
    CHECK_EQ(ring.free_frames(), 4);

    // Order is kept across the wrap-around of the storage.
    const float a[6] = {1, 10, 2, 20, 3, 30};
    CHECK_EQ(ring.push(a, 3), 3);
    CHECK_EQ(ring.size(), 3);
    float out[8] = {};
    CHECK_EQ(ring.pop(out, 2), 2);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 10);
    CHECK_EQ(out[2], 2);
    CHECK_EQ(out[3], 20);
    CHECK_EQ(ring.size(), 1);
    const float b[4] = {4, 40, 5, 50}; // the storage index wraps at frame 4
    CHECK_EQ(ring.push(b, 2), 2);
    CHECK_EQ(ring.size(), 3);
    CHECK_EQ(ring.pop(out, 8), 3); // asking for more than there is gives what there is
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[1], 30);
    CHECK_EQ(out[2], 4);
    CHECK_EQ(out[3], 40);
    CHECK_EQ(out[4], 5);
    CHECK_EQ(out[5], 50);
    CHECK_EQ(ring.size(), 0);
    CHECK_EQ(ring.pop(out, 4), 0);

    // Overflow: the oldest frames are dropped, the newest are kept and counted.
    ring.clear();
    std::vector<float> many(size_t(10) * 2);
    for (int i = 0; i < 10; ++i)
    {
        many[size_t(i) * 2] = float(i);
        many[size_t(i) * 2 + 1] = float(-i);
    }
    CHECK_EQ(ring.push(many.data(), 10), 4);
    CHECK_EQ(ring.dropped(), 6);
    CHECK_EQ(ring.size(), 4);
    CHECK_EQ(ring.pop(out, 4), 4);
    CHECK_EQ(out[0], 6);
    CHECK_EQ(out[1], -6);
    CHECK_EQ(out[6], 9);
    CHECK_EQ(out[7], -9);

    // Partial overflow: a full ring with an old frame drops just as many as are needed.
    ring.clear();
    const uint64_t dropped_before = ring.dropped();
    const float c[6] = {4, 40, 5, 50, 6, 60};
    CHECK_EQ(ring.push(a, 3), 3);
    CHECK_EQ(ring.push(c, 3), 3); // capacity 4: the two oldest of the six go
    CHECK_EQ(ring.dropped() - dropped_before, 2);
    CHECK_EQ(ring.pop(out, 4), 4);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[1], 30);
    CHECK_EQ(out[2], 4);
    CHECK_EQ(out[4], 5);
    CHECK_EQ(out[6], 6);
    CHECK_EQ(out[7], 60);

    // Nothing to take: short pop, null pointers, zero frames.
    ring.clear();
    CHECK_EQ(ring.pop(nullptr, 4), 0);
    CHECK_EQ(ring.push(nullptr, 4), 0);
    CHECK_EQ(ring.push(a, 0), 0);
}

void test_wav_writer()
{
    std::printf("-- WAV writer\n");
    const std::string path = temp_path("don_audio_pure_test.wav");
    const std::vector<float> lr = pattern(kFrames);

    WavWriter writer;
    std::string err;
    CHECK(writer.open(path, err));
    CHECK(writer.is_open());
    // Write in three chunks: the seams must not matter.
    writer.write(lr.data(), kFrames / 3);
    writer.write(lr.data() + size_t(kFrames / 3) * 2, kFrames / 3);
    writer.write(lr.data() + size_t(2 * (kFrames / 3)) * 2, kFrames - 2 * (kFrames / 3));
    CHECK_EQ(writer.frames_written(), kFrames);
    CHECK(writer.close());
    CHECK(writer.close()); // closing twice is safe

    const Wav wav = parse_wav(path);
    CHECK(wav.ok);
    CHECK_EQ(wav.format, 1);
    CHECK_EQ(wav.channels, 2);
    CHECK_EQ(wav.rate, kAudioSampleRate);
    CHECK_EQ(wav.bits, 16);
    CHECK_EQ(wav.fmt_size, 16);
    CHECK_EQ(wav.block_align, 4);
    CHECK_EQ(wav.byte_rate, kAudioSampleRate * 4);
    CHECK_EQ(wav.data_bytes, kFrames * 4);
    CHECK_EQ(wav.riff_size, wav.data_bytes + 36);
    CHECK_EQ(wav.file_bytes, uint64_t(wav.data_bytes) + 44);
    CHECK_EQ(wav.samples.size(), size_t(kFrames) * 2);
    int mismatches = 0;
    for (size_t i = 0; i < wav.samples.size() && i < lr.size(); ++i)
        if (wav.samples[i] != float_to_i16(lr[i]))
            ++mismatches;
    CHECK_EQ(mismatches, 0);

    // An abrupt end: the file is a complete WAV holding what was written so far, without close().
    const std::string abrupt = temp_path("don_audio_pure_abrupt.wav");
    {
        WavWriter partial;
        CHECK(partial.open(abrupt, err));
        partial.write(lr.data(), 5);
        const Wav after5 = parse_wav(abrupt);
        CHECK(after5.ok);
        CHECK_EQ(after5.data_bytes, 5 * 4);
        CHECK_EQ(after5.samples.size(), 10);
        CHECK_EQ(after5.samples[0], float_to_i16(lr[0]));
        partial.write(lr.data() + 10, 7);
        const Wav after12 = parse_wav(abrupt);
        CHECK(after12.ok);
        CHECK_EQ(after12.data_bytes, 12 * 4);
        // The destructor closes it; the file stays complete.
    }
    const Wav final_wav = parse_wav(abrupt);
    CHECK(final_wav.ok);
    CHECK_EQ(final_wav.data_bytes, 12 * 4);

    // A WAV with no frames is still a valid WAV.
    const std::string empty = temp_path("don_audio_pure_empty.wav");
    {
        WavWriter none;
        CHECK(none.open(empty, err));
    }
    const Wav empty_wav = parse_wav(empty);
    CHECK(empty_wav.ok);
    CHECK_EQ(empty_wav.data_bytes, 0);
    CHECK_EQ(empty_wav.file_bytes, 44);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(abrupt, ec);
    std::filesystem::remove(empty, ec);
}

void test_host_audio_without_device()
{
    std::printf("-- HostAudio without a device (headless)\n");
    const std::string path = temp_path("don_audio_pure_headless.wav");
    HostAudio audio;
    AudioOutputParams params;
    params.open_device = false;
    params.wav_path = path;
    std::string err;
    CHECK(audio.init(params, err));
    CHECK(!audio.device_open());
    const std::vector<float> lr = pattern(1000);
    audio.write(lr.data(), 1000);
    CHECK_EQ(audio.frames_written(), 1000);
    CHECK_EQ(audio.frames_dropped(), 0);
    audio.shutdown();
    CHECK(audio.device_open() == false);
    const Wav wav = parse_wav(path);
    CHECK(wav.ok);
    CHECK_EQ(wav.data_bytes, 1000 * 4);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
} // namespace

int main()
{
    test_conversion();
    test_ring();
    test_wav_writer();
    test_host_audio_without_device();
    return check_result("test_audio_pure");
}
