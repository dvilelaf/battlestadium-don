// SPDX-License-Identifier: GPL-3.0-or-later
// The prebuffer/underrun state of the audio output (src/host/audio_prebuffer.h).
#include "../../src/host/audio_prebuffer.h"

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
} // namespace

int main()
{
    const uint32_t cushion = 4800; // 100 ms
    AudioPrebuffer p;
    expect(!p.playing() && p.underruns() == 0, "starts paused, no underruns");
    expect(!p.check_underrun(0), "an empty stream before the first start is not an underrun");
    expect(!p.check_ready(cushion - 1, cushion), "not ready below the cushion");
    expect(!p.playing(), "still paused");
    expect(p.check_ready(cushion, cushion), "ready at the cushion: resume");
    expect(p.playing(), "playing");
    expect(!p.check_ready(cushion * 2, cushion), "no second resume while playing");
    expect(!p.check_underrun(1) && p.underruns() == 0, "a non-empty stream is not an underrun");
    expect(p.check_underrun(0), "an empty stream while playing: pause");
    expect(p.underruns() == 1 && !p.playing(), "counted once, paused");
    expect(!p.check_underrun(0) && p.underruns() == 1, "an empty stream while paused is not counted again");
    expect(!p.check_ready(100, cushion), "a trickle does not resume");
    expect(p.check_ready(cushion + 10, cushion), "refilled: resume");
    expect(p.check_underrun(0) && p.underruns() == 2, "a second underrun counts");
    if (failures != 0)
        return 1;
    std::printf("audio_prebuffer_test: all checks passed\n");
    return 0;
}
