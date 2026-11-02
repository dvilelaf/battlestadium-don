// SPDX-License-Identifier: GPL-3.0-or-later
// Sound output of the host program (T-303): the frames the SPU2 produces go to a 16-bit WAV file and,
// unless the run is headless, to an SDL3 audio stream. The pure part (ring, float to 16-bit conversion,
// WAV writer) has no SDL and is built and tested everywhere; the SDL3 part is compiled only when
// DON_AUDIO_SDL is defined (the host program and the container tests), and it is the only place of the
// sound output that touches SDL (the SPU2 glue also uses SDL's portable library loader, like gs_glue).
//
// The WAV survives an abrupt end. After every write() the file on disk is a complete, readable WAV that
// holds every frame written so far: the sample bytes are written and flushed first, and only then are the
// two size fields of the header updated and flushed. The header therefore never claims more data than is
// on disk, and a process that dies (or is killed, or calls std::_Exit) at any point leaves a file that is
// readable up to the last completed write. close() does the last update and closes the file.
#pragma once

#include "audio_prebuffer.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The rate and layout of the SPU2 output and of the WAV (don_spu2.h: DON_SPU2_SAMPLE_RATE, stereo).
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint32_t kAudioChannels = 2;
// Bounds of the output queue, in stereo frames: 1 s in the ring, 0.25 s inside SDL's stream.
constexpr uint32_t kDefaultRingFrames = 48000;
constexpr uint32_t kDefaultDeviceQueueFrames = 12000;
// The cushion built before the device starts and after every underrun: 100 ms.
constexpr uint32_t kDefaultPrebufferFrames = 4800;

// A float sample to the 16-bit value stored in the WAV. -1 gives -32767, 0 gives 0 and +1 gives +32767;
// anything outside -1..1 is clamped to the nearest end, and NaN (which a broken voice could produce) is 0,
// so no input can produce an out-of-range or undefined value. 32767 (not 32768) keeps the two ends
// symmetric.
int16_t float_to_i16(float value);

// A ring of stereo float frames between the producer (the frames pulled from the SPU2) and the consumer
// (the device, through the SDL stream). Thread-safe: the producer and the consumer may be different
// threads. Order is kept and the storage wraps around; the capacity is fixed, so memory never grows.
class AudioRing
{
public:
    explicit AudioRing(uint32_t capacity_frames = kDefaultRingFrames);

    uint32_t capacity() const { return m_capacity; }
    uint32_t size() const;
    uint32_t free_frames() const;

    // Copies `frames` frames in. When they do not fit, the oldest queued frames are dropped, so the
    // newest audio is the one that is kept (a consumer that is late hears a short gap, not ever-older
    // sound). Returns how many frames of `stereo` were stored (all of them when frames <= capacity).
    uint32_t push(const float *stereo, uint32_t frames);

    // Copies the oldest queued frames (at most `frames`) into `stereo` and removes them. Returns how many
    // were copied; fewer than asked when fewer are queued. Never blocks and never pads.
    uint32_t pop(float *stereo, uint32_t frames);

    // Frames dropped by push() because the ring was full, since construction.
    uint64_t dropped() const;

    void clear();

private:
    mutable std::mutex m_mutex;
    std::vector<float> m_data; // capacity() * kAudioChannels
    uint32_t m_capacity;
    uint64_t m_read = 0;  // frame counters; the index in m_data is counter % capacity
    uint64_t m_write = 0;
    uint64_t m_dropped = 0;
};

// A 16-bit stereo PCM WAV at 48,000 Hz (the SPU2 output format). See the file comment for the guarantee
// about abrupt ends. Not thread-safe; the producer thread owns it.
class WavWriter
{
public:
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter &) = delete;
    WavWriter &operator=(const WavWriter &) = delete;

    // Creates the file and writes a header with zero sizes. Missing parent folders are created (the same
    // thing the screenshot writer does for --shots). false and a message in `err` on failure.
    bool open(const std::filesystem::path &path, std::string &err);
    bool is_open() const { return m_file != nullptr; }
    const std::filesystem::path &path() const { return m_path; }

    // Converts (float_to_i16) and appends `frames` stereo frames, then updates the header sizes as
    // described in the file comment.
    void write(const float *stereo, uint32_t frames);

    // Last header update, flush and close. false when the file could not be flushed. Safe to call twice.
    bool close();

    uint64_t frames_written() const { return m_frames; }

private:
    bool write_header_sizes();

    std::FILE *m_file = nullptr;
    std::filesystem::path m_path;
    uint64_t m_frames = 0;
    bool m_failed = false;
    bool m_size_warned = false;
};

struct AudioOutputParams
{
    std::filesystem::path wav_path;              // empty: no WAV is written
    bool open_device = false;                    // false (headless): nothing is opened, only the WAV
    uint32_t ring_frames = kDefaultRingFrames;   // bound of the queue (frames)
    uint32_t device_queue_frames = kDefaultDeviceQueueFrames; // how far ahead of the device to feed SDL
    uint32_t prebuffer_frames = kDefaultPrebufferFrames;      // queued before the device plays (start and after an underrun)
};

// The output side. The host loop is the producer: at least once per loop iteration (after each slice of
// emulation; the SPU2 keeps its own buffer of about 5 s, so a late loop loses nothing) it pulls every frame
// the SPU2 has produced and calls write() with them. With a device, write() queues the frames in the ring
// and feeds SDL's own stream buffer up to AudioOutputParams::device_queue_frames; SDL's audio thread then
// drains that buffer at the device's rate. Bounds (the card): when the game runs faster than real time the
// ring fills and push() drops the oldest frames (counted by frames_dropped()); memory never grows past
// ring_frames plus the SDL queue. When it runs slower than real time (today about a quarter of full speed
// on some screens) the ring empties, SDL's stream runs out and the device plays silence: the sound has gaps.
// Nothing resamples or stretches time. The WAV always gets every frame handed to write(), including the ones
// the device dropped.
//
// Not thread-safe: init(), write(), shutdown() and the accessors are called from the host loop's thread
// (SDL's own audio thread only drains the stream). With no device (headless, or no audio hardware) the
// frames go to the WAV only.
class HostAudio
{
public:
    HostAudio();
    ~HostAudio();
    HostAudio(const HostAudio &) = delete;
    HostAudio &operator=(const HostAudio &) = delete;

    // Opens the WAV (if asked for) and the device (if asked for; a device that cannot be opened is not
    // fatal: it prints a "[don-spu2]" line and the frames go to the WAV only). Returns false only when the
    // WAV was asked for and could not be created.
    bool init(const AudioOutputParams &params, std::string &err);
    void shutdown();

    // The producer call. `stereo` is interleaved left/right floats in -1..1; `frames` is the frame count.
    void write(const float *stereo, uint32_t frames);

    bool device_open() const { return m_device != nullptr; }
    uint64_t frames_written() const { return m_frames; }
    uint64_t frames_dropped() const { return m_dropped; }   // overruns: frames the full ring dropped
    uint64_t underruns() const { return m_prebuffer.underruns(); } // real underruns: the stream ran dry while playing
    // T-303: frames queued for the device, in the ring and inside SDL's stream (the sound that has been
    // produced but not heard yet; the device's own small buffer is not counted).
    uint32_t ring_queued() const { return m_ring ? m_ring->size() : 0; }
    uint32_t stream_queued() const;
    uint32_t queued_frames() const { return ring_queued() + stream_queued(); }
    // Playback ratio of the device stream (1.0 = nominal; see audio_rate.h). Safe to call from the game thread: SDL
    // guards the stream.
    void set_rate(double ratio);
    double rate() const { return m_rate; }
    // Drops the oldest queued sound in the ring, whole frames; returns how many were dropped.
    uint32_t drop_oldest(uint32_t frames);
    double seconds_written() const { return double(m_frames) / double(kAudioSampleRate); }

    // One "[don-spu2]" line if DON_AUDIO_LOG=1: frames written, seconds and frames the device dropped.
    void print_summary() const;

private:
    struct Device; // the SDL3 audio stream, defined in audio.cpp only when DON_AUDIO_SDL is defined

    bool open_device(std::string &err);
    void feed_device();

    AudioOutputParams m_params;
    std::unique_ptr<Device> m_device;
    std::unique_ptr<AudioRing> m_ring; // created by init() with the requested capacity
    WavWriter m_wav;
    uint64_t m_frames = 0;
    uint64_t m_dropped = 0;
    AudioPrebuffer m_prebuffer;
    double m_rate = 1.0;
    bool m_device_warned = false;
    bool m_audio_owned = false; // this object started SDL's audio subsystem (SDL build only)
};
