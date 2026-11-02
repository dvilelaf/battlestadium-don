// SPDX-License-Identifier: GPL-3.0-or-later
// net.ini: see net_config_file.h.
#include "net_config_file.h"

#include "user_dir.h"

#include <cstdlib>
#include <fstream>
#include <system_error>

namespace don_host
{
namespace
{

std::string getenv_string(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string trim(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

// A non-negative decimal integer, or -1 when the value is empty, has a non-digit, or overflows.
long parse_int(const std::string &value)
{
    if (value.empty())
        return -1;
    long n = 0;
    for (char c : value)
    {
        if (c < '0' || c > '9')
            return -1;
        n = n * 10 + (c - '0');
        if (n > 1000000)
            return -1;
    }
    return n;
}

} // namespace

std::string don_os_user_name()
{
    std::string name = getenv_string("USER");
    if (name.empty())
        name = getenv_string("LOGNAME");
#ifdef _WIN32
    if (name.empty())
        name = getenv_string("USERNAME");
#endif
    return trim(name);
}

std::filesystem::path don_net_config_dir()
{
    // T-524: net.ini lives in the one user folder (see user_dir.h), not in ~/.config/don anymore.
    return don_user_dir();
}

std::filesystem::path don_net_config_path() { return don_net_config_dir() / "net.ini"; }

bool don_net_config_load(const std::filesystem::path &path, NetConfigFile &out, std::string &error)
{
    error.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return true; // nothing remembered yet
    if (!std::filesystem::is_regular_file(path, ec))
    {
        error = path.string() + " is not a file";
        return false;
    }
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot read " + path.string();
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue; // a line that is not key=value is ignored, not an error
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "name")
            out.name = value;
        else if (key == "address")
            out.address = value;
        else if (key == "players")
        {
            const long n = parse_int(value);
            if (n >= 2 && n <= 4)
                out.players = static_cast<int>(n);
        }
        else if (key == "local_players")
        {
            const long n = parse_int(value);
            if (n >= 1 && n <= 4)
                out.local_players = static_cast<int>(n);
        }
        else if (key == "port")
        {
            const long n = parse_int(value);
            if (n >= 1024 && n <= 65535)
                out.port = static_cast<int>(n);
        }
        else if (key == "fullscreen")
        {
            const long n = parse_int(value);
            if (n == 0 || n == 1)
                out.fullscreen = (n == 1);
        }
        else if (key == "renderer")
        {
            const long n = parse_int(value);
            if (n == 0 || n == 1)
                out.renderer = static_cast<int>(n);
        }
        else if (key == "vibration")
        {
            const long n = parse_int(value);
            if (n == 0 || n == 1)
                out.vibration = (n == 1);
        }
        else if (key == "net_delay")
        {
            const long n = parse_int(value);
            if (n >= 0 && n <= 10)
                out.net_delay = static_cast<int>(n);
        }
    }
    if (!in.eof() && in.fail())
    {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

bool don_net_config_save(const std::filesystem::path &path, const NetConfigFile &in, std::string &error)
{
    error.clear();
    std::error_code ec;
    const std::filesystem::path dir = path.parent_path();
    if (!dir.empty())
    {
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir))
        {
            error = "cannot create " + dir.string() + ": " + ec.message();
            return false;
        }
    }
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + tmp.string();
            return false;
        }
        out << "# Battle Stadium D.O.N: what the start screen remembers (name, address, players, settings).\n";
        out << "name=" << in.name << "\n";
        out << "address=" << in.address << "\n";
        out << "players=" << in.players << "\n";
        out << "local_players=" << in.local_players << "\n";
        out << "port=" << in.port << "\n";
        out << "fullscreen=" << (in.fullscreen ? 1 : 0) << "\n";
        out << "renderer=" << in.renderer << "\n";
        out << "vibration=" << (in.vibration ? 1 : 0) << "\n";
        out << "net_delay=" << in.net_delay << "\n";
        out.flush();
        if (!out)
        {
            error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        // Windows' rename does not overwrite an existing file.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            error = "cannot replace " + path.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

} // namespace don_host
