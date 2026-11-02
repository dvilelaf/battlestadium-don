// SPDX-License-Identifier: GPL-3.0-or-later
#include "options.h"

#include "don_gs.h" // DON_GS_RENDERER_* only

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

#include <SDL3/SDL.h>

namespace
{
    // T-519: where the program sits, so that the game and the memory cards can have default places next to
    // it. SDL_GetBasePath answers from the executable itself (not from the current directory, which for a
    // double-clicked program can be anywhere) and needs no SDL_Init; the three *_glue.cpp use it for the
    // same purpose. Never empty: the current directory when SDL cannot say.
    std::filesystem::path program_folder()
    {
        const char *base = SDL_GetBasePath();
        if (base != nullptr && base[0] != '\0')
            return std::filesystem::path(base);
        std::error_code ec;
        const std::filesystem::path here = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : here;
    }

    // The game's main executable inside that folder (the layout of the Windows package and of the AppImage).
    constexpr const char *kDefaultGameDir = "game";
    constexpr const char *kDefaultGameElf = "SLPS_256.75";

    bool parse_double(const char *text, double &out)
    {
        char *end = nullptr;
        const double value = std::strtod(text, &end);
        if (end == text || *end != '\0' || !(value >= 0.0) || value > 1.0e9)
            return false;
        out = value;
        return true;
    }

    bool parse_uint(const char *text, uint32_t &out, uint32_t max_value)
    {
        uint32_t value = 0;
        const char *end = text + std::strlen(text);
        const auto result = std::from_chars(text, end, value);
        if (result.ec != std::errc() || result.ptr != end || value > max_value)
            return false;
        out = value;
        return true;
    }
} // namespace

void don_print_usage(const char *program, bool needs_elf)
{
    std::fprintf(stderr,
                 "usage: %s [options]%s\n"
                 "\n"
                 "  --iso <path>            disc image (same as the environment variable DON_ISO)\n"
                 "  --game-module <path>    prepared game module to load\n"
                 "  --prepare <iso>         prepare the game from this disc image\n"
                 "  --setup-demo            run the setup screen in demo mode\n"
                 "  --screenshot-dir <dir>  folder for setup screenshots\n"
                 "  --setup-auto-pick <iso> disc image the setup screen picks by itself\n"
                 "  --mc-dir <folder>       folder that holds the memory cards mc0 and mc1 (same as DON_MC_DIR;\n"
                 "                          default: next to the executable file given)\n"
                 "  --headless              no window; the GS opens without a surface\n"
                 "  --seconds <n>           end after n seconds with exit code 0\n"
                 "  --shots <folder>        save PNG screenshots in this folder (created if missing)\n"
                 "  --shot-every <frames>   one screenshot every n vertical blanks (default 60 with --shots)\n"
                 "  --input-script <file>   scripted input: pad states, shots and quit at numbered frames (docs/INPUT_SCRIPTS.md)\n"
                 "  --players 2|4           2 (default): one pad per port; 4: a multitap in port 0 with up to four pads\n"
                 "                          (the keyboard is player 1, gamepads follow in the order they were connected)\n"
                 "  --renderer vulkan|software   GS renderer (default vulkan)\n"
                 "  --fullscreen             the game window opens fullscreen (F11 or Alt+Enter toggles it)\n"
                 "  --upscale <n>           internal resolution multiplier 1..8 (hardware renderer only)\n"
                 "  --gs-dir <folder>       folder with the GS library (else DON_GS_DIR, else next to the executable)\n"
                 "  --vu-dir <folder>       folder with the VU1 library (else DON_VU_DIR, else next to the executable);\n"
                 "                          DON_VU1=microvu|runtime|interp|pcsx2interp selects the VU1\n"
                 "                          (default microvu; runtime or interp = the runtime's own interpreter)\n"
                 "  --sound on|off          sound through the SPU2 library (default on; off loads nothing of it)\n"
                 "  --rumble on|off         the game's vibration on the gamepads (default on; the game's own setting\n"
                 "                          Vibration ON/OFF in its key configuration still applies; the keyboard has none)\n"
                 "  --spu2-dir <folder>     folder with the SPU2 library (else DON_SPU2_DIR, else next to the executable)\n"
                 "  --wav <file>            write the SPU2 output to this WAV (not with --sound off)\n"
                 "  --net-host <port>       host an online session on this UDP port (1..65535; up to 4 players)\n"
                 "  --net-join <host[:port]>  join a session at this address (default port 7877); nothing to install\n"
                 "  --net-players <n>       host only: total players, 2..4 (default 2)\n"
                 "  --net-delay <n>         host only: input delay in frames, 1..10 (not given = from the ping)\n"
                 "  --net-name <name>       the name shown in the host's log (at most 15 characters)\n"
                 "  --mute                  net mode: the sound is emulated, no audio device is opened\n"
                 "  --help                  this text\n"
                 "\n"
                 "The ELF path (the game's main executable) may also be given alone, without any option. Without\n"
                 "it, the game is looked for in <folder of the program>/game/SLPS_256.75 and the memory cards\n"
                 "go in <folder of the program>/saves.\n"
                 "\n"
                 "exit codes: 0 normal end or end of --seconds; 2 argument error; 3 the GS (or the window), or the\n"
                 "SPU2 library asked for with --sound on, could not be opened; 4 the runtime ended with an error\n",
                 program, needs_elf ? " <ELF path>" : " [<ELF path>]");
}

DonParseResult don_parse_options(int argc, char **argv, bool needs_elf, DonHostOptions &out)
{
    const char *program = argc > 0 ? argv[0] : "don";
    bool shot_every_given = false;
    bool have_positional = false;

    auto fail = [&](const char *format, const char *arg) {
        std::fprintf(stderr, "[don] ");
        std::fprintf(stderr, format, arg);
        std::fprintf(stderr, "\n[don] try --help\n");
        return DonParseResult::Error;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        auto value = [&](const char *&dst) {
            if (i + 1 >= argc)
                return false;
            dst = argv[++i];
            return true;
        };

        if (arg == "--help" || arg == "-h")
        {
            don_print_usage(program, needs_elf);
            return DonParseResult::Help;
        }
        if (arg == "--headless")
        {
            out.headless = true;
            continue;
        }
        if (arg == "--mute")
        {
            out.mute = true;
            continue;
        }
        if (arg == "--fullscreen")
        {
            out.fullscreen = true;
            continue;
        }
        if (arg == "--setup-demo")
        {
            out.setup_demo = true;
            continue;
        }
        if (arg.size() > 2 && arg.substr(0, 2) == "--")
        {
            const char *v = nullptr;
            if (arg == "--iso" || arg == "--mc-dir" || arg == "--shots" || arg == "--input-script" || arg == "--gs-dir" ||
                arg == "--vu-dir" || arg == "--spu2-dir" || arg == "--wav" || arg == "--game-module" ||
                arg == "--prepare" || arg == "--screenshot-dir" || arg == "--setup-auto-pick")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                if (arg == "--iso")
                    out.iso_path = v;
                else if (arg == "--game-module")
                    out.game_module = v;
                else if (arg == "--prepare")
                    out.prepare_iso = v;
                else if (arg == "--screenshot-dir")
                    out.screenshot_dir = v;
                else if (arg == "--setup-auto-pick")
                    out.setup_auto_pick = v;
                else if (arg == "--mc-dir")
                    out.mc_dir = v;
                else if (arg == "--shots")
                    out.shots_dir = v;
                else if (arg == "--input-script")
                    out.input_script = v;
                else if (arg == "--gs-dir")
                    out.gs_dir = v;
                else if (arg == "--vu-dir")
                    out.vu_dir = v;
                else if (arg == "--spu2-dir")
                    out.spu2_dir = v;
                else
                    out.wav_path = v;
            }
            else if (arg == "--sound")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                out.sound_explicit = true;
                if (std::string_view(v) == "on")
                    out.sound = true;
                else if (std::string_view(v) == "off")
                    out.sound = false;
                else
                    return fail("--sound must be on or off, got '%s'", v);
            }
            else if (arg == "--rumble")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                out.rumble_given = true;
                if (std::string_view(v) == "on")
                    out.rumble = true;
                else if (std::string_view(v) == "off")
                    out.rumble = false;
                else
                    return fail("--rumble must be on or off, got '%s'", v);
            }
            else if (arg == "--players")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                out.players_given = true;
                if (std::string_view(v) == "2")
                    out.players = 2;
                else if (std::string_view(v) == "4")
                    out.players = 4;
                else
                    return fail("--players must be 2 or 4, got '%s'", v);
            }
            else if (arg == "--seconds")
            {
                if (!value(v) || !parse_double(v, out.seconds))
                    return fail("--seconds needs a number of seconds >= 0", "");
            }
            else if (arg == "--shot-every")
            {
                if (!value(v) || !parse_uint(v, out.shot_every, 1000000u) || out.shot_every == 0)
                    return fail("--shot-every needs a number of frames >= 1", "");
                shot_every_given = true;
            }
            else if (arg == "--renderer")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                out.renderer_given = true;
                if (std::string_view(v) == "vulkan")
                    out.renderer = DON_GS_RENDERER_VULKAN;
                else if (std::string_view(v) == "software")
                    out.renderer = DON_GS_RENDERER_SOFTWARE;
                else
                    return fail("--renderer must be vulkan or software, got '%s'", v);
            }
            else if (arg == "--upscale")
            {
                uint32_t n = 0;
                if (!value(v) || !parse_uint(v, n, 8) || n < 1)
                    return fail("--upscale needs a number from 1 to 8", "");
                out.upscale = static_cast<int>(n);
            }
            else if (arg == "--net-host" || arg == "--net-join" || arg == "--net-name")
            {
                if (!value(v))
                    return fail("missing value for %s", argv[i]);
                if (arg == "--net-host")
                {
                    uint32_t port = 0;
                    if (!parse_uint(v, port, 65535) || port < 1)
                        return fail("--net-host needs a port from 1 to 65535", "");
                    out.net_host_port = static_cast<int>(port);
                }
                else if (arg == "--net-join")
                {
                    out.net_join = v;
                }
                else
                {
                    if (std::strlen(v) > 15)
                        return fail("--net-name is at most 15 characters", "");
                    out.net_name = v;
                }
            }
            else if (arg == "--net-players" || arg == "--net-delay")
            {
                uint32_t n = 0;
                if (!value(v) || !parse_uint(v, n, 10))
                    return fail("--net-players needs 2..4, --net-delay needs 1..10", "");
                if (arg == "--net-players")
                {
                    if (n < 2 || n > 4)
                        return fail("--net-players needs a number from 2 to 4", "");
                    out.net_players = static_cast<int>(n);
                }
                else
                {
                    if (n < 1 || n > 10)
                        return fail("--net-delay needs a number from 1 to 10", "");
                    out.net_delay = static_cast<int>(n);
                }
            }
            else
            {
                return fail("unknown option %s", argv[i]);
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-')
            return fail("unknown option %s", argv[i]);
        if (have_positional)
            return fail("more than one ELF path: %s", argv[i]);
        out.elf_path = argv[i];
        have_positional = true;
    }

    if (needs_elf && !have_positional)
    {
        // T-519: the player double-clicks the program and gives nothing. The game is where the Windows
        // package (and the AppImage) put it: <folder of the program>/game/SLPS_256.75. Found: the run goes on
        // (the memory cards are in the user folder, D15: host.cpp). Not found: the usual usage error, which
        // says where it looked.
        std::error_code ec;
        const std::filesystem::path folder = program_folder();
        const std::filesystem::path candidate = folder / kDefaultGameDir / kDefaultGameElf;
        if (!std::filesystem::is_regular_file(candidate, ec))
            return fail("the ELF path is required (no %s)", candidate.string().c_str());
        out.elf_path = candidate.string();
        out.elf_default_dir = folder.string();
        std::fprintf(stderr, "[don] no ELF path: using %s\n", out.elf_path.c_str());
    }
    if (!out.sound && !out.wav_path.empty())
        return fail("--wav needs sound (--sound off was given)%s", "");
    if (shot_every_given && out.shots_dir.empty())
        return fail("--shot-every needs --shots <folder>%s", "");
    if (!out.shots_dir.empty() && out.shot_every == 0)
        out.shot_every = 60;
    const bool net_mode = out.net_host_port != 0 || !out.net_join.empty();
    if (out.net_host_port != 0 && !out.net_join.empty())
        return fail("--net-host and --net-join cannot be given together%s", "");
    if (out.net_players != 0 && out.net_host_port == 0)
        return fail("--net-players needs --net-host%s", "");
    if (out.net_delay != 0 && out.net_host_port == 0)
        return fail("--net-delay needs --net-host%s", "");
    if (net_mode && out.sound_explicit && !out.sound)
        return fail("--sound off cannot be used with --net-host or --net-join%s", "");
    if (net_mode && out.players_given)
        return fail("--players cannot be used with --net-host or --net-join%s", "");
    return DonParseResult::Ok;
}
