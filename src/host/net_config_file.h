// SPDX-License-Identifier: GPL-3.0-or-later
// net.ini (T-517, docs/knowledge/22-netplay.md section 7.2): the start screen remembers the player's name, the
// last address and the last player count between runs, and nothing about a session. The file lives in the one
// user folder (<user>/net.ini; see user_dir.h), never next to the game.
//
// Plain C++ only (std::filesystem), so the unit tests use it without SDL and without the runtime.
#pragma once

#include <filesystem>
#include <string>

namespace don_host
{

struct NetConfigFile
{
    std::string name;    // the last name; empty = the OS user name
    std::string address; // the last address: an IP or "host[:port]"
    int players = 2;     // the last player count chosen when hosting, 2..4
    int port = 7877;     // the host's UDP port, 1024..65535 (the Settings' HOST PORT row)
    // 7.3 (T-522): the Settings and LocalSetup values, remembered between runs. Old files without these keys
    // load fine (the defaults stand).
    int local_players = 1;   // the last local player count, 1..4
    bool fullscreen = false; // the game window opens fullscreen
    int renderer = 0;        // 0 Vulkan, 1 software
    bool vibration = true;   // the game's vibration on the gamepads
    int net_delay = 0;       // 0 = automatic, 1..10 frames (hosting)
};

// The per-user configuration folder: $XDG_CONFIG_HOME/don, else ~/.config/don, else %APPDATA%\don.
std::filesystem::path don_net_config_dir();

// don_net_config_dir()/"net.ini".
std::filesystem::path don_net_config_path();

// Reads key=value lines. A missing file is not an error: out keeps its values and the call returns true. A
// file that exists and cannot be read, or is a folder, is one: the call returns false and fills error.
bool don_net_config_load(const std::filesystem::path &path, NetConfigFile &out, std::string &error);

// Writes the values, creating the folder. The call writes "<path>.tmp" and renames it over path, so a
// crash during the write leaves the previous file in place.
bool don_net_config_save(const std::filesystem::path &path, const NetConfigFile &in, std::string &error);

// The name of the user of the session: $USER, $LOGNAME or %USERNAME% (empty when none is set).
std::string don_os_user_name();

} // namespace don_host
