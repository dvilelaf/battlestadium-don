// SPDX-License-Identifier: GPL-3.0-or-later
// See gs_trace.h: T-202's one-frame GIF capture, off unless DON_GS_PACKETS names a file.
#include "gs_trace.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct State
    {
        bool initialised = false;
        bool on = false;                       // DON_GS_PACKETS was given
        uint64_t target = 0;                   // the frame to record (0 = not known yet)
        bool target_known = false;
        uint64_t frames_seen = 0;              // how many vsyncs were reported
        bool recording = false;                // the target frame is the one being collected
        bool done = false;
        std::string path;
        std::vector<uint8_t> buffer;           // the records of the frame being collected
        uint64_t records = 0;
        uint64_t bytes = 0;
    };

    State &state()
    {
        static State s;
        return s;
    }

    void init()
    {
        State &s = state();
        s.initialised = true;
        const char *file = std::getenv("DON_GS_PACKETS");
        if (file == nullptr || file[0] == '\0')
            return;
        s.on = true;
        s.path = file;
        const char *frame = std::getenv("DON_GS_PACKETS_FRAME");
        if (frame != nullptr && frame[0] != '\0')
        {
            char *end = nullptr;
            const unsigned long long value = std::strtoull(frame, &end, 10);
            if (end != frame && *end == '\0' && value > 0)
            {
                s.target = value;
                s.target_known = true;
            }
        }
        // Without a frame number the last frame of the run cannot be known in advance: record from
        // the second vertical blank on and let the caller stop the run with the script's quit.
        if (!s.target_known)
            s.target = 2;
        std::fprintf(stderr, "[don-gs] GIF capture of frame %llu into %s (DON_GS_PACKETS)\n",
                     static_cast<unsigned long long>(s.target), s.path.c_str());
    }

    void write_file()
    {
        State &s = state();
        std::FILE *file = std::fopen(s.path.c_str(), "wb");
        if (file == nullptr)
        {
            std::fprintf(stderr, "[don-gs] cannot write %s\n", s.path.c_str());
            return;
        }
        const char magic[] = "DONGS1\n";
        std::fwrite(magic, 1, sizeof(magic) - 1, file);
        const uint32_t frame = static_cast<uint32_t>(s.target);
        std::fwrite(&frame, sizeof(frame), 1, file);
        const std::size_t written = std::fwrite(s.buffer.data(), 1, s.buffer.size(), file);
        std::fclose(file);
        std::fprintf(stderr, "[don-gs] GIF capture: frame %llu, %llu transfer(s), %llu bytes, "
                             "%zu written to %s\n",
                     static_cast<unsigned long long>(s.target),
                     static_cast<unsigned long long>(s.records),
                     static_cast<unsigned long long>(s.bytes), written, s.path.c_str());
    }
} // namespace

bool gs_trace::enabled()
{
    State &s = state();
    if (!s.initialised)
        init();
    return s.on;
}

void gs_trace::transfer(int path, const uint8_t *data, uint32_t bytes)
{
    State &s = state();
    if (!s.initialised)
        init();
    if (!s.on || s.done || data == nullptr || bytes == 0)
        return;
    if (!s.recording)
    {
        // The transfers of frame N arrive before its vsync, that is while the previous frame was
        // last reported: start collecting one frame early.
        if (!(s.frames_seen + 1 == s.target))
            return;
        s.recording = true;
        s.buffer.reserve(1 << 20);
    }
    const uint8_t path_byte = static_cast<uint8_t>(path);
    const uint32_t size = bytes;
    s.buffer.insert(s.buffer.end(), &path_byte, &path_byte + 1);
    s.buffer.insert(s.buffer.end(), reinterpret_cast<const uint8_t *>(&size),
                    reinterpret_cast<const uint8_t *>(&size) + sizeof(size));
    s.buffer.insert(s.buffer.end(), data, data + bytes);
    ++s.records;
    s.bytes += bytes;
}

void gs_trace::vsync(uint64_t frame)
{
    State &s = state();
    if (!s.initialised)
        init();
    if (!s.on || s.done)
        return;
    s.frames_seen = frame;
    if (s.recording && frame >= s.target)
    {
        s.recording = false;
        s.done = true;
        write_file();
    }
}
