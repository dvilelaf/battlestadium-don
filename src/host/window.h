// SPDX-License-Identifier: GPL-3.0-or-later
// The SDL3 window of the host (main thread only) and the native handle the GS draws into.
#pragma once

#include "don_gs.h"

#include <string>

struct SDL_Window;
union SDL_Event;

class HostWindow
{
public:
    HostWindow() = default;
    ~HostWindow();
    HostWindow(const HostWindow &) = delete;
    HostWindow &operator=(const HostWindow &) = delete;

    // Initialises SDL's video and events and creates a resizable window. false with a message in err.
    bool create(const char *title, int width, int height, std::string &err);

    // Fullscreen (7.3): set it once at start, and toggle it with F11 / Alt+Enter during play.
    void set_fullscreen(bool fullscreen);
    void toggle_fullscreen();

    // Pixel size of the drawable area.
    void pixel_size(int &width, int &height) const;

    // Fills the native-handle fields of cfg (native_display, native_window, window_type, width, height).
    // The per-platform part lives in window_native.cpp. false with a message in err when the video driver
    // is not one the GS supports.
    bool fill_gs_config(DonGsConfig &cfg, std::string &err) const;

    SDL_Window *handle() const { return m_window; }

private:
    SDL_Window *m_window = nullptr;
    bool m_video_initialised = false;
};

// window_native.cpp: the one place that knows the window systems.
bool don_native_window_handle(SDL_Window *window, DonGsConfig &cfg, std::string &err);
