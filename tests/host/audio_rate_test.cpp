// SPDX-License-Identifier: GPL-3.0-or-later
// The rate control of the audio output (src/host/audio_rate.h).
#include "../../src/host/audio_rate.h"

#include <cmath>
#include <cstdio>

namespace
{
    int failures = 0;
    void expect(bool ok, const char *what)
    {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    }
    bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
} // namespace

int main()
{
    const double target = 4800.0; // 100 ms
    expect(near(audio_rate_ratio(target, target), 1.0), "at target: ratio 1");
    expect(audio_rate_ratio(target * 1.05, target) > 1.0, "above target: faster");
    expect(audio_rate_ratio(target * 0.95, target) < 1.0, "below target: slower");
    expect(near(audio_rate_ratio(target * 1.1, target), 1.005), "10% above: 1.005 (gain 0.05)");
    expect(near(audio_rate_ratio(target * 1.02, target), 1.001), "2% above: 1.001");
    expect(near(audio_rate_ratio(target * 10, target), kAudioRateMax), "far above: clamped to max");
    expect(near(audio_rate_ratio(0, target), 1.0 - kAudioRateGain) || near(audio_rate_ratio(0, target), kAudioRateMin),
           "empty: clamped to min");
    expect(near(audio_rate_ratio(0, target), kAudioRateMin), "empty is the minimum");
    expect(near(audio_rate_ratio(100, 0), 1.0), "no target: ratio 1");
    // Symmetric around the target.
    expect(near(audio_rate_ratio(target * 1.03, target) - 1.0, 1.0 - audio_rate_ratio(target * 0.97, target)),
           "symmetric");

    // Smoothing converges and does not jump.
    double s = 0.0;
    s = audio_level_smooth(s, 1000.0);
    expect(near(s, 50.0), "one step moves 5% of the way");
    for (int i = 0; i < 200; ++i)
        s = audio_level_smooth(s, 1000.0);
    expect(std::fabs(s - 1000.0) < 1.0, "converges to the input");

    // Closed loop: a queue fed 1% faster than real time settles near the target within the clamp.
    double q = target, smoothed = target;
    for (int frame = 0; frame < 20000; ++frame)
    {
        smoothed = audio_level_smooth(smoothed, q);
        const double ratio = audio_rate_ratio(smoothed, target);
        q += 800.0 * 1.003 - 800.0 * ratio; // 800 frames per game frame in, 800*ratio out
    }
    expect(std::fabs(q - target) < target * 0.2, "closed loop settles near the target for a 0.3% faster producer");

    // Ceiling.
    expect(audio_overflow_drop(19200, 19200, 4800) == 0, "at the ceiling: nothing dropped");
    expect(audio_overflow_drop(20000, 19200, 4800) == 15200, "above: dropped back to the target");
    expect(audio_overflow_drop(3000, 0, 4800) == 0, "below target: nothing");

    if (failures != 0)
        return 1;
    std::printf("audio_rate_test: all checks passed\n");
    return 0;
}
