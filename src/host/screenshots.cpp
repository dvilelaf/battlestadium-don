// SPDX-License-Identifier: GPL-3.0-or-later
#include "screenshots.h"

#include "gs_glue.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

ScreenshotWriter::ScreenshotWriter(const DonGsApi &api, std::filesystem::path dir, uint32_t every)
    : m_api(api), m_dir(std::move(dir)), m_every(every)
{
    std::error_code ec;
    std::filesystem::create_directories(m_dir, ec);
    if (ec)
    {
        std::fprintf(stderr, "[don] cannot create the screenshot folder %s: %s\n", m_dir.string().c_str(),
                     ec.message().c_str());
        m_every = 0;
    }
}

void ScreenshotWriter::on_vsync(uint64_t frame)
{
    if (m_every == 0 || frame % m_every != 0)
        return;

    if (snapshot() != 0)
        return;

    char name[48];
    std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(frame));
    write(name);
}

// The script's `shot NAME`: the same picture, under the name the script asked for, at that vertical blank.
bool ScreenshotWriter::write_named(const std::string &name)
{
    if (name.empty())
        return false;
    if (snapshot() != 0)
    {
        std::fprintf(stderr, "[don] shot '%s': nothing to show yet\n", name.c_str());
        return false;
    }
    return write(name + ".png");
}

// Fills m_pixels with the picture of the moment. 0 on success; -2 means the display circuits were not
// enabled yet, -3/negative are the GS library's other codes (nothing to show).
int ScreenshotWriter::snapshot()
{
    int width = 0, height = 0;
    int rc = m_api.snapshot_rgba(nullptr, &width, &height, 0);
    if (rc == -3) // dst too small: the size is filled in
    {
        m_pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        rc = m_api.snapshot_rgba(m_pixels.data(), &width, &height, static_cast<int>(m_pixels.size()));
    }
    if (rc != 0)
    {
        // -2: the display circuits were not enabled yet, nothing to show.
        const char *log = std::getenv("DON_HOST_LOG");
        if (log && log[0] == '1')
            std::fprintf(stderr, "[don] no screenshot (snapshot returned %d)\n", rc);
        return rc;
    }
    m_width = width;
    m_height = height;
    return 0;
}

bool ScreenshotWriter::write(const std::string &file_name)
{
    const std::string path = (m_dir / file_name).string();
    SDL_Surface *surface =
        SDL_CreateSurfaceFrom(m_width, m_height, SDL_PIXELFORMAT_RGBA32, m_pixels.data(), m_width * 4);
    if (!surface || !SDL_SavePNG(surface, path.c_str()))
    {
        std::fprintf(stderr, "[don] cannot write %s: %s\n", path.c_str(), SDL_GetError());
        if (surface)
            SDL_DestroySurface(surface);
        return false;
    }
    SDL_DestroySurface(surface);
    ++m_written;
    return true;
}
