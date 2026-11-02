// SPDX-License-Identifier: GPL-3.0-or-later
// Command line of the host program (T-110). This header depends on neither SDL nor the runtime;
// options.cpp uses SDL_GetBasePath (T-519) for the default place of the game and of the memory cards.
#pragma once

#include <cstdint>
#include <string>

struct DonHostOptions
{
    std::string elf_path;     // <ELF path>; empty when the game setup does not need one
    // T-519: no ELF path was given and the game was found in the default place, <folder of the program>/game/
    // SLPS_256.75 (the Windows package and the AppImage put it there). Set to the folder of the program when
    // that happened, empty otherwise. host.cpp then puts the memory cards in <that folder>/saves, unless
    // --mc-dir or DON_MC_DIR says otherwise.
    std::string elf_default_dir;
    std::string iso_path;     // --iso, same effect as the environment variable DON_ISO
    std::string game_module;  // --game-module: the prepared game module to load
    std::string prepare_iso;  // --prepare: disc image to prepare the game from
    bool setup_demo = false;  // --setup-demo
    std::string screenshot_dir; // --screenshot-dir
    std::string setup_auto_pick; // --setup-auto-pick: disc image the setup screen picks by itself
    std::string mc_dir;       // --mc-dir (or DON_MC_DIR): the folder that holds mc0 and mc1; empty = next to the ELF
    bool headless = false;    // --headless: no window, the GS opens without a surface
    double seconds = 0.0;     // --seconds, 0 = run until the window closes
    std::string shots_dir;    // --shots
    uint32_t shot_every = 0;  // --shot-every (vertical blanks); 0 = no screenshots
    int players = 2;          // --players 2|4 (T-306): 4 presents a multitap in port 0 (four pads), 2 is two ports
    bool players_given = false; // --players was given (a command-line option wins over the start screen)
    std::string input_script; // --input-script: the input script to play (T-112, docs/INPUT_SCRIPTS.md)
    int renderer = 0;         // --renderer: DON_GS_RENDERER_VULKAN (0) or DON_GS_RENDERER_SOFTWARE (1)
    bool renderer_given = false; // --renderer was given
    int upscale = 1;          // --upscale 1..8 (hardware renderer only)
    bool fullscreen = false;  // --fullscreen (7.3): the game window opens fullscreen (F11 / Alt+Enter toggles)
    std::string gs_dir;       // --gs-dir: folder with the GS library (else DON_GS_DIR, else next to the executable)
    // T-205/T-303: the VU1 and SPU2 libraries, both off/absent by default.
    std::string vu_dir;        // --vu-dir: folder with the VU1 library (else DON_VU_DIR, else next to the executable)
    bool sound = true;         // --sound on|off: the SPU2 + audio path (default on, see --sound in the usage)
    bool sound_explicit = false; // --sound was given: a missing library or device is then an error, else a warning
    bool rumble = true;        // --rumble on|off: the game's vibration to the gamepads (default on)
    bool rumble_given = false; // --rumble was given
    std::string wav_path;      // --wav: write what the SPU2 produces to this WAV (not with --sound off)
    std::string spu2_dir;      // --spu2-dir: folder with the SPU2 library (else DON_SPU2_DIR, else next to it)
    // T-504: the net mode (used by T-509). 0 = not given.
    int net_host_port = 0;   // --net-host PORT: host, listen on this UDP port (1..65535)
    std::string net_join;    // --net-join HOST[:PORT]: client, join this host
    int net_players = 0;     // --net-players N: host, total players 2..4 (default 2)
    int net_delay = 0;       // --net-delay N: host, input delay 1..10 (not given = automatic)
    std::string net_name;    // --net-name NAME: shown in the host's log (at most 15 characters)
    bool mute = false;       // --mute: sound is emulated, no audio device is opened
};

enum class DonParseResult
{
    Ok,
    Help,  // --help was given: the usage was printed, exit code 0
    Error, // a message was printed to stderr, exit code 2
};

// argv[0] is the program name. needs_elf: exactly one positional argument (the ELF) is required;
// otherwise at most one is accepted.
DonParseResult don_parse_options(int argc, char **argv, bool needs_elf, DonHostOptions &out);

void don_print_usage(const char *program, bool needs_elf);
