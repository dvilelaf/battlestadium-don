// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.h"

#include "audio.h"
#include "audio_rate.h"
#include "gs_glue.h"
#include "pad_table.h"
#include "perf_window.h"
#include "profile.h"
#include "screenshots.h"
#include "spu2_glue.h"
#include "vu_glue.h"
#include "window.h"

#include "ps2_runtime.h"
// T-207 patch 0034: the scheduler's dispatch counters (EeDispatchStats), for the end-of-run line below.
// The header exists with or without the patch; the line itself is guarded by the macro the patch defines.
#include "runtime/ee_scheduler.h"

#include "input.h"
#include "input_script.h"
#include "video_skip.h"
#include "folder_warmup.h"
#include "pad_layout_flag.h"
#include "input_sdl.h"
#include "log_redirect.h"

#include "build_id.h"
#include "crash_report.h"
#include "sysinfo.h"
#include "user_dir.h"
#include "net_config_file.h"
#include "net_hash.h"
#include "net/game_hash.h"
#include "net/net_pad.h"
#include "net/netplay.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "net/upnp.h"
#include "start_screen.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
    // 7.3 (T-522): what the game uses, from the player's local-play choices. The command line's options are the
    // default; each choice overrides one the command line did not set (a command-line option wins). Used by the
    // start screen's outcome and, when the first-run setup already ended with PLAY (T-720), by net.ini's remembered
    // values: the start screen's model starts from those same values (start_screen_model.cpp:201-208: local players
    // 1..4 else 1, renderer 0/1 else 0, fullscreen, vibration), so skipping the screen equals a plain PLAY on it.
    struct GameChoices
    {
        int players;
        int renderer;
        bool rumble;
        bool fullscreen;
    };

    GameChoices apply_local_choices(const DonHostOptions &options, int local_players, int renderer, bool vibration,
                                    bool fullscreen)
    {
        GameChoices out{options.players, options.renderer, options.rumble, options.fullscreen};
        const int local = (local_players >= 1 && local_players <= 4) ? local_players : 1;
        if (!options.players_given)
            out.players = (local >= 3) ? 4 : 2;
        if (!options.renderer_given)
            out.renderer = (renderer == 0 || renderer == 1) ? renderer : 0;
        if (!options.rumble_given)
            out.rumble = vibration;
        if (!options.fullscreen)
            out.fullscreen = fullscreen;
        return out;
    }

    // Initial window size: the PS2's 640x448 picture, doubled.
    constexpr int kWindowWidth = 1280;
    constexpr int kWindowHeight = 896;

    // Exit codes (the card of T-110).
    constexpr int kExitOk = 0;
    constexpr int kExitArguments = 2;
    constexpr int kExitGs = 3;
    constexpr int kExitRuntime = 4;
    // T-517 (docs/knowledge/22-netplay.md 7.2): the online session ended for everyone (a player gone for 30 s,
    // a desync, the host quitting) after the message box named the reason. The same number as kExitRuntime, and
    // it is the exit code section 7.2 asks for; a plain quit keeps 0.
    constexpr int kExitSessionEnd = 4;

    // T-509/T-517: whatever path run() leaves by (the port in use, a connect that failed, a missing file, a game
    // thread that does not start, an exception, a desync or the player quitting), the session leaves nothing
    // behind: BYE to the peers over the still-open transport, the UPnP mapping removed and the temporary
    // memory-card folder deleted. Declared before the window and the runtime, so it is destroyed after both.
    struct NetSessionCleanup
    {
        std::unique_ptr<don_net::Netplay> *netplay = nullptr;
        std::string *mc_dir = nullptr;

        ~NetSessionCleanup()
        {
            if (netplay != nullptr && *netplay != nullptr)
                (*netplay)->close(don_net::kByeQuit);
            // Nothing when no mapping was made (a client, a LAN, no UPnP library), so this is safe on every
            // path; upnp.h asks for it "at exit, and after a desync or error".
            don_net::upnp_close();
            if (mc_dir != nullptr && !mc_dir->empty())
            {
                std::error_code ec;
                fs::remove_all(*mc_dir, ec);
            }
        }
    };

    // Section 7.2, during play: the lobby's names by wire player index, so that a departing player is named
    // ("Player 3 (Ana) left the game"). Empty entries are players whose name never arrived.
    std::vector<std::string> net_player_names(const don_net::Netplay &netplay, int players)
    {
        std::vector<std::string> names(static_cast<std::size_t>(players > 0 ? players : 0));
        for (const don_net::LobbyPeer &peer : netplay.lobby())
        {
            if (peer.player >= 0 && peer.player < players)
                names[static_cast<std::size_t>(peer.player)] = peer.name;
        }
        return names;
    }

    std::atomic<int> g_signal{0};

    void on_signal(int signal_number) { g_signal.store(signal_number); }

    bool env_flag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && value[0] == '1';
    }

    // T-520: split a multi-line string on '\n' (used to print the GS device block one [don] line each).
    std::vector<std::string> split_lines(const std::string &text)
    {
        std::vector<std::string> lines;
        std::size_t begin = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                std::string line = text.substr(begin, i - begin);
                if (!line.empty())
                    lines.push_back(std::move(line));
                begin = i + 1;
            }
        }
        if (begin < text.size())
            lines.push_back(text.substr(begin));
        return lines;
    }

    int host_process_id()
    {
#ifdef _WIN32
        return static_cast<int>(::_getpid());
#else
        return static_cast<int>(::getpid());
#endif
    }

    // T-524: a folder "has card files" when mc0 or mc1 holds at least one regular file.
    bool has_card_files(const fs::path &dir)
    {
        std::error_code ec;
        for (const char *card : {"mc0", "mc1"})
        {
            for (fs::recursive_directory_iterator it(dir / card, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code e2;
                if (it->is_regular_file(e2))
                    return true;
            }
            ec.clear();
        }
        return false;
    }

    // T-524: the cheap game-data identity for the [don] start block — FNV-1a 64 over the ELF's bytes and
    // the sizes of the data files, without reading the big AFS files. The full hash (hash_game_files) is
    // computed only for online sessions, where it is needed anyway.
    uint64_t data_identity(const fs::path &elf)
    {
        constexpr uint64_t kOffset = 14695981039346656037ull;
        constexpr uint64_t kPrime = 1099511628211ull;
        uint64_t hash = kOffset;
        auto mix = [&](const void *data, std::size_t n) {
            const uint8_t *p = static_cast<const uint8_t *>(data);
            for (std::size_t i = 0; i < n; ++i)
            {
                hash ^= p[i];
                hash *= kPrime;
            }
        };
        auto mix_u64 = [&](uint64_t value) {
            for (int i = 0; i < 8; ++i)
            {
                hash ^= static_cast<uint8_t>(value & 0xFF);
                hash *= kPrime;
                value >>= 8;
            }
        };
        std::error_code ec;
        const uint64_t elf_size = fs::file_size(elf, ec);
        mix_u64(ec ? 0 : elf_size);
        std::ifstream in(elf, std::ios::binary);
        if (in)
        {
            char buf[65536];
            while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
                mix(buf, static_cast<std::size_t>(in.gcount()));
        }
        for (const char *name : {"DATA.AFS", "BGM.AFS", "SE.AFS", "MOVIE.AFS"})
        {
            const std::uintmax_t size = fs::file_size(elf.parent_path() / name, ec);
            mix_u64(ec ? 0 : static_cast<uint64_t>(size));
        }
        return hash;
    }

    // T-524: copy-only migration into the one user folder (never move, never delete), each step logged.
    void migrate_user_data(const fs::path &user_dir)
    {
        std::error_code ec;
        const fs::path memcards = user_dir / "memcards";
        if (!has_card_files(memcards))
        {
            std::vector<fs::path> candidates;
            // The owner's real saves (Linux test bundles): <data>/don-test/saves.
            if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0')
                candidates.push_back(fs::path(xdg) / "don-test" / "saves");
            else if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
                candidates.push_back(fs::path(home) / ".local" / "share" / "don-test" / "saves");
            // The Windows package's saves: <folder of the program>/saves.
            if (const char *base = SDL_GetBasePath())
                candidates.push_back(fs::path(base) / "saves");
            for (const fs::path &src : candidates)
            {
                if (!has_card_files(src))
                    continue;
                fs::create_directories(memcards, ec);
                for (const char *card : {"mc0", "mc1"})
                {
                    if (!fs::exists(src / card, ec))
                        continue;
                    std::error_code copy_ec;
                    fs::copy(src / card, memcards / card,
                             fs::copy_options::recursive | fs::copy_options::copy_symlinks, copy_ec);
                }
                std::fprintf(stderr, "[don] migrate: copied memory cards from %s\n", src.string().c_str());
                break;
            }
        }
        // net.ini from the old config folder (Linux), only when the user folder has none.
        const fs::path net_ini = user_dir / "net.ini";
        if (!fs::exists(net_ini, ec))
        {
            fs::path old;
            if (const char *cfg = std::getenv("XDG_CONFIG_HOME"); cfg != nullptr && cfg[0] != '\0')
                old = fs::path(cfg) / "don" / "net.ini";
            else if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
                old = fs::path(home) / ".config" / "don" / "net.ini";
            if (!old.empty() && fs::is_regular_file(old, ec))
            {
                std::error_code copy_ec;
                fs::copy_file(old, net_ini, copy_ec);
                if (!copy_ec)
                    std::fprintf(stderr, "[don] migrate: copied %s\n", old.string().c_str());
            }
        }
    }

    // SEAM T-111: every SDL event goes to input::SdlInput, which follows gamepads and the keyboard and
    // answers with one HostDeviceState per player. The states are gathered here, on the main thread, and
    // published into the *device* table; the game thread merges them with the input script and publishes
    // the result into the table the runtime reads (see the vblank hook in don_host_run). Two tables, one
    // writer each: PadTable is single-writer, any readers.
    void handle_input_event(input::SdlInput *sdl, const SDL_Event &event)
    {
        if (sdl != nullptr)
            sdl->handle_event(event);
    }

    // Main thread: the device states of this moment into the device table.
    void publish_devices(const input::SdlInput *sdl, PadTable &devices, input::PadLayout layout)
    {
        for (int player = 0; player < input::SdlInput::kMaxPlayers; ++player)
        {
            const input::PortSlot where =
                input::player_to_port_slot(player, layout);
            if (!where.valid)
                continue;
            const Ps2HostPadState state =
                sdl != nullptr ? sdl->pad_state(player) : input::neutral_pad_state(player == 0);
            devices.publish(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot), state);
        }
    }

    // Main thread (T-308): the motors the game set, to the gamepad of each player.
    void drive_rumble(input::SdlInput &sdl, const PadTable &pads, input::PadLayout layout)
    {
        for (int player = 0; player < input::SdlInput::kMaxPlayers; ++player)
        {
            const input::PortSlot where = input::player_to_port_slot(player, layout);
            if (!where.valid)
                continue;
            uint8_t small_motor = 0, large_motor = 0;
            pads.actuators(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot), small_motor, large_motor);
            sdl.set_rumble(player, small_motor, large_motor);
        }
    }

    // Main thread: events of one loop iteration. Returns true when the user asked to leave.
    bool pump_events(HostWindow &window, HostGs &gs, input::SdlInput *sdl)
    {
        bool quit = false;
        SDL_Event event;
        bool have = SDL_WaitEventTimeout(&event, 8);
        while (have)
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE)
                    quit = true;
                else if (event.key.key == SDLK_F11 ||
                         (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
                    window.toggle_fullscreen(); // 7.3: F11 / Alt+Enter during play
                else
                    handle_input_event(sdl, event);
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                gs.request_resize(event.window.data1, event.window.data2);
                break;
            default:
                handle_input_event(sdl, event);
                break;
            }
            have = SDL_PollEvent(&event);
        }
        return quit;
    }

    // Everything between the argument check and the shutdown. Returns the exit code.
    int run(const DonHostOptions &options, const DonHostGame &game)
    {
        const bool log = env_flag("DON_HOST_LOG");

        // T-520: the [don] start block, always printed (no env gate), so a report never has to ask
        // what machine it ran on. init() raises the Windows timer to 1 ms (see sysinfo_win32.cpp).
        don_sysinfo::init();
        const Clock::time_point run_start = Clock::now();
        {
            const don_sysinfo::Info sys = don_sysinfo::gather();
            std::fprintf(stderr, "[don] build %s\n", don_build_id());
            std::fprintf(stderr, "[don] os %s\n", sys.os.c_str());
            std::fprintf(stderr, "[don] cpu %s (%d logical cores)\n", sys.cpu.c_str(), sys.logical_cores);
            std::fprintf(stderr, "[don] ram %llu MiB\n", static_cast<unsigned long long>(sys.ram_total_mib));
#ifdef _WIN32
            // Always a line on Windows, "unknown" when the system did not say (Wine has neither call): a missing
            // line would not tell a report whether it was asked.
            std::fprintf(stderr, "[don] timer %s\n",
                         sys.timer_resolution.empty() ? "unknown" : sys.timer_resolution.c_str());
            std::fprintf(stderr, "[don] power %s (%s)\n", sys.power_plan.empty() ? "unknown" : sys.power_plan.c_str(),
                         sys.power_source.empty() ? "unknown" : sys.power_source.c_str());
#endif
        }

        // T-524: the one user folder (user_dir.h) and the copy-only migration into it (never move or
        // delete). DON_GS_DATA_DIR defaults to <user>/gs-cache here; the variable still overrides.
        const fs::path user_dir = don_host::don_user_dir();
        std::fprintf(stderr, "[don] user %s\n", user_dir.string().c_str());
        // T-524: copy-only migration into the user folder, for a real user only. With DON_USER_DIR set
        // (tests, developers) nothing is migrated, so a test never reads or copies the owner's real save.
        if (std::getenv("DON_USER_DIR") == nullptr || std::getenv("DON_USER_DIR")[0] == '\0')
            migrate_user_data(user_dir);
        if (const char *gs_data = std::getenv("DON_GS_DATA_DIR"); gs_data == nullptr || gs_data[0] == '\0')
            SDL_setenv_unsafe("DON_GS_DATA_DIR", (user_dir / "gs-cache").string().c_str(), 1);

        // 1. The GS library (exit code 3 when it is missing, before anything heavy is built).
        DonGsApi api;
        std::string error;
        if (!don_gs_load(options.gs_dir, api, error))
        {
            std::fprintf(stderr, "[don] %s\n", error.c_str());
            return kExitGs;
        }
        if (log)
            std::fprintf(stderr, "[don] GS library: %s\n", api.path.string().c_str());

        // T-520: the program's folder, the game's data folder and the GS data/cache folder (read before
        // the GS opens, so "existed" describes the state before this run; an empty cache is a first run,
        // the main suspect for a slow first game).
        if (const char *base = SDL_GetBasePath())
            std::fprintf(stderr, "[don] program %s\n", base);
        if (game.needs_elf)
            std::fprintf(stderr, "[don] data %s\n", fs::path(options.elf_path).parent_path().string().c_str());
        {
            const fs::path gs_data =
                std::getenv("DON_GS_DATA_DIR") && std::getenv("DON_GS_DATA_DIR")[0]
                    ? fs::path(std::getenv("DON_GS_DATA_DIR"))
                    : (user_dir / "gs-cache");
            bool cache_existed = false;
            std::uintmax_t cache_files = 0, cache_bytes = 0;
            std::error_code cache_ec;
            if (fs::is_directory(gs_data, cache_ec))
            {
                cache_existed = true;
                for (fs::recursive_directory_iterator it(gs_data, fs::directory_options::skip_permission_denied,
                                                         cache_ec),
                     end;
                     !cache_ec && it != end; it.increment(cache_ec))
                {
                    std::error_code e2;
                    if (!it->is_regular_file(e2))
                        continue;
                    ++cache_files;
                    const std::uintmax_t size = fs::file_size(it->path(), e2);
                    if (!e2)
                        cache_bytes += size;
                }
            }
            std::fprintf(stderr, "[don] gs-data %s (%s, %llu files, %llu bytes)\n", gs_data.string().c_str(),
                         cache_existed ? "existed" : "new", static_cast<unsigned long long>(cache_files),
                         static_cast<unsigned long long>(cache_bytes));
        }

        // 2. T-517: the start screen (docs/knowledge/22-netplay.md 7.2), before the game window opens. It is
        // skipped when the command line already says what to do (--net-host / --net-join), with --headless and
        // with an input script (a script drives the game, not a menu). The screen is the only way a player of
        // the AppImage or the Windows package hosts or joins.
        // T-720: and when the first-run setup just ended with PLAY in this process (game.skip_start_screen): the
        // player already pressed PLAY, so the game starts without asking again.
        const bool no_net_args = options.net_host_port == 0 && options.net_join.empty() && !options.headless &&
                                 options.input_script.empty();
        const bool start_screen = no_net_args && !game.skip_start_screen;
        // Net mode (T-509): the host connects before the game thread starts; the guest's clock, pad layout and
        // pads come from the session. `netplay` is non-null in net mode and the vblank hook uses it. The
        // command line (--net-host / --net-join) sets it here; the start screen (T-517) below may set it too.
        const bool net_from_command_line = options.net_host_port != 0 || !options.net_join.empty();
        bool net_mode = net_from_command_line;
        std::unique_ptr<don_net::Netplay> netplay;
        don_net::NetSession net_session;
        std::string net_mc_dir;
        int net_hash_every = 120;
        NetSessionCleanup cleanup;
        cleanup.netplay = &netplay;
        cleanup.mc_dir = &net_mc_dir;

        // 7.3 (T-522): the values the game will actually use. The command line's options are the default; the
        // start screen's choices override each one the command line did not set (a command-line option wins).
        int game_players = options.players;
        int game_renderer = options.renderer;
        bool game_rumble = options.rumble;
        bool game_fullscreen = options.fullscreen;

        std::unique_ptr<don_host::StartScreenNetLayer> screen_net;
        if (no_net_args && game.skip_start_screen)
        {
            // What a plain PLAY on the start screen would have set (7.3, T-522), from the same net.ini the screen
            // would have shown: the remembered local player count, renderer, vibration and fullscreen.
            don_host::NetConfigFile remembered;
            std::string config_error;
            if (!don_host::don_net_config_load(don_host::don_net_config_path(), remembered, config_error))
                std::fprintf(stderr, "[don] net.ini: %s\n", config_error.c_str());
            const GameChoices c = apply_local_choices(options, remembered.local_players, remembered.renderer,
                                                      remembered.vibration, remembered.fullscreen);
            game_players = c.players;
            game_renderer = c.renderer;
            game_rumble = c.rumble;
            game_fullscreen = c.fullscreen;
            std::fprintf(stderr, "[don] the start screen: skipped, setup ended with PLAY\n");
        }
        if (start_screen)
        {
            // What the screen remembers between runs (net.ini): the name, the last address, the last count.
            // A file that cannot be read is one [don] line: the screen works with the OS user's name.
            don_host::NetConfigFile remembered;
            std::string config_error;
            if (!don_host::don_net_config_load(don_host::don_net_config_path(), remembered, config_error))
                std::fprintf(stderr, "[don] net.ini: %s\n", config_error.c_str());

            don_host::StartScreenRequest request;
            request.remembered.name = remembered.name;
            request.remembered.address = remembered.address;
            request.remembered.port = remembered.port;
            request.remembered.players = remembered.players;
            request.remembered.local_players = remembered.local_players;
            request.remembered.fullscreen = remembered.fullscreen;
            request.remembered.renderer = remembered.renderer;
            request.remembered.vibration = remembered.vibration;
            request.remembered.net_delay = remembered.net_delay;
            request.remembered.default_name = don_host::don_os_user_name();
            request.remembered.build_id = don_build_id();
#ifdef _WIN32
            request.remembered.show_firewall_hint = true;
#endif

            // HELLO carries the build id (build_id.h) and the hash of the game files (game_hash.h): the layer
            // computes the hash only when the player actually hosts or joins.
            screen_net =
                std::make_unique<don_host::StartScreenNetLayer>(don_build_id(), options.elf_path, options.net_delay);

            const don_host::StartScreenOutcome outcome =
                don_host::don_run_start_screen(request, screen_net.get());

            // T-520: the start screen's outcome, always (a report must not have to ask).
            {
                const char *what = outcome.kind == don_host::StartScreenOutcomeKind::Net
                                       ? (outcome.host ? "host" : "join")
                                       : (outcome.kind == don_host::StartScreenOutcomeKind::Quit ? "quit" : "local");
                std::fprintf(stderr, "[don] start %s", what);
                if (outcome.kind == don_host::StartScreenOutcomeKind::Net)
                    std::fprintf(stderr, " (%d players, port %u, delay %d)", outcome.players,
                                 static_cast<unsigned>(outcome.port), outcome.net_delay);
                else if (outcome.kind == don_host::StartScreenOutcomeKind::PlayLocal)
                    std::fprintf(stderr, " (%d player(s))", outcome.local_players);
                std::fprintf(stderr, "\n");
            }

            // net.ini: what the player chose, whatever he does next.
            don_host::NetConfigFile save;
            save.name = outcome.name;
            save.address = outcome.address;
            save.port = outcome.port;
            save.players = outcome.last_players;
            save.local_players = outcome.local_players;
            save.fullscreen = outcome.fullscreen;
            save.renderer = outcome.renderer;
            save.vibration = outcome.vibration;
            save.net_delay = outcome.net_delay;
            std::string save_error;
            if (!don_host::don_net_config_save(don_host::don_net_config_path(), save, save_error))
                std::fprintf(stderr, "[don] net.ini: %s\n", save_error.c_str());

            if (outcome.kind == don_host::StartScreenOutcomeKind::Quit)
            {
                if (log)
                    std::fprintf(stderr, "[don] the start screen: quit\n");
                return kExitOk;
            }
            if (outcome.kind == don_host::StartScreenOutcomeKind::Net)
            {
                net_mode = true;
                netplay = screen_net->take_netplay(); // the UPnP mapping goes with the session (NetSessionCleanup)
                net_session = screen_net->session();
                if (log)
                    std::fprintf(stderr, "[don] the start screen: %s with %d players on UDP %u\n",
                                 net_session.local_player == 0 ? "hosting" : "joined", net_session.players,
                                 static_cast<unsigned>(outcome.port));
            }

            // 7.3 (T-522): the LocalSetup and Settings choices become what the game uses; the command line wins.
            const GameChoices c = apply_local_choices(options, outcome.local_players, outcome.renderer,
                                                      outcome.vibration, outcome.fullscreen);
            game_players = c.players;
            game_renderer = c.renderer;
            game_rumble = c.rumble;
            game_fullscreen = c.fullscreen;

            screen_net.reset(); // the layer is done: it owns nothing now
        }

        // T-520: the renderer asked and the one used, and the game data identity (the netplay hash).
        {
            const char *asked = options.renderer == DON_GS_RENDERER_SOFTWARE ? "software" : "vulkan";
            const char *used = game_renderer == DON_GS_RENDERER_SOFTWARE ? "software" : "vulkan";
            std::fprintf(stderr, "[don] renderer %s (asked) / %s (used)\n", asked, used);
        }
        if (!start_screen && net_from_command_line)
            std::fprintf(stderr, "[don] start %s (command line)\n", options.net_host_port != 0 ? "host" : "join");
        if (game.needs_elf)
            std::fprintf(stderr, "[don] data identity %016llx\n",
                         static_cast<unsigned long long>(data_identity(fs::path(options.elf_path))));

        // 3. The window, on this (the main) thread. Headless: no SDL video at all.
        HostWindow window;
        HostGsParams gs_params;
        gs_params.renderer = game_renderer;
        gs_params.upscale = options.upscale;
        // T-305 (journal 031): the default presents without waiting for the display (the swapchain's mailbox mode,
        // no tearing) and leaves the loop's own 60 Hz clock as the only pace; DON_HOST_VSYNC=1 is the old behaviour
        // (FIFO: the present waits for the refresh), 0 is the same as unset.
        gs_params.vsync = std::getenv("DON_HOST_VSYNC") != nullptr && std::string(std::getenv("DON_HOST_VSYNC")) == "1";
        if (!options.headless)
        {
            if (!window.create(game.title, kWindowWidth, kWindowHeight, error))
            {
                std::fprintf(stderr, "[don] %s\n", error.c_str());
                return kExitGs;
            }
            // 7.3: the start screen's (or the command line's) choice. Set before the GS reads the pixel size,
            // so a fullscreen game opens at the display's size.
            window.set_fullscreen(game_fullscreen);
            if (!window.fill_gs_config(gs_params.window, error))
            {
                std::fprintf(stderr, "[don] %s\n", error.c_str());
                return kExitGs;
            }

            // T-520: the display mode (resolution + refresh) and windowed/fullscreen, always.
            {
                int pixel_w = 0, pixel_h = 0;
                window.pixel_size(pixel_w, pixel_h);
                float refresh = 0.0f;
                int mode_w = 0, mode_h = 0;
                if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window.handle()))
                {
                    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display))
                    {
                        mode_w = mode->w;
                        mode_h = mode->h;
                        refresh = mode->refresh_rate;
                    }
                }
                const bool fullscreen =
                    (SDL_GetWindowFlags(window.handle()) & SDL_WINDOW_FULLSCREEN) != 0;
                std::fprintf(stderr, "[don] display %dx%d @ %.2f Hz, %s (window %dx%d)\n", mode_w, mode_h,
                             refresh, fullscreen ? "fullscreen" : "windowed", pixel_w, pixel_h);
            }
        }

        // Net mode (T-509): `netplay` is non-null in net mode and the vblank hook uses it. A session the start
        // screen set up (T-517) is already here; only the command line's own --net-host / --net-join is done
        // below, before the game starts.
        if (net_from_command_line)
        {
            const bool is_host = options.net_host_port != 0;
            const uint16_t net_port = static_cast<uint16_t>(is_host ? options.net_host_port : 0);

            std::string hash_error;
            const uint64_t data_hash = don_net::hash_game_files(fs::path(options.elf_path), hash_error);
            if (!hash_error.empty())
            {
                std::fprintf(stderr, "[don-net] %s\n", hash_error.c_str());
                return kExitGs;
            }

            // The port is bound first: "the port is in use" is then reported before the router is asked to
            // open it (the mapping of another copy of the game is nobody's to remove).
            std::string transport_error;
            std::unique_ptr<don_net::Transport> transport = don_net::open_udp(is_host ? net_port : 0, transport_error);
            if (!transport)
            {
                std::fprintf(stderr, "[don-net] %s\n", transport_error.c_str());
                return kExitGs;
            }

            // UPnP (host only): map the UDP port on the router. A failure is logged, never fatal.
            if (is_host)
            {
                const don_net::UpnpResult upnp = don_net::upnp_open(net_port);
                if (upnp.mapped)
                    std::fprintf(stderr, "[don-net] players can join at %s:%u (or your domain)\n",
                                 upnp.external_ip.c_str(), static_cast<unsigned>(net_port));
                else
                    std::fprintf(stderr, "[don-net] the router did not open the port (%s): forward UDP %u to %s once on the router\n",
                                 upnp.message.c_str(), static_cast<unsigned>(net_port), upnp.local_ip.c_str());
            }

            // Diagnostics: DON_NET_SIM_* wrap the UDP transport with latency, jitter and loss.
            if (std::getenv("DON_NET_SIM_LATENCY_MS") != nullptr)
            {
                const char *const lat_text = std::getenv("DON_NET_SIM_LATENCY_MS");
                const char *const jit_text = std::getenv("DON_NET_SIM_JITTER_MS");
                const char *const loss_text = std::getenv("DON_NET_SIM_LOSS");
                const char *const seed_text = std::getenv("DON_NET_SIM_SEED");
                const int latency = lat_text != nullptr ? std::atoi(lat_text) : 0;
                const int jitter = jit_text != nullptr ? std::atoi(jit_text) : 0;
                const double loss = loss_text != nullptr ? std::atof(loss_text) : 0.0;
                const uint64_t seed = seed_text != nullptr ? std::strtoull(seed_text, nullptr, 0) : 1u;
                transport = don_net::with_simulated_network(std::move(transport), latency, jitter, loss, seed);
            }

            don_net::NetConfig net_config;
            net_config.host = is_host;
            net_config.port = net_port;
            net_config.join = options.net_join;
            net_config.players = is_host ? (options.net_players != 0 ? options.net_players : 2) : 0;
            net_config.delay = options.net_delay;
            net_config.name = options.net_name;
            net_config.build_id = don_build_id();
            net_config.data_hash = data_hash;
            net_config.hash_every = 120;

            net_hash_every = net_config.hash_every;
            netplay = std::make_unique<don_net::Netplay>(std::move(net_config), std::move(transport));

            std::string connect_error;
            if (!netplay->connect(net_session, connect_error, []() { return g_signal.load() != 0; }))
            {
                std::fprintf(stderr, "[don-net] %s\n", connect_error.c_str());
                return kExitGs;
            }
        }

        // 3. The runtime: the host owns the window, the loop and the GS.
        auto runtime = std::make_unique<PS2Runtime>();
        runtime->setHostOwnsWindow(true);
        if (!runtime->initialize(game.title))
        {
            std::fprintf(stderr, "[don] the runtime could not be initialised\n");
            return kExitRuntime;
        }

        // T-509: in net mode the guest's clock comes from the session (patch 0074).
        if (netplay)
            runtime->setSessionClock(net_session.session_epoch);

        // T-517: net diagnostics. The host's own lines (who joined, the measured delay, "starting with N
        // players") come from Netplay; the client has none, so it logs its side here: whom it joined, its
        // index and the delay the host chose. The end reason is the vblank hook's [don-net] line and the
        // session-end message box below.
        if (netplay && net_session.local_player != 0)
            std::fprintf(stderr, "[don-net] connected to %s (player %d of %d), starting with delay %d\n",
                         netplay->host_name().c_str(), net_session.local_player + 1, net_session.players,
                         net_session.delay);

        if (!options.iso_path.empty())
        {
            // The same thing the environment variable does (src/game/don_overrides.cpp reads DON_ISO and sets
            // the disc image path): do both, so the option wins over a DON_ISO inherited from the shell.
            SDL_setenv_unsafe("DON_ISO", options.iso_path.c_str(), 1);
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.cdImage = fs::path(options.iso_path);
            PS2Runtime::setIoPaths(paths);
        }

        std::unique_ptr<ScreenshotWriter> shots;
        if (!options.shots_dir.empty())
            shots = std::make_unique<ScreenshotWriter>(api, fs::path(options.shots_dir), options.shot_every);
        HostGs gs(api, gs_params, std::move(shots));
        PS2Runtime *runtime_ptr = runtime.get();
        gs.set_on_open_failed([runtime_ptr]() { runtime_ptr->requestStop(); });
        runtime->setHostGs(&gs.iface());
        if (gs.pacing_log())
            gs.set_pacing_wait_source([runtime_ptr]() { return runtime_ptr->eeScheduler().pacingWaitNs(); });
        // DON_AUDIO_LATENCY=1 (T-303): every 5 s of host time, how much sound is queued and the rates.
        const bool lat_log = env_flag("DON_AUDIO_LATENCY");
        Clock::time_point lat_last, lat_t0;
        uint64_t lat_produced = 0, lat_prev_produced = 0, lat_prev_blanks = 0;
        double audio_write_worst_ms = 0.0; // T-305: DON_PACING_LOG, main thread only

        // T-111/T-112: the input script is parsed before the game starts; every error is printed with its line
        // number and nothing is started (exit code 2, like the other argument errors).
        input::InputScript script;
        std::unique_ptr<input::ScriptPlayer> script_player;
        if (!options.input_script.empty())
        {
            const input::ParseResult parsed = input::parse_script_file(fs::path(options.input_script));
            if (!parsed.ok)
            {
                for (const input::ScriptError &error : parsed.errors)
                    std::fprintf(stderr, "[don] %s: %s\n", options.input_script.c_str(), error.to_string().c_str());
                std::fprintf(stderr, "[don] %zu error(s) in the input script: nothing was started\n",
                             parsed.errors.size());
                return kExitArguments;
            }
            script = parsed.script;
            script_player = std::make_unique<input::ScriptPlayer>(script);
            if (log)
                std::fprintf(stderr, "[don] input script %s: %zu command(s), last frame %llu\n",
                             options.input_script.c_str(), script.commands().size(),
                             static_cast<unsigned long long>(script.last_frame()));
        }

        // SDL's gamepad and keyboard reading needs SDL's events, which headless runs do not have: with
        // --headless there is no SdlInput and player 0 is published as connected-and-neutral (the script is
        // the only input). In a windowed run every event goes to SdlInput (handle_input_event).
        std::unique_ptr<input::SdlInput> sdl_input;
        if (!options.headless)
        {
            sdl_input = std::make_unique<input::SdlInput>();
            if (!sdl_input->init())
                std::fprintf(stderr, "[don] no gamepads or keyboard (SDL): %s\n", SDL_GetError());
        }

        // The devices: written by this thread (publish_devices) and read by the game thread, which merges
        // them with the script and publishes into the table the runtime reads.
        // T-306: --players 4 puts a multitap in port 0 (four slots; port 1 stays described, one slot, nobody
        // plugged in) and tells the game's connection query (src/game/don_overrides.cpp) so; --players 2 (the
        // default) is the two-port layout, exactly as before.
        const input::PadLayout layout =
            net_mode ? (net_session.layout == 1 ? input::PadLayout::MultitapPort0
                                                : input::PadLayout::TwoPortsNoMultitap)
                     : (game_players >= 4 ? input::PadLayout::MultitapPort0
                                          : input::PadLayout::TwoPortsNoMultitap);
        don_pads::multitap_in_port0().store(layout == input::PadLayout::MultitapPort0);
        const uint32_t slots_port0 = layout == input::PadLayout::MultitapPort0 ? 4u : 1u;
        PadTable device_pads(2u, slots_port0);
        PadTable pads(2u, slots_port0);
        publish_devices(sdl_input.get(), device_pads, layout);
        {
            // Until the first publish the table says "not connected": player 1 is always connected.
            const Ps2HostPadState neutral = input::neutral_pad_state(true);
            const input::PortSlot where = input::player_to_port_slot(0, layout);
            if (where.valid)
                pads.publish(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot), neutral);
        }
        runtime->setHostPad(&pads.source());

        // The game thread: one step of the input script per presented frame, before the guest reads the pads
        // of that frame, then the merge of the script's state and the devices' state into the pad table.
        // The third source (T-113, video_skip.h): while the game's video player runs, the port holds Start and
        // Circle (Start ends the opening, Circle the ending), after DON_MOVIE_SKIP_AFTER vertical blanks. Without
        // that variable it does so after 240 blanks in a build without FFmpeg and not at all in a build with it
        // (the movies play; the player's own Start skips them). `video_since` is 0 while no player runs.
        uint64_t video_since = 0;
        // DON_MOVIE_SKIP_AFTER=<vertical blanks>: how long the player must have been running before the port
        // holds the buttons (ending the video too early leaves the title frozen, journal 023).
        bool video_auto = don_video::kAutoSkipByDefault;
        uint64_t video_delay = don_video::kDefaultSkipAfterBlanks;
        if (const char *text = std::getenv("DON_MOVIE_SKIP_AFTER"); text != nullptr && text[0] != '\0')
        {
            video_delay = std::strtoull(text, nullptr, 10);
            video_auto = true;
        }
        // DON_TRACE_PAD_ACT=1 (T-308): the motors the game sets, one line per change with the blank it was seen at
        // (the runtime prints the raw calls, "[don-pad]"). Off by default.
        const bool act_trace = env_flag("DON_TRACE_PAD_ACT");
        uint16_t act_seen[input::kScriptPlayers] = {};
        // T-517: a session that ends during play (a player gone for 30 s, a desync, the host quitting) stops the
        // game and, after the game thread is done, shows the reason and exits 4 (section 7.2). The hook runs on
        // the game thread and this is read after it stopped, so it needs no lock.
        std::string net_end_error;
        gs.set_on_vblank([&](uint64_t frame) {
            if (act_trace)
            {
                for (int player = 0; player < input::kScriptPlayers; ++player)
                {
                    const input::PortSlot at = input::player_to_port_slot(player, layout);
                    if (!at.valid)
                        continue;
                    uint8_t small_motor = 0, large_motor = 0;
                    pads.actuators(static_cast<uint32_t>(at.port), static_cast<uint32_t>(at.slot), small_motor, large_motor);
                    const uint16_t now = static_cast<uint16_t>(small_motor | (large_motor << 8));
                    if (now != act_seen[player])
                    {
                        act_seen[player] = now;
                        std::fprintf(stderr, "[don] blank %llu: vibration player %d (port %d slot %d): small %u large %u\n",
                                     static_cast<unsigned long long>(frame), player + 1, at.port, at.slot,
                                     static_cast<unsigned>(small_motor), static_cast<unsigned>(large_motor));
                    }
                }
            }
            const input::FrameActions actions = script_player ? script_player->step(frame) : input::FrameActions{};
            if (netplay)
            {
                // T-509: the local pad is player 0's device merged with the input script's player 0 (scripts
                // drive the local player in tests); the other players' pads come from the session. The
                // automatic video skip is off (players skip with Start).
                Ps2HostPadState device = input::neutral_pad_state(true);
                const input::PortSlot where0 = input::player_to_port_slot(0, layout);
                if (where0.valid)
                    device_pads.read(static_cast<uint32_t>(where0.port), static_cast<uint32_t>(where0.slot), device);
                const Ps2HostPadState local_state = input::merge_pad_states(device, actions.pads[0]);

                don_net::NetPad confirmed[don_net::kMaxPlayers];
                std::string net_error;
                if (!netplay->step(static_cast<uint32_t>(frame), don_net::to_net(local_state), confirmed,
                                   net_error, [&]() { return g_signal.load() != 0 || runtime->isStopRequested(); }))
                {
                    net_end_error = net_error; // "stopped" (a signal, the window) says nothing to the player
                    std::fprintf(stderr, "[don-net] %s\n", net_error.c_str());
                    runtime->requestStop();
                    return;
                }
                for (int player = 0; player < net_session.players; ++player)
                {
                    const input::PortSlot where = input::player_to_port_slot(player, layout);
                    if (!where.valid)
                        continue;
                    pads.publish(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot),
                                 don_net::from_net(confirmed[player]));
                }
                // Every hash_every frames the guest RAM hash goes to the host (a mismatch ends the session).
                if (frame % static_cast<uint64_t>(net_hash_every) == 0)
                {
                    const uint8_t *const ram = runtime->memory().getRDRAM();
                    const uint64_t hash = don_net::ram_hash() != nullptr ? don_net::ram_hash()(ram) : 0ull;
                    std::string hash_error;
                    if (!netplay->report_hash(static_cast<uint32_t>(frame), hash, hash_error))
                    {
                        net_end_error = hash_error; // a desync or a client gone quiet: the box names it
                        std::fprintf(stderr, "[don-net] %s\n", hash_error.c_str());
                        runtime->requestStop();
                        return;
                    }
                }
            }
            else
            {
                bool video_hold = false;
                if (const auto probe = don_video::player_probe(); video_auto && probe != nullptr && probe())
                {
                    if (video_since == 0)
                        video_since = frame + 1;
                    video_hold = frame + 1 >= video_since + video_delay;
                }
                else
                {
                    video_since = 0;
                }
                for (int player = 0; player < input::kScriptPlayers; ++player)
                {
                    const input::PortSlot where =
                        input::player_to_port_slot(player, layout);
                    if (!where.valid)
                        continue;
                    Ps2HostPadState device;
                    device_pads.read(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot), device);
                    Ps2HostPadState state = input::merge_pad_states(device, actions.pads[player]);
                    if (player == 0 && video_hold)
                    {
                        Ps2HostPadState press = input::neutral_pad_state(true);
                        press.buttons = static_cast<uint16_t>(
                            press.buttons & ~((1u << static_cast<unsigned>(input::PadButton::Start)) |
                                              (1u << static_cast<unsigned>(input::PadButton::Circle))));
                        state = input::merge_pad_states(state, press);
                    }
                    pads.publish(static_cast<uint32_t>(where.port), static_cast<uint32_t>(where.slot), state);
                }
            }
            for (const std::string &name : actions.shots)
            {
                if (!gs.write_shot(name))
                    std::fprintf(stderr, "[don] shot '%s': not written (no picture yet)\n", name.c_str());
            }
            if (actions.quit)
            {
                // Once: the hook is called again on the frames until the stop takes effect.
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    if (log)
                        std::fprintf(stderr, "[don] input script: quit at frame %llu\n",
                                     static_cast<unsigned long long>(frame));
                }
                runtime->requestStop();
            }
        });

        // SEAM T-303: the sound path, on unless --sound off (a missing library or device is then one [don] line, no sound). The SPU2 library and the audio output are
        // loaded here, before any IOP module is loaded. The library itself is opened on the game thread
        // (the hook below, at thread_begin, before the guest boots): don_spu2.h asks every call except
        // the pull to come from the thread that opened it, and the table's callbacks are called from the
        // game thread. The main thread only pulls (the one call the library allows from another thread).
        DonSpu2Api spu2_api;
        std::unique_ptr<HostSpu2> spu2;
        std::unique_ptr<HostAudio> audio;
        uint32_t audio_target_ms = 100; // rate-control target of the queued sound (DON_AUDIO_TARGET_MS)
        bool sound_on = net_mode ? true : options.sound;
        if (sound_on)
        {
            std::string spu2_error;
            if (!don_spu2_load(options.spu2_dir, spu2_api, spu2_error))
            {
                // Asked for with --sound on and missing: the same exit code as a missing GS library.
                // By default: one line and the run goes on without sound.
                std::fprintf(stderr, "%s\n", spu2_error.c_str());
                if (options.sound_explicit)
                    return kExitGs;
                std::fprintf(stderr, "[don] no SPU2 library: running without sound (--sound off silences this)\n");
                sound_on = false;
            }
        }
        if (sound_on)
        {
            if (log)
                std::fprintf(stderr, "[don] SPU2 library: %s\n", spu2_api.path.string().c_str());
            audio = std::make_unique<HostAudio>();
            AudioOutputParams audio_params;
            // DON_AUDIO_DEVICE=1 opens the device in headless mode too (with SDL_AUDIO_DRIVER=dummy it plays nothing;
            // used to measure the delay of the sound path).
            audio_params.open_device = !options.headless || env_flag("DON_AUDIO_DEVICE");
            // T-509: --mute emulates the sound processor but opens no audio device.
            if (net_mode && options.mute)
                audio_params.open_device = false;
            audio_params.wav_path = fs::path(options.wav_path);
            // The rate control below holds the queue near audio_target_ms; the prebuffer (the cushion built at the
            // start and after an underrun) must be reachable under it, so it is 60 ms or 10 ms under the target.
            if (const char *env = std::getenv("DON_AUDIO_TARGET_MS"))
                if (const long v = std::strtol(env, nullptr, 10); v >= 10 && v <= 1000)
                    audio_target_ms = static_cast<uint32_t>(v);
            audio_params.prebuffer_frames =
                std::min<uint32_t>(60u * kAudioSampleRate / 1000u, std::max<uint32_t>(audio_target_ms - 10u, 1u) * kAudioSampleRate / 1000u);
            std::string audio_error;
            if (!audio->init(audio_params, audio_error))
            {
                std::fprintf(stderr, "[don] %s\n", audio_error.c_str());
                if (options.sound_explicit || !options.wav_path.empty())
                    return kExitArguments;
                std::fprintf(stderr, "[don] no audio output: running without sound (--sound off silences this)\n");
                audio.reset();
                sound_on = false;
            }
        }
        if (sound_on)
        {
            spu2 = std::make_unique<HostSpu2>(spu2_api);
            HostSpu2 *const spu2_ptr = spu2.get();
            const char *iop_thread_env = std::getenv("DON_IOP_THREAD");
            const bool iop_thread = iop_thread_env && iop_thread_env[0] == '1';
            if (iop_thread)
                spu2_ptr->set_worker_owned();
            gs.set_on_thread_begin([spu2_ptr, iop_thread]() {
                if (iop_thread)
                {
                    // The IOP worker opens the library (its thread_begin) and makes every call but the pull.
                    ps2x::iop::iopSetHostSpu2(&spu2_ptr->iface());
                    return;
                }
                std::string open_error;
                if (!spu2_ptr->open(open_error))
                {
                    // No table is installed: the runtime keeps its own stand-in sound service.
                    std::fprintf(stderr, "%s\n", open_error.c_str());
                    return;
                }
                ps2x::iop::iopSetHostSpu2(&spu2_ptr->iface());
            });
            gs.set_on_thread_end([spu2_ptr]() {
                ps2x::iop::iopSetHostSpu2(nullptr);
                spu2_ptr->close();
            });
        }

        // SEAM T-205: the host VU1 (patch 0020, runtime/ps2_host_vu1.h), selected by DON_VU1. Unset means
        // microVU (the runtime's interpreter gives up on the menus' microprograms, docs/journal/008);
        // "runtime" (also "interp") installs no table at all. A missing library or a refused open is not
        // fatal: the runtime keeps its own interpreter, with one [don-vu] warning.
        std::unique_ptr<HostVu> vu;
        {
            const int selection = don_vu1_selection_from_env();
            if (selection != DON_VU1_RUNTIME)
            {
                DonVuApi vu_api;
                std::string vu_error;
                if (!don_vu_load(options.vu_dir, vu_api, vu_error))
                {
                    // vu_error starts with its own [don-vu] prefix.
                    std::fprintf(stderr, "%s\n[don-vu] warning: no VU1 library; the runtime's own VU1 interpreter runs "
                                         "instead (it gives up on the menus; DON_VU1=runtime silences this)\n",
                                 vu_error.c_str());
                }
                else
                {
                    vu = std::make_unique<HostVu>(vu_api, don_vu1_engine_for_selection(selection));
                    runtime->setHostVu1(&vu->iface());
                    if (log)
                        std::fprintf(stderr, "[don] VU1 library: %s (%s)\n", vu_api.path.string().c_str(),
                                     don_vu1_selection_name(selection));
                }
            }
        }

        if (game.needs_elf)
            don_host::warm_game_folder(options.elf_path);
        if (game.needs_elf && !runtime->loadELF(options.elf_path))
        {
            std::fprintf(stderr, "[don] cannot load the ELF %s\n", options.elf_path.c_str());
            return kExitRuntime;
        }
        if (game.setup && !game.setup(*runtime, options, game.user))
        {
            std::fprintf(stderr, "[don] the game setup failed\n");
            return kExitRuntime;
        }

        // The memory cards: mc0 and mc1 inside this folder instead of next to the ELF (the runtime derives mc1
        // from the mc0 path). loadELF resets the card path, so this comes after it. T-524: the default is the
        // one user folder (memcards/), never <exe folder>/saves or the working folder; --mc-dir / DON_MC_DIR
        // override, and net mode keeps its empty temporary card (decision D14).
        std::string mc_dir = options.mc_dir;
        if (net_mode)
        {
            // T-509: a fresh empty memory card folder for the session, whatever --mc-dir says (removed at
            // exit). T-517 review: a folder of a run that was killed is removed first, so the session never
            // inherits somebody else's card (the name is <temp>/don-net-<pid>, and a recycled pid would
            // otherwise find the old files).
            net_mc_dir = (fs::temp_directory_path() / ("don-net-" + std::to_string(host_process_id()))).string();
            std::error_code net_ec;
            fs::remove_all(net_mc_dir, net_ec);
            fs::create_directories(net_mc_dir, net_ec);
            mc_dir = net_mc_dir;
        }
        if (mc_dir.empty())
            if (const char *env = std::getenv("DON_MC_DIR"))
                mc_dir = env;
        if (mc_dir.empty())
            mc_dir = (user_dir / "memcards").string();
        {
            std::error_code ec;
            fs::create_directories(mc_dir, ec);
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.mcRoot = fs::path(mc_dir) / "mc0";
            PS2Runtime::setIoPaths(paths);
            if (log)
                std::fprintf(stderr, "[don] memory cards in %s\n", mc_dir.c_str());
        }

        // T-303 / B4b: the video is the clock (the EE scheduler paces the game to real time) and the sound follows it
        // by rate control. At each vertical blank the queued sound (ring plus SDL stream) is smoothed and the stream's
        // playback ratio is set from its distance to the target (audio_rate.h): a little faster above, a little
        // slower below, within 0.5 %. The game thread never waits. A queue above the ceiling (the game ran far
        // ahead) is cut in whole frames. DON_AUDIO_PACING=0: no control; DON_AUDIO_PACING=wait: the old blocking
        // wait (sleeps 1 ms at a time while more than the target is queued), for comparison.
        const char *pacing_env = std::getenv("DON_AUDIO_PACING");
        const std::string pacing_mode = pacing_env ? pacing_env : "rate";
        if (audio && audio->device_open() && pacing_mode == "wait")
        {
            const uint32_t target = audio_target_ms * kAudioSampleRate / 1000;
            HostAudio *audio_ptr = audio.get();
            gs.set_vblank_hook([audio_ptr, target, stalls = 0, off = false]() mutable {
                if (off || audio_ptr->queued_frames() <= target)
                {
                    stalls = 0;
                    return;
                }
                const auto begin = Clock::now();
                while (audio_ptr->queued_frames() > target)
                {
                    if (Clock::now() - begin > std::chrono::milliseconds(200))
                    {
                        if (++stalls >= 8)
                        {
                            off = true;
                            std::fprintf(stderr, "[don-spu2] the audio device does not drain: the game is no longer paced by it\n");
                        }
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                stalls = 0;
            });
        }
        else if (audio && audio->device_open() && pacing_mode != "0")
        {
            const uint32_t target = audio_target_ms * kAudioSampleRate / 1000;
            const uint32_t ceiling = 400u * kAudioSampleRate / 1000;
            HostAudio *audio_ptr = audio.get();
            gs.set_vblank_hook([audio_ptr, target, ceiling, smoothed = double(audio_target_ms * kAudioSampleRate / 1000)]() mutable {
                const uint32_t queued = audio_ptr->queued_frames();
                if (const uint32_t cut = audio_overflow_drop(queued, ceiling, target); cut != 0)
                {
                    const uint32_t dropped = audio_ptr->drop_oldest(cut);
                    smoothed = audio_level_smooth(smoothed, double(queued - dropped), 1.0);
                }
                else
                    smoothed = audio_level_smooth(smoothed, double(queued));
                audio_ptr->set_rate(audio_rate_ratio(smoothed, double(target)));
            });
        }

        // 4. Run. The game thread opens the GS in thread_begin; this thread polls events and the state.
        if (!runtime->startGameThread())
        {
            std::fprintf(stderr, "[don] the game thread could not be started\n");
            return kExitRuntime;
        }

        int code = kExitOk;
        bool timer_started = false;
        Clock::time_point timer_start;
        const char *end_reason = "the game ended";
        // T-520: perf line and window-state events, always. Cheap counters, no allocation.
        double last_perf_t = 0.0;
        uint64_t last_cpu_us = 0;
        uint64_t last_dropped = 0;
        uint64_t last_underruns = 0;
        uint64_t last_10s_vblanks = 0;
        perfwin::WorstWindow worst_10s;
        perfwin::Window perf_win; // T-757: the last-window fields of the perf line
        double perf_win_t = 0.0;  // the windows' seconds added up (the GS opening restarts the baseline)
        uint64_t last_win_flags = 0;
        bool have_win_flags = false;
        int last_mode_w = 0, last_mode_h = 0;
        float last_refresh = 0.0f;
        bool have_display_mode = false;
        for (;;)
        {
            // T-303: everything the SPU2 produced since the last iteration. This is the one SPU2 call the
            // library allows from a thread other than the one that opened it. In headless mode the loop is
            // the sleep below, so the pull still happens every 8 ms (the library holds about 5 s of output).
            if (spu2 && audio && spu2->is_open())
            {
                float frames[1024 * kAudioChannels];
                uint32_t got = 0;
                uint32_t pulled_now = 0;
                while ((got = spu2->pull(frames, 1024)) > 0)
                {
                    pulled_now += got;
                    lat_produced += got;
                    const auto write_start = gs.pacing_log() ? Clock::now() : Clock::time_point();
                    audio->write(frames, got);
                    if (gs.pacing_log())
                        audio_write_worst_ms = std::max(
                            audio_write_worst_ms, std::chrono::duration<double, std::milli>(Clock::now() - write_start).count());
                }
            }

            if (lat_log && audio)
            {
                const auto now = Clock::now();
                if (lat_last == Clock::time_point())
                    lat_last = now, lat_t0 = now;
                const double dt = std::chrono::duration<double>(now - lat_last).count();
                if (dt >= 5.0)
                {
                    const uint64_t blanks = gs.vblanks();
                    const double ring_ms = audio->ring_queued() * 1000.0 / kAudioSampleRate;
                    const double stream_ms = audio->stream_queued() * 1000.0 / kAudioSampleRate;
                    std::fprintf(stderr,
                                 "[don-spu2] latency t=%.0f s: queued %.0f ms (ring %.0f + stream %.0f), "
                                 "produced %.0f frames/s, blanks %.3f/s, dropped %llu\n",
                                 std::chrono::duration<double>(now - lat_t0).count(), ring_ms + stream_ms, ring_ms,
                                 stream_ms, double(lat_produced - lat_prev_produced) / dt,
                                 double(blanks - lat_prev_blanks) / dt,
                                 static_cast<unsigned long long>(audio->frames_dropped()));
                    lat_last = now;
                    lat_prev_produced = lat_produced;
                    lat_prev_blanks = blanks;
                }
            }

            bool quit = false;
            if (options.headless)
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            else
            {
                quit = pump_events(window, gs, sdl_input.get());
                publish_devices(sdl_input.get(), device_pads, layout);
                if (game_rumble && sdl_input)
                    drive_rumble(*sdl_input, pads, layout);

                // T-520: window-state events (focus, minimize, fullscreen) and display-mode changes,
                // one line each with the time since start. Always, cheap flag comparisons.
                const double t_ev = std::chrono::duration<double>(Clock::now() - run_start).count();
                const uint64_t flags = SDL_GetWindowFlags(window.handle());
                if (!have_win_flags)
                {
                    have_win_flags = true;
                }
                else
                {
                    auto report = [&](uint64_t bit, const char *lost, const char *gained) {
                        const bool was = (last_win_flags & bit) != 0;
                        const bool is = (flags & bit) != 0;
                        if (was != is)
                            std::fprintf(stderr, "[don] %s (t=%.1fs)\n", is ? gained : lost, t_ev);
                    };
                    report(SDL_WINDOW_INPUT_FOCUS, "focus lost", "focus gained");
                    report(SDL_WINDOW_MINIMIZED, "minimized", "restored");
                    report(SDL_WINDOW_FULLSCREEN, "windowed", "fullscreen");
                }
                last_win_flags = flags;

                int mode_w = 0, mode_h = 0;
                float refresh = 0.0f;
                if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window.handle()))
                    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display))
                    {
                        mode_w = mode->w;
                        mode_h = mode->h;
                        refresh = mode->refresh_rate;
                    }
                if (have_display_mode && (mode_w != last_mode_w || mode_h != last_mode_h || refresh != last_refresh))
                    std::fprintf(stderr, "[don] display mode changed to %dx%d @ %.2f Hz (t=%.1fs)\n", mode_w, mode_h,
                                 refresh, t_ev);
                have_display_mode = true;
                last_mode_w = mode_w;
                last_mode_h = mode_h;
                last_refresh = refresh;
            }

            if (quit)
            {
                end_reason = "window closed or Escape";
                if (log)
                    std::fprintf(stderr, "[don] window closed or Escape: leaving\n");
                break;
            }
            if (g_signal.load() != 0)
            {
                end_reason = "signal";
                std::fprintf(stderr, "[don] signal %d: leaving\n", g_signal.load());
                break;
            }

            const HostGs::State state = gs.state();
            if (state == HostGs::Failed)
            {
                end_reason = "cannot open the GS";
                std::fprintf(stderr, "[don] cannot open the GS: %s\n", gs.error().c_str());
                code = kExitGs;
                break;
            }
            if (state == HostGs::Open && !timer_started)
            {
                timer_started = true; // --seconds counts from the moment the GS is open
                timer_start = Clock::now();
                // T-520: the renderer and the Vulkan devices the GS picked, plus vsync and upscale. The
                // device block is filled on the game thread right after the GS opened (gs.device_info()).
                std::fprintf(stderr, "[don] vsync %s, upscale %d\n", gs_params.vsync ? "on" : "off",
                             gs_params.upscale);
                for (const std::string &line : split_lines(gs.device_info()))
                    std::fprintf(stderr, "[don] gs %s\n", line.c_str());
                // The perf baseline starts when the GS opens.
                last_perf_t = std::chrono::duration<double>(Clock::now() - run_start).count();
                uint64_t cpu_base = 0, rss_base = 0;
                don_sysinfo::process_usage(cpu_base, rss_base);
                last_cpu_us = cpu_base;
            }
            if (!runtime->isGameThreadRunning())
            {
                end_reason = runtime->gameThreadFailed() ? "the runtime ended with an error" : "the game ended";
                code = runtime->gameThreadFailed() ? kExitRuntime : kExitOk;
                break;
            }
            if (options.seconds > 0.0 && timer_started &&
                std::chrono::duration<double>(Clock::now() - timer_start).count() >= options.seconds)
            {
                end_reason = "--seconds elapsed";
                break;
            }

            // T-520: the perf line every 10 s, always.
            {
                const double t_now = std::chrono::duration<double>(Clock::now() - run_start).count();
                if (t_now - last_perf_t >= 10.0)
                {
                    const double window = t_now - last_perf_t;
                    last_perf_t = t_now;
                    const uint64_t vblanks = gs.vblanks();
                    const double fps = t_now > 0.0 ? static_cast<double>(vblanks) / t_now : 0.0;
                    const uint64_t min_fps_raw = gs.perf_min_fps();
                    uint64_t min_fps = min_fps_raw == ~0ull ? 0u : min_fps_raw;
                    if (static_cast<double>(min_fps) > fps) // never above the average fps
                        min_fps = static_cast<uint64_t>(fps);
                    const uint64_t slow = gs.perf_slow_frames();
                    const uint64_t max_us = gs.perf_max_frame_us();
                    const uint64_t win_max_us = gs.take_perf_max_frame_us();
                    const double avg_ms = vblanks ? t_now * 1000.0 / static_cast<double>(vblanks) : 0.0;
                    const uint64_t wait_ms = gs.take_audio_wait_max_us() / 1000u;
                    double audio_ms = 0.0;
                    uint64_t underruns = 0, overruns = 0;
                    if (audio)
                    {
                        audio_ms = audio->queued_frames() * 1000.0 / kAudioSampleRate;
                        const uint64_t dropped = audio->frames_dropped();
                        overruns = dropped - last_dropped;
                        last_dropped = dropped;
                        const uint64_t under = audio->underruns();
                        underruns = under - last_underruns;
                        last_underruns = under;
                    }
                    uint64_t cpu_us = 0, rss = 0;
                    don_sysinfo::process_usage(cpu_us, rss);
                    const double cpu_pct = window > 0.0 ? 100.0 * static_cast<double>(cpu_us - last_cpu_us) /
                                                              (window * 1.0e6)
                                                        : 0.0;
                    last_cpu_us = cpu_us;
                    const uint64_t frames_window = vblanks - last_10s_vblanks;
                    worst_10s.add(last_10s_vblanks, frames_window);
                    last_10s_vblanks = vblanks;
                    perf_win_t += window;
                    const perfwin::Result lw = perf_win.advance({perf_win_t, vblanks, slow, win_max_us});
                    const char *win_state =
                        (last_win_flags & SDL_WINDOW_MINIMIZED) ? "minimized" : ((last_win_flags & SDL_WINDOW_INPUT_FOCUS) ? "focus" : "unfocused");
                    std::fprintf(stderr,
                                 "[don] perf t=%.0fs fps=%.1f game_fps=%.1f min_fps=%llu frame_ms avg=%.2f max=%.2f "
                                 "slow=%llu audio_ms=%.0f rate=%.4f underruns=%llu overruns=%llu wait_ms=%llu cpu=%.0f%% rss=%lluMB win=%s "
                                 "win_frames=%llu win_fps=%.1f win_frame_ms avg=%.2f max=%.2f win_slow=%llu\n",
                                 t_now, fps, fps, static_cast<unsigned long long>(min_fps), avg_ms,
                                 max_us / 1000.0, static_cast<unsigned long long>(slow), audio_ms, audio ? audio->rate() : 1.0,
                                 static_cast<unsigned long long>(underruns), static_cast<unsigned long long>(overruns),
                                 static_cast<unsigned long long>(wait_ms),
                                 cpu_pct, static_cast<unsigned long long>(rss / (1024u * 1024u)), win_state,
                                 // T-757: the same counters over this window only; the window max is exact (taken and reset).
                                 static_cast<unsigned long long>(lw.frames), lw.fps, lw.avg_ms,
                                 lw.max_us / 1000.0, static_cast<unsigned long long>(lw.slow));
                }
            }
        }

        // 5. Shutdown: stop the game thread (thread_end closes the GS on that thread), then the rest.
        runtime->stopGameThread();
        // T-203: the game thread has ended and thread_end() disarmed the sampler; write its file
        // here, before main.cpp's std::_Exit(0). No cost when DON_PROFILE is unset.
        profile_finish();
        gs.print_pacing_summary(audio_write_worst_ms);
        if (code == kExitOk && runtime->gameThreadFailed())
            code = kExitRuntime;
        if (code == kExitRuntime)
            std::fprintf(stderr, "[don] the runtime ended with an error\n");

        // DON_DUMP_RAM=<file>: once the game thread has stopped, write the 32 MiB of guest RAM, so that a run
        // can be compared word by word with a console memory image (tools/verify/ram_diff.py). Off by default,
        // one line says what was written. The dump is game memory: it belongs under work/ and never in git
        // (AGENTS.md, rule 1).
        if (const char *dump_path = std::getenv("DON_DUMP_RAM"); dump_path != nullptr && dump_path[0] != '\0')
        {
            const uint8_t *const ram = runtime->memory().getRDRAM();
            std::FILE *const file = std::fopen(dump_path, "wb");
            if (file == nullptr)
            {
                std::fprintf(stderr, "[don-ram] cannot write the RAM dump %s\n", dump_path);
            }
            else
            {
                const std::size_t written = std::fwrite(ram, 1, PS2_RAM_SIZE, file);
                std::fclose(file);
                std::fprintf(stderr, "[don-ram] RAM dump %s: %zu of %u bytes%s\n", dump_path, written,
                             PS2_RAM_SIZE, written == PS2_RAM_SIZE ? "" : " (short write)");
            }
        }
        gs.print_summary();
        // T-207 patch 0034: how many dispatch decisions the game thread took and how many of them needed
        // the slow path. Skipped when the runtime was built without the patch (no such counters).
#if defined(PS2X_EE_SCHEDULER_STATS)
        if (log)
        {
            const EeDispatchStats dispatch = runtime->eeScheduler().dispatchStats();
            std::fprintf(stderr,
                         "[don] dispatches %llu: fast path %llu, slow path %llu (%.4f%% of the dispatches)\n",
                         static_cast<unsigned long long>(dispatch.dispatches),
                         static_cast<unsigned long long>(dispatch.fastPath),
                         static_cast<unsigned long long>(dispatch.slowPath),
                         dispatch.dispatches != 0u ? 100.0 * static_cast<double>(dispatch.slowPath) /
                                                         static_cast<double>(dispatch.dispatches)
                                                   : 0.0);
        }
#endif
        runtime->setHostGs(nullptr);
        // T-303: the table was removed and the library closed on the game thread (thread_end); both calls
        // are no-ops if that already happened. The audio output is flushed here, before main.cpp's exit.
        if (spu2)
        {
            ps2x::iop::iopSetHostSpu2(nullptr);
            spu2->close();
        }
        if (audio)
        {
            audio->shutdown();
            audio->print_summary();
        }
        // T-205: the table is removed while the game thread is not running; the summary was printed at
        // thread_end, so print_summary() here prints nothing more.
        if (vu)
        {
            runtime->setHostVu1(nullptr);
            vu->print_summary();
            vu.reset();
        }
        // T-517: tell the player why the session ended (a player gone for 30 s, a desync, the host quitting) and
        // exit 4 (section 7.2). An empty reason is a plain stop (a signal, the window closing) and keeps the
        // exit code as it was.
        if (netplay)
        {
            const int players = net_session.players > 0 ? net_session.players : 1;
            const std::string reason =
                don_host::StartScreenModel::net_end_reason(net_end_error, net_player_names(*netplay, players));
            if (!reason.empty())
            {
                don_host::don_show_net_session_end(reason);
                code = kExitSessionEnd;
            }
        }
        // T-520: the end-of-run line, always. log_redirect.cpp looks for the "[don] exit:" prefix
        // to tell a run that ended cleanly from one that crashed or was killed.
        {
            const double total = std::chrono::duration<double>(Clock::now() - run_start).count();
            const uint64_t frames = gs.vblanks();
            don_log_note_exit_written();
            std::fprintf(stderr, "[don] exit: %s, code %d (%.1fs, %llu frames",
                         end_reason, code, total, static_cast<unsigned long long>(frames));
            if (worst_10s.has())
                std::fprintf(stderr, ", worst 10s window %.1f fps",
                             static_cast<double>(worst_10s.worst()) / 10.0);
            std::fprintf(stderr, ")\n");
        }
        runtime.reset();
        don_sysinfo::shutdown();
        // The UPnP mapping, the temporary card folder and the BYE to the peers are NetSessionCleanup's (T-517):
        // it runs on this path, on every early return above and on an exception alike.
        return code;
    }
} // namespace

namespace
{
// The run itself; don_host_run (below) wraps it with the log.
int host_run_body(int argc, char **argv, const DonHostGame &game)
{
    // The crash reporter right after the log, so a death leaves its block in it.
    don_crash_report_install();
#ifdef _WIN32
    {
        std::int64_t previous_start = 0;
        if (don_previous_start_from_log(&previous_start))
            don_driver_events_report(previous_start);
    }
#endif

    // T-520: the full command line, always, before anything else of the start block (a report must not
    // have to ask how the program was started).
    {
        std::string cmd;
        for (int i = 0; i < argc; ++i)
        {
            if (i != 0)
                cmd += ' ';
            const std::string arg = argv[i];
            const bool needs_quotes = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
            if (!needs_quotes)
                cmd += arg;
            else
            {
                cmd += '"';
                for (const char c : arg)
                {
                    if (c == '"' || c == '\\')
                        cmd += '\\';
                    cmd += c;
                }
                cmd += '"';
            }
        }
        std::fprintf(stderr, "[don] cmd %s\n", cmd.c_str());
    }

    DonHostOptions options;
    switch (don_parse_options(argc, argv, game.needs_elf, options))
    {
    case DonParseResult::Help:
        return kExitOk;
    case DonParseResult::Error:
        return kExitArguments;
    case DonParseResult::Ok:
        break;
    }
    if (game.needs_elf)
    {
        std::error_code ec;
        if (!fs::is_regular_file(options.elf_path, ec))
        {
            std::fprintf(stderr, "[don] the ELF %s does not exist\n", options.elf_path.c_str());
            return kExitArguments;
        }
    }
    if (!options.iso_path.empty())
    {
        std::error_code ec;
        if (!fs::is_regular_file(options.iso_path, ec))
        {
            std::fprintf(stderr, "[don] the disc image %s does not exist\n", options.iso_path.c_str());
            return kExitArguments;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try
    {
        return run(options, game);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[don] fatal: %s\n", e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "[don] fatal: unknown exception\n");
    }
    don_sysinfo::shutdown();
    return kExitRuntime;
}
} // namespace

int don_host_run(int argc, char **argv, const DonHostGame &game)
{
    // T-524: first, before anything prints. stdout/stderr go to <user>/don.log (appended under a dated
    // header) AND are tee'd to the original streams; see log_redirect.h.
    don_log_redirect();
    const int code = host_run_body(argc, argv, game);
    // Every way out has an exit line (the next start reads a missing one as a crash): the end of a game
    // writes its own with the reason and a summary; an early stop (a bad argument, the GS not opening) gets
    // this one. Then the tee ends and what it still holds is written before main.cpp's _Exit.
    if (!don_log_exit_written())
        std::fprintf(stderr, "[don] exit: stopped early, code %d\n", code);
    don_log_finish();
    return code;
}
