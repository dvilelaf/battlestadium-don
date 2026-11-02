// SPDX-License-Identifier: GPL-3.0-or-later
// The one user folder, decided by the program (not by wrappers), for every per-user file the port
// writes: memory cards (memcards/), the GS shader/pipeline cache (gs-cache/), net.ini, launcher.conf
// and don.log. Windows: %APPDATA%\don. Linux: $XDG_DATA_HOME/don, else ~/.local/share/don. Tests and
// developers override it with DON_USER_DIR. Never empty, never throws.
#pragma once

#include <cstdlib>
#include <filesystem>

namespace don_host
{
    inline std::filesystem::path don_user_dir()
    {
        if (const char *override = std::getenv("DON_USER_DIR"); override != nullptr && override[0] != '\0')
            return std::filesystem::path(override);
#ifdef _WIN32
        if (const char *appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0')
            return std::filesystem::path(appdata) / "don";
        if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr && profile[0] != '\0')
            return std::filesystem::path(profile) / "AppData" / "Roaming" / "don";
        return std::filesystem::path(".") / "don";
#else
        if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0')
            return std::filesystem::path(xdg) / "don";
        if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
            return std::filesystem::path(home) / ".local" / "share" / "don";
        return std::filesystem::path(".") / ".local" / "share" / "don";
#endif
    }
} // namespace don_host
