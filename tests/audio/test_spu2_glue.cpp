// T-303: the SPU2 library through src/host/spu2_glue.{h,cpp}, with the REAL library
// (build/spu2/libdon_spu2.so), NO game and NO IOP emulator. This program plays the IOP emulator's part:
// it calls the callbacks of the IopHostSpu2 table the way ps2x/iop/iop_host_spu2.h says the emulator does
// (register writes, a DMA upload, key-on, advance in slices), pulls the produced audio through the glue,
// writes it with the WAV writer and checks it: not silence, no clipping, and the pitch of the sample.
//
// The synthetic sample and the register sequence are the ones of src/spu2bridge/spu2_selftest.cpp: a
// 56-sample looping SPU2-ADPCM square wave on voice 0 of core 1, uploaded through core 0 with a DMA.
//
//   test_spu2_glue [--spu2-dir <folder with libdon_spu2>] [--wav <file>]
#include "audio.h"
#include "check.h"
#include "spu2_glue.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// The SPU2 register map (offsets from DON_SPU2_REG_BASE; core 1 is 0x400 higher). Same values as
// src/spu2bridge/spu2_selftest.cpp.
constexpr uint32_t kVoiceStride = 0x10;
constexpr uint32_t kVoiceAddrBase = 0x1C0;
constexpr uint32_t kVoiceAddrStride = 0x0C;
constexpr uint32_t VOLL = 0x0, VOLR = 0x2, PITCH = 0x4, ADSR1 = 0x6, ADSR2 = 0x8;
constexpr uint32_t ATTR = 0x19A, KON = 0x1A0, TSA = 0x1A8;
constexpr uint32_t MVOLL = 0x760, MVOLR = 0x762;
constexpr uint32_t kAttrEnable = 0x8000, kAttrDmaWrite = 0x0020;

uint32_t core_reg(int core, uint32_t off) { return DON_SPU2_REG_BASE + (core ? 0x400u : 0u) + off; }
uint32_t voice_reg(int core, int voice, uint32_t off) { return core_reg(core, uint32_t(voice) * kVoiceStride + off); }
uint32_t voice_addr_reg(int core, int voice, uint32_t off)
{
    return core_reg(core, kVoiceAddrBase + uint32_t(voice) * kVoiceAddrStride + off);
}
uint32_t master_reg(int core, uint32_t off) { return DON_SPU2_REG_BASE + off + (core ? 0x28u : 0u); }

// A 32-bit register write as the IOP emulator delivers it: two 16-bit writes, the low halfword first
// (ps2x/iop/iop_host_spu2.h, "Registers").
void write32(const ps2x::iop::IopHostSpu2 &h, uint32_t reg, uint32_t value)
{
    h.write16(h.user, reg, static_cast<uint16_t>(value & 0xFFFFu));
    h.write16(h.user, reg + 2, static_cast<uint16_t>(value >> 16));
}

// A looping SPU2-ADPCM square wave: 16-byte blocks of 28 samples; block header byte 0 = shift | filter
// << 4, byte 1 = flags (bit 0 loop end, bit 1 loop, bit 2 loop start). With shift 0 and filter 0 a nibble
// n decodes to n << 12. One block of +7 and one of -8 make a wave of 56 samples (857 Hz at pitch 0x1000).
constexpr int kSquarePeriod = 56;
std::vector<uint8_t> make_square_sample(size_t total_bytes)
{
    std::vector<uint8_t> bytes(total_bytes, 0);
    for (int block = 0; block < 2; ++block)
    {
        uint8_t *b = bytes.data() + block * 16;
        b[0] = 0x00;
        b[1] = block == 0 ? 0x06 /* loop + loop start */ : 0x03 /* loop + loop end */;
        const uint8_t nibble = block == 0 ? 0x7 : 0x8;
        for (int i = 2; i < 16; ++i)
            b[i] = static_cast<uint8_t>(nibble | (nibble << 4));
    }
    return bytes;
}

// Plays voice 0 of `core` from `ssa` (master volume 0x3FFF), the sequence of spu2_selftest.cpp.
void play_voice(const ps2x::iop::IopHostSpu2 &h, int core, uint32_t ssa)
{
    h.write16(h.user, master_reg(core, MVOLL), 0x3FFF);
    h.write16(h.user, master_reg(core, MVOLR), 0x3FFF);
    h.write16(h.user, voice_reg(core, 0, VOLL), 0x3FFF);
    h.write16(h.user, voice_reg(core, 0, VOLR), 0x3FFF);
    h.write16(h.user, voice_reg(core, 0, PITCH), 0x1000); // 48 kHz
    h.write16(h.user, voice_reg(core, 0, ADSR1), 0x000F);  // fastest attack, full sustain
    h.write16(h.user, voice_reg(core, 0, ADSR2), 0x1FC0);
    write32(h, voice_addr_reg(core, 0, 0), ssa);
    h.write16(h.user, core_reg(core, KON), 0x0001);
    h.write16(h.user, core_reg(core, KON + 2), 0x0000);
}

struct Event
{
    int type;
    uint64_t cycle;
};

// Moves time forward in 1 ms steps, so that the events of every step are drained in order, and takes the
// audio the glue pulled out of the SPU2. `advanced` counts the IOP cycles advanced.
void advance_collect(const ps2x::iop::IopHostSpu2 &h, HostSpu2 &spu2, uint64_t cycles, uint64_t &advanced,
                     std::vector<Event> &events, std::vector<float> &audio)
{
    constexpr uint64_t kStep = DON_SPU2_IOP_HZ / 1000; // 1 ms
    while (cycles != 0)
    {
        const uint64_t n = cycles < kStep ? cycles : kStep;
        h.advance(h.user, n);
        advanced += n;
        cycles -= n;
        Event e;
        while (h.poll_event(h.user, &e.type, &e.cycle))
            events.push_back(e);
        float buffer[2 * 512];
        uint32_t got = 0;
        while ((got = spu2.pull(buffer, 512)) > 0)
            audio.insert(audio.end(), buffer, buffer + 2 * got);
    }
}

struct Stats
{
    float peak = 0;
    double rms = 0;
};

Stats measure(const std::vector<float> &lr, size_t first_frame, size_t last_frame)
{
    Stats stats;
    double sum = 0;
    size_t n = 0;
    for (size_t i = first_frame; i < last_frame && 2 * i + 1 < lr.size(); ++i)
        for (int c = 0; c < 2; ++c)
        {
            const float v = lr[2 * i + c];
            stats.peak = std::fmax(stats.peak, std::fabs(v));
            sum += double(v) * v;
            ++n;
        }
    stats.rms = n ? std::sqrt(sum / double(n)) : 0.0;
    return stats;
}

// Sign changes of the left channel, ignoring noise below the hysteresis: two per wave period.
size_t count_crossings(const std::vector<float> &lr, size_t first_frame, size_t last_frame)
{
    size_t n = 0;
    int state = 0;
    for (size_t i = first_frame; i < last_frame && 2 * i < lr.size(); ++i)
    {
        const float v = lr[2 * i];
        const int s = v > 0.02f ? 1 : (v < -0.02f ? -1 : 0);
        if (s != 0 && s != state)
        {
            if (state != 0)
                ++n;
            state = s;
        }
    }
    return n;
}

constexpr uint32_t kSsa = 0x5000; // in 16-bit units of sound memory
} // namespace

int main(int argc, char **argv)
{
    std::string spu2_dir = std::string(DON_REPO_ROOT) + "/build/spu2";
    std::string wav_path;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--spu2-dir") == 0 && i + 1 < argc)
            spu2_dir = argv[++i];
        else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
            wav_path = argv[++i];
        else if (argv[i][0] != '-')
            spu2_dir = argv[i];
        else
        {
            std::fprintf(stderr, "usage: %s [--spu2-dir <folder>] [--wav <file>]\n", argv[0]);
            return 2;
        }
    }

    // The two interfaces were designed to match: check it here, where both are visible.
    CHECK_EQ(ps2x::iop::kHostSpu2EventIrq, DON_SPU2_EVENT_IRQ);
    CHECK_EQ(ps2x::iop::kHostSpu2EventDmaEnd0, DON_SPU2_EVENT_DMA_END_0);
    CHECK_EQ(ps2x::iop::kHostSpu2EventDmaEnd1, DON_SPU2_EVENT_DMA_END_1);

    // A folder with no library: a clear message and a failure, not a crash.
    {
        DonSpu2Api missing;
        std::string missing_error;
        CHECK(!don_spu2_load("no-such-spu2-folder", missing, missing_error));
        CHECK(missing_error.rfind("[don-spu2]", 0) == 0);
        CHECK(missing_error.find("libdon_spu2") != std::string::npos);
    }

    DonSpu2Api api;
    std::string error;
    if (!don_spu2_load(spu2_dir, api, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("-- library: %s\n", api.path.string().c_str());

    HostSpu2 spu2(api);
    std::string open_error;
    if (!spu2.open(open_error))
    {
        std::fprintf(stderr, "%s\n", open_error.c_str());
        return 1;
    }
    CHECK(spu2.is_open());
    const ps2x::iop::IopHostSpu2 &h = spu2.iface();
    CHECK(h.user == &spu2);
    CHECK(h.write16 != nullptr && h.read16 != nullptr && h.dma_write != nullptr && h.dma_read != nullptr &&
          h.advance != nullptr && h.poll_event != nullptr && h.reset != nullptr);
    CHECK_EQ(spu2.pull(nullptr, 10), 0); // a null buffer asks for nothing

    std::vector<Event> events;
    std::vector<float> audio;
    uint64_t advanced = 0;

    // 1. Upload the sample with core 0's DMA (channel 4), the sequence a sound driver uses. The emulator
    //    normally reads the bytes from IOP RAM after the guest wrote CHCR; the glue's callback is the same.
    h.write16(h.user, core_reg(0, ATTR), kAttrEnable | kAttrDmaWrite);
    CHECK_EQ(h.read16(h.user, core_reg(0, ATTR)), kAttrEnable | kAttrDmaWrite);
    write32(h, core_reg(0, TSA), kSsa);
    const std::vector<uint8_t> sample = make_square_sample(1024);
    h.dma_write(h.user, 0, sample.data(), static_cast<uint32_t>(sample.size()));
    advance_collect(h, spu2, 20000, advanced, events, audio); // the transfer ends a few thousand cycles later
    int dma_end_events = 0;
    for (const Event &e : events)
        if (e.type == DON_SPU2_EVENT_DMA_END_0)
            ++dma_end_events;
    CHECK(dma_end_events >= 1);
    const size_t quiet_frames = audio.size() / 2;

    // 2. Key on voice 0 of core 1 and play one second from the start of the run.
    h.write16(h.user, core_reg(1, ATTR), kAttrEnable);
    play_voice(h, 1, kSsa);
    const uint64_t key_cycle = advanced;
    advance_collect(h, spu2, DON_SPU2_IOP_HZ - advanced, advanced, events, audio);

    const size_t frames = audio.size() / 2;
    CHECK_EQ(advanced, DON_SPU2_IOP_HZ);
    CHECK(frames + 1 >= DON_SPU2_SAMPLE_RATE && frames <= DON_SPU2_SAMPLE_RATE + 1);

    const Stats quiet = measure(audio, 0, quiet_frames);
    CHECK(quiet.peak < 1e-3f);
    const size_t settle = size_t(key_cycle / DON_SPU2_CYCLES_PER_FRAME) + 2000;
    const Stats stats = measure(audio, settle, frames);
    CHECK(stats.peak > 0.05f);
    CHECK(stats.peak <= 1.0f);
    CHECK(stats.rms > 0.02 && stats.rms < 0.9);
    const size_t crossings = count_crossings(audio, settle, frames);
    const double hz = double(crossings) / 2.0 * double(DON_SPU2_SAMPLE_RATE) / double(frames - settle);
    const double want = double(DON_SPU2_SAMPLE_RATE) / kSquarePeriod;
    CHECK(std::fabs(hz - want) / want < 0.03);
    std::printf("-- %zu frames, peak %.4f, RMS %.4f, %.1f Hz measured (%.1f expected)\n", frames, stats.peak,
                stats.rms, hz, want);
    float extra[8];
    CHECK_EQ(spu2.pull(extra, 4), 0); // everything was already taken

    // 3. The WAV writer of the host program, fed with what the SPU2 produced.
    if (!wav_path.empty())
    {
        WavWriter writer;
        std::string wav_error;
        CHECK(writer.open(wav_path, wav_error));
        writer.write(audio.data(), static_cast<uint32_t>(frames));
        CHECK(writer.close());
        CHECK_EQ(writer.frames_written(), frames);
        std::printf("-- wav: %s (%zu frames)\n", wav_path.c_str(), frames);
    }

    spu2.close();
    CHECK(!spu2.is_open());
    CHECK_EQ(spu2.pull(extra, 4), 0); // after close the glue stops pulling
    return check_result("test_spu2_glue");
}
