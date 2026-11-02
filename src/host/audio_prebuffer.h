// SPDX-License-Identifier: GPL-3.0-or-later
// The start/underrun cushion of the audio output, as a pure state machine (no SDL), so it can be tested.
// The device stays paused until `prebuffer` frames are queued; when the device's stream runs dry while it is
// playing, that is a real underrun: it is counted, the device is paused and it resumes only after the queue
// has been refilled to the cushion. Without this, every underrun is a burst of silence in the middle of a
// waveform (a click) followed by a trickle that runs dry again.
#pragma once

#include <cstdint>

class AudioPrebuffer
{
public:
    // Call before feeding the stream, with the frames queued in the device's stream. true: pause the device
    // (a real underrun happened; it was counted).
    bool check_underrun(uint32_t stream_frames)
    {
        if (!m_playing || stream_frames != 0)
            return false;
        m_playing = false;
        ++m_underruns;
        return true;
    }

    // Call after feeding, with every frame queued for the device (ring plus stream). true: resume the device.
    bool check_ready(uint32_t total_frames, uint32_t prebuffer_frames)
    {
        if (m_playing || total_frames < prebuffer_frames)
            return false;
        m_playing = true;
        return true;
    }

    bool playing() const { return m_playing; }
    uint64_t underruns() const { return m_underruns; }

private:
    bool m_playing = false; // the device starts paused: the first cushion is built before it plays
    uint64_t m_underruns = 0;
};
