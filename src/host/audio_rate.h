// SPDX-License-Identifier: GPL-3.0-or-later
// Rate control of the audio output, as pure functions (no SDL), so they can be tested. The game paces itself to
// real time; the sound follows it. Instead of holding the game back while too much sound is queued, the device is
// played a little faster when the queue is above its target and a little slower when it is below, so the level
// settles on the target without ever blocking the game thread. Only a queue far above the ceiling (the game ran
// well ahead) is cut, in whole frames.
#pragma once

#include <algorithm>
#include <cstdint>

constexpr double kAudioRateMin = 0.995;
constexpr double kAudioRateMax = 1.005;
constexpr double kAudioRateGain = 0.05;      // ratio change per unit of relative queue error (10 % off target: 0.5 %)
constexpr double kAudioRateSmoothing = 0.05; // exponential smoothing of the level per call (one call per frame)

// The playback ratio for a (smoothed) queue level and a target, both in frames: 1 + k * (queued - target) / target,
// clamped to [kAudioRateMin, kAudioRateMax]. A target of 0 gives 1.
inline double audio_rate_ratio(double queued_frames, double target_frames, double gain = kAudioRateGain)
{
    if (target_frames <= 0.0)
        return 1.0;
    const double r = 1.0 + gain * (queued_frames - target_frames) / target_frames;
    return std::clamp(r, kAudioRateMin, kAudioRateMax);
}

// One step of the exponential smoothing of the queue level.
inline double audio_level_smooth(double smoothed, double queued_frames, double alpha = kAudioRateSmoothing)
{
    return smoothed + alpha * (queued_frames - smoothed);
}

// How many frames to drop from the oldest end when the queue is above the ceiling: enough to come back to the
// target (a whole number of frames), 0 when at or below the ceiling.
inline uint32_t audio_overflow_drop(uint32_t queued_frames, uint32_t ceiling_frames, uint32_t target_frames)
{
    if (queued_frames <= ceiling_frames || queued_frames <= target_frames)
        return 0;
    return queued_frames - target_frames;
}
