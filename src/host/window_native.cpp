// SPDX-License-Identifier: GPL-3.0-or-later
// The only file of the host that knows which window system SDL3 is running on: it reads the native handles
// from the window's properties (SDL3 provides them on every platform, no system header is needed here) and
// maps them to the DON_GS_WINDOW_* kinds of the GS library.
#include "window.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>

bool don_native_window_handle(SDL_Window *window, DonGsConfig &cfg, std::string &err)
{
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!props || !driver)
    {
        err = "SDL gave no window properties";
        return false;
    }

    if (std::strcmp(driver, "x11") == 0)
    {
        void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        const int64_t xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (!display || xwindow == 0)
        {
            err = "SDL gave no X11 display or window";
            return false;
        }
        cfg.window_type = DON_GS_WINDOW_X11;
        cfg.native_display = display;
        cfg.native_window = reinterpret_cast<void *>(static_cast<uintptr_t>(xwindow));
        return true;
    }
    if (std::strcmp(driver, "wayland") == 0)
    {
        void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        void *surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (!display || !surface)
        {
            err = "SDL gave no Wayland display or surface";
            return false;
        }
        cfg.window_type = DON_GS_WINDOW_WAYLAND;
        cfg.native_display = display;
        cfg.native_window = surface;
        return true;
    }
    if (std::strcmp(driver, "windows") == 0)
    {
        void *hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd)
        {
            err = "SDL gave no Win32 window handle";
            return false;
        }
        cfg.window_type = DON_GS_WINDOW_WIN32;
        cfg.native_display = nullptr;
        cfg.native_window = hwnd;
        return true;
    }
    err = std::string("the GS cannot draw into a window of the SDL video driver '") + driver +
          "' (supported: x11, wayland, windows)";
    return false;
}
