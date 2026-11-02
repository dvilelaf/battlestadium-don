// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"

#include <SDL3/SDL.h>

HostWindow::~HostWindow()
{
    if (m_window)
        SDL_DestroyWindow(m_window);
    if (m_video_initialised)
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

bool HostWindow::create(const char *title, int width, int height, std::string &err)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        err = std::string("cannot start SDL's video: ") + SDL_GetError();
        return false;
    }
    m_video_initialised = true;
    // No SDL_WINDOW_VULKAN: the GS library creates its own Vulkan surface on the native handle, and SDL must
    // not create a graphics context of its own on the window.
    m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!m_window)
    {
        err = std::string("cannot create the window: ") + SDL_GetError();
        return false;
    }
    return true;
}

void HostWindow::set_fullscreen(bool fullscreen)
{
    if (m_window)
        SDL_SetWindowFullscreen(m_window, fullscreen);
}

void HostWindow::toggle_fullscreen()
{
    if (!m_window)
        return;
    const bool fullscreen = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(m_window, !fullscreen);
}

void HostWindow::pixel_size(int &width, int &height) const
{
    width = height = 0;
    if (m_window)
        SDL_GetWindowSizeInPixels(m_window, &width, &height);
}

bool HostWindow::fill_gs_config(DonGsConfig &cfg, std::string &err) const
{
    if (!m_window)
    {
        err = "no window";
        return false;
    }
    if (!don_native_window_handle(m_window, cfg, err))
        return false;
    pixel_size(cfg.width, cfg.height);
    return cfg.width > 0 && cfg.height > 0;
}
