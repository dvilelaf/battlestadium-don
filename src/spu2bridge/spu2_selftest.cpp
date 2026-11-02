// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self-test of libdon_spu2 through its C API only (don_spu2.h). It needs no game data: it builds a
// synthetic SPU2-ADPCM sample, uploads it by DMA with the register sequence a sound driver uses,
// plays it on one voice, and checks what comes out. The structure of the test (a looping ADPCM
// tone, key-on, advance, analyse) follows the self-test of the Ridge Racer V recompilation
// project, tools/pcsx2-spu2/test/spu2_selftest.cpp, commit e8818121566cfaf3d361e38745860b0a9438e27b
// (GPL-3.0-or-later); the code is written for this project.
//
//   spu2_selftest [--wav <file>]
//
// Prints one line per check, "OK" or "FAIL", and exits with 0 only if every check passed. With
// --wav the audio of the first case is also written to a 16-bit stereo WAV file.
#include "don_spu2.h"

#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* name, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void check(bool ok, const char* name, const char* fmt, ...)
{
	char detail[256];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	std::printf("%s %s: %s\n", ok ? "OK  " : "FAIL", name, detail);
	g_checks++;
	if (!ok)
		g_failures++;
}

// Register offsets from the SPU2 register map (the IOP address is DON_SPU2_REG_BASE + offset;
// core 1 is 0x400 higher).
constexpr uint32_t kVoiceStride = 0x10;
constexpr uint32_t kVoiceAddrBase = 0x1C0;
constexpr uint32_t kVoiceAddrStride = 0x0C;
constexpr uint32_t VOLL = 0x0, VOLR = 0x2, PITCH = 0x4, ADSR1 = 0x6, ADSR2 = 0x8;
constexpr uint32_t ATTR = 0x19A, IRQA = 0x19C, KON = 0x1A0, TSA = 0x1A8;
constexpr uint32_t MVOLL = 0x760, MVOLR = 0x762;
constexpr uint32_t kAttrEnable = 0x8000, kAttrIrq = 0x0040, kAttrDmaWrite = 0x0020;

uint32_t core_reg(int core, uint32_t off) { return DON_SPU2_REG_BASE + (core ? 0x400u : 0u) + off; }
uint32_t voice_reg(int core, int v, uint32_t off) { return core_reg(core, v * kVoiceStride + off); }
uint32_t voice_addr_reg(int core, int v, uint32_t off) { return core_reg(core, kVoiceAddrBase + v * kVoiceAddrStride + off); }
// The "different" register area (master volume): core 1 is 0x28 higher, not 0x400.
uint32_t master_reg(int core, uint32_t off) { return DON_SPU2_REG_BASE + off + (core ? 0x28u : 0u); }

void write32(uint32_t reg, uint32_t value)
{
	don_spu2_write16(reg, static_cast<uint16_t>(value >> 16));
	don_spu2_write16(reg + 2, static_cast<uint16_t>(value & 0xFFFF));
}

// A looping SPU2-ADPCM square wave: 16-byte blocks of 28 samples. Block header: byte 0 = shift
// (low nibble) | filter << 4, byte 1 = flags (bit 0 loop end, bit 1 loop, bit 2 loop start).
// With shift 0 and filter 0 a 4-bit nibble n decodes to n << 12 (signed). One block of +7 and one of -8
// make a wave of 56 samples, 857 Hz at pitch 0x1000 (48 kHz).
constexpr int kSquarePeriod = 56;
std::vector<uint8_t> make_square_sample(size_t total_bytes)
{
	std::vector<uint8_t> bytes(total_bytes, 0);
	for (int block = 0; block < 2; block++)
	{
		uint8_t* b = bytes.data() + block * 16;
		b[0] = 0x00;
		b[1] = block == 0 ? 0x06 /* loop + loop start */ : 0x03 /* loop + loop end */;
		const uint8_t nibble = block == 0 ? 0x7 : 0x8;
		for (int i = 2; i < 16; i++)
			b[i] = static_cast<uint8_t>(nibble | (nibble << 4));
	}
	return bytes;
}

// Moves time forward in small steps, so that the events of every step are drained in order.
struct Event
{
	int type;
	uint64_t cycle;
};

void advance_collect(uint64_t cycles, std::vector<Event>& events, std::vector<float>& audio)
{
	constexpr uint64_t kStep = 36864; // 1 ms
	while (cycles)
	{
		const uint64_t n = cycles < kStep ? cycles : kStep;
		don_spu2_advance(n);
		cycles -= n;
		Event e;
		while (don_spu2_poll_event(&e.type, &e.cycle))
			events.push_back(e);
		float buf[2 * 512];
		uint32_t got;
		while ((got = don_spu2_pull_output(buf, 512)) > 0)
			audio.insert(audio.end(), buf, buf + 2 * got);
	}
}

struct Stats
{
	float peak = 0;
	double rms = 0;
};

Stats measure(const std::vector<float>& lr, size_t first_frame, size_t last_frame)
{
	Stats s;
	double sum = 0;
	size_t n = 0;
	for (size_t i = first_frame; i < last_frame && 2 * i + 1 < lr.size(); i++)
		for (int c = 0; c < 2; c++)
		{
			const float v = lr[2 * i + c];
			s.peak = std::fmax(s.peak, std::fabs(v));
			sum += double(v) * v;
			n++;
		}
	s.rms = n ? std::sqrt(sum / double(n)) : 0;
	return s;
}

// Sign changes of the left channel, ignoring noise below the hysteresis: two per wave period.
size_t count_crossings(const std::vector<float>& lr, size_t first_frame, size_t last_frame)
{
	size_t n = 0;
	int state = 0;
	for (size_t i = first_frame; i < last_frame && 2 * i < lr.size(); i++)
	{
		const float v = lr[2 * i];
		const int s = v > 0.02f ? 1 : (v < -0.02f ? -1 : 0);
		if (s != 0 && s != state)
		{
			if (state != 0)
				n++;
			state = s;
		}
	}
	return n;
}

bool write_wav(const std::string& path, const std::vector<float>& lr)
{
	std::FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const uint32_t frames = static_cast<uint32_t>(lr.size() / 2);
	const uint32_t data_bytes = frames * 4;
	auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) std::fputc(int((v >> (8 * i)) & 0xFF), f); };
	auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; i++) std::fputc(int((v >> (8 * i)) & 0xFF), f); };
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + data_bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(1); // PCM
	u16(2); // channels
	u32(DON_SPU2_SAMPLE_RATE);
	u32(DON_SPU2_SAMPLE_RATE * 4);
	u16(4);  // block align
	u16(16); // bits
	std::fwrite("data", 1, 4, f);
	u32(data_bytes);
	for (float v : lr)
	{
		const float c = std::fmax(-1.0f, std::fmin(1.0f, v));
		u16(static_cast<uint16_t>(static_cast<int16_t>(std::lrintf(c * 32767.0f))));
	}
	return std::fclose(f) == 0;
}

// Program voice 0 of `core` to play the sample at `ssa` and key it on (the master volume of the
// core is set too). Volumes are fixed at 0x3FFF (half of the maximum).
void play_voice(int core, uint32_t ssa)
{
	don_spu2_write16(master_reg(core, MVOLL), 0x3FFF);
	don_spu2_write16(master_reg(core, MVOLR), 0x3FFF);
	don_spu2_write16(voice_reg(core, 0, VOLL), 0x3FFF);
	don_spu2_write16(voice_reg(core, 0, VOLR), 0x3FFF);
	don_spu2_write16(voice_reg(core, 0, PITCH), 0x1000); // 48 kHz
	don_spu2_write16(voice_reg(core, 0, ADSR1), 0x000F);  // fastest attack, full sustain
	don_spu2_write16(voice_reg(core, 0, ADSR2), 0x1FC0);
	write32(voice_addr_reg(core, 0, 0), ssa); // start address (the loop address comes from the loop flags)
	don_spu2_write16(core_reg(core, KON), 0x0001);
	don_spu2_write16(core_reg(core, KON + 2), 0x0000);
}

// Uploads `sample` at `ssa` with the sequence a driver uses: attributes (core enabled, DMA write
// mode), transfer address, DMA.
int32_t upload(int core, uint32_t ssa, const std::vector<uint8_t>& sample)
{
	don_spu2_write16(core_reg(core, ATTR), kAttrEnable | kAttrDmaWrite);
	write32(core_reg(core, TSA), ssa);
	return don_spu2_dma_write(core, sample.data(), static_cast<uint32_t>(sample.size()));
}

constexpr uint32_t kSsa = 0x5000; // in 16-bit units of sound memory
constexpr uint64_t kOneSecond = DON_SPU2_IOP_HZ;

bool case_tone(const std::string& wav_path)
{
	std::printf("-- case 1: tone on core 1, sample uploaded by DMA through core 0\n");
	const int before = g_failures;
	if (don_spu2_open() != 0)
	{
		check(false, "open", "don_spu2_open failed");
		return false;
	}
	check(don_spu2_version() == DON_SPU2_API_VERSION, "version", "%d", don_spu2_version());

	std::vector<Event> events;
	std::vector<float> audio;
	const std::vector<uint8_t> sample = make_square_sample(1024);

	const int32_t accepted = upload(0, kSsa, sample);
	check(accepted == int32_t(sample.size()), "dma_write", "accepted %" PRId32 " of %zu bytes", accepted, sample.size());
	advance_collect(20000, events, audio); // the transfer takes a few thousand cycles
	check(events.size() == 1 && events[0].type == DON_SPU2_EVENT_DMA_END_0 && events[0].cycle > 0 && events[0].cycle <= 20000,
	      "dma_end_event", "%zu event(s)%s", events.size(),
	      events.size() == 1 ? (events[0].type == DON_SPU2_EVENT_DMA_END_0 ? ", end of DMA core 0" : ", wrong type") : "");
	if (!events.empty())
		std::printf("     DMA end at IOP cycle %" PRIu64 " (started at 0)\n", events[0].cycle);

	// Read the sample back by DMA from core 0 as a check of the memory path.
	write32(core_reg(0, TSA), kSsa);
	std::vector<uint8_t> back(sample.size(), 0xAA);
	const int32_t read = don_spu2_dma_read(0, back.data(), static_cast<uint32_t>(back.size()));
	check(read == int32_t(sample.size()) && std::memcmp(back.data(), sample.data(), sample.size()) == 0, "dma_read",
	      "%" PRId32 " bytes, sound memory %s the uploaded sample", read,
	      std::memcmp(back.data(), sample.data(), sample.size()) == 0 ? "equals" : "differs from");
	events.clear();
	advance_collect(20000, events, audio);
	const size_t read_events = events.size();
	check(read_events == 1 && events[0].type == DON_SPU2_EVENT_DMA_END_0, "dma_read_end_event", "%zu event(s)", read_events);

	const uint64_t quiet_frames = audio.size() / 2;
	const uint64_t key_cycle = don_spu2_cycle();
	don_spu2_write16(core_reg(1, ATTR), kAttrEnable);
	play_voice(1, kSsa);
	const uint64_t rest = kOneSecond - key_cycle;
	events.clear();
	advance_collect(rest, events, audio);

	const size_t frames = audio.size() / 2;
	check(don_spu2_cycle() == kOneSecond, "cycles", "%" PRIu64 " advanced", don_spu2_cycle());
	check(frames + 1 >= DON_SPU2_SAMPLE_RATE && frames <= DON_SPU2_SAMPLE_RATE + 1, "frame_count",
	      "%zu frames for one second of cycles (expected %u, +-1)", frames, DON_SPU2_SAMPLE_RATE);
	check(don_spu2_pull_output(nullptr, 10) == 0, "pull_null", "NULL buffer gives 0");
	float extra[8];
	check(don_spu2_pull_output(extra, 4) == 0, "pull_empty", "nothing left to pull after everything was taken");

	const Stats quiet = measure(audio, 0, quiet_frames);
	check(quiet.peak < 1e-3f, "silent_before_key_on", "peak %.6f over the first %" PRIu64 " frames", quiet.peak, quiet_frames);

	const size_t settle = key_cycle / DON_SPU2_CYCLES_PER_FRAME + 2000;
	const Stats s = measure(audio, settle, frames);
	check(s.peak > 0.05f, "not_silence", "peak %.4f (limit: above 0.05)", s.peak);
	check(s.peak <= 1.0f, "no_clipping", "peak %.4f (limit: at most 1.0)", s.peak);
	const Stats whole = measure(audio, 0, frames);
	check(whole.peak <= 1.0f, "no_clipping_whole", "peak %.4f over all %zu frames", whole.peak, frames);
	check(s.rms > 0.02 && s.rms < 0.9, "level", "RMS %.4f (limit: between 0.02 and 0.9)", s.rms);

	const size_t span_frames = frames - settle;
	const size_t crossings = count_crossings(audio, settle, frames);
	const double hz = double(crossings) / 2.0 * double(DON_SPU2_SAMPLE_RATE) / double(span_frames);
	const double want = double(DON_SPU2_SAMPLE_RATE) / kSquarePeriod;
	check(std::fabs(hz - want) / want < 0.03, "pitch", "%.1f Hz measured, %.1f Hz expected (square of %d samples)", hz, want, kSquarePeriod);

	bool irq_seen = false;
	for (const Event& e : events)
		irq_seen |= e.type == DON_SPU2_EVENT_IRQ;
	check(!irq_seen, "no_irq_without_irq_enable", "no interrupt events in the second of playback");
	std::printf("     peak %.4f, RMS %.4f, %zu frames\n", s.peak, s.rms, frames);

	if (!wav_path.empty())
	{
		const bool ok = write_wav(wav_path, audio);
		check(ok, "wav", "%s (%zu frames)", wav_path.c_str(), frames);
	}
	don_spu2_close();
	return g_failures == before;
}

bool case_irq()
{
	std::printf("-- case 2: SPU2 interrupt, sample uploaded through core 1 (DMA channel 7)\n");
	const int before = g_failures;
	check(don_spu2_open() == 0, "open_again", "open after close gives a clean state");

	std::vector<Event> events;
	std::vector<float> audio;
	const std::vector<uint8_t> sample = make_square_sample(512);
	upload(1, kSsa, sample);
	advance_collect(20000, events, audio);
	check(events.size() == 1 && events[0].type == DON_SPU2_EVENT_DMA_END_1, "dma_end_event_core1", "%zu event(s)%s", events.size(),
	      events.size() == 1 && events[0].type == DON_SPU2_EVENT_DMA_END_1 ? ", end of DMA core 1" : "");

	// Interrupt address inside the sample: the second block (8 halfwords after the first).
	events.clear();
	don_spu2_write16(core_reg(1, ATTR), kAttrEnable | kAttrIrq);
	write32(core_reg(1, IRQA), kSsa + 8);
	const uint64_t key_cycle = don_spu2_cycle();
	play_voice(1, kSsa);
	advance_collect(kOneSecond / 10, events, audio);
	int irq_events = 0;
	uint64_t first_irq = 0;
	for (const Event& e : events)
		if (e.type == DON_SPU2_EVENT_IRQ)
		{
			if (!irq_events)
				first_irq = e.cycle;
			irq_events++;
		}
	check(irq_events >= 1, "irq_event", "%d interrupt event(s) in 100 ms", irq_events);
	check(irq_events >= 1 && first_irq >= key_cycle && first_irq < key_cycle + 40 * 768, "irq_cycle",
	      "first at IOP cycle %" PRIu64 ", key-on at %" PRIu64 " (+%" PRId64 ")", first_irq, key_cycle,
	      int64_t(first_irq) - int64_t(key_cycle));
	check(don_spu2_read16(core_reg(1, ATTR)) == (kAttrEnable | kAttrIrq), "read16", "ATTR reads back as written");

	// Reset must clear time, events and sound memory.
	don_spu2_reset();
	check(don_spu2_cycle() == 0 && !don_spu2_poll_event(nullptr, nullptr), "reset", "time %" PRIu64 ", no events", don_spu2_cycle());
	check(don_spu2_open() < 0, "open_twice", "second open is refused");
	don_spu2_close();
	check(don_spu2_poll_event(nullptr, nullptr) == 0 && don_spu2_pull_output(nullptr, 1) == 0, "closed", "calls after close do nothing");
	return g_failures == before;
}
} // namespace

int main(int argc, char** argv)
{
	std::string wav;
	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
			wav = argv[++i];
		else
		{
			std::fprintf(stderr, "usage: %s [--wav <file>]\n", argv[0]);
			return 2;
		}
	}
	case_tone(wav);
	case_irq();
	std::printf("%s: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
