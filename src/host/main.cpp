// SPDX-License-Identifier: GPL-3.0-or-later
// The real executable's main: the game's setup is "load the ELF given on the command line"; the game's
// overrides (src/game/don_overrides.cpp) register themselves and the runtime applies them in loadELF.
#include "host.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "cpu_check.h"
#include "crash_trace.h"
#include "game_module.h"
#include "options.h"
#include "prepare_cli.h"
#include "../setup/storage.h"
#include "../setup/user_paths.h"
#include "setup_demo.h"
#include "setup_run.h"

#ifdef DON_NO_GENERATED
// Runs after loadELF and before the first guest instruction (host.cpp): the module must be bound by then.
bool checkModuleBound(PS2Runtime &, const DonHostOptions &, void *)
{
    if (don_game_module_bound())
        return true;
    std::fprintf(stderr, "[don] the game module is not bound to the runtime: its bind did not run in loadELF\n");
    std::fflush(stderr);
    return false;
}

// The crash marker of a module folder (T-616): written here, just before the first guest instruction.
bool beginModuleRun(PS2Runtime &runtime, const DonHostOptions &options, void *user)
{
    if (!checkModuleBound(runtime, options, user))
        return false;
    return don_game_module_begin_run();
}
#endif

int main(int argc, char **argv)
{
    // T-518: first, before any code of this x86-64-v3 translation unit can run: a processor
    // without AVX2 gets the message of knowledge 22 section 7.2 and exit code 5 instead of an
    // illegal instruction inside the game's own code.
    const int cpu = don_cpu_check();
    if (cpu != 0)
        return cpu;
    // T-705: a fatal signal or an uncaught exception leaves a line, from here on (crash_trace.h).
    don_host::crash_trace_install();
    // The runtime logs through std::cout, which is block-buffered when stdout is not a terminal:
    // redirected to a file, its last lines were lost when a run was stopped by a time limit.
    // Line-buffered from the first moment, whatever the destination.
#ifdef _WIN32
    // The Windows C runtime has no line buffering (_IOLBF is taken as full buffering) and rejects a buffer
    // size under 2 for it with an invalid-parameter fast-fail (0xc0000417) before anything is printed.
    // Unbuffered gives the same guarantee: nothing is lost when a run is stopped.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif

    // --setup-demo: the setup screen against a fake engine, before anything game-related starts (no ELF needed).
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--setup-demo") != 0)
            continue;
        DonHostOptions demo_options;
        if (don_parse_options(argc, argv, false, demo_options) != DonParseResult::Ok)
            return 2;
        const int demo_code = don_setup_demo(demo_options.screenshot_dir);
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(demo_code);
    }

    // Arguments the game run sees: argv, plus the ELF of the installed variant when auto-load supplies it.
    std::vector<char *> run_args(argv, argv + argc);
    std::string installed_elf;

    DonHostOptions early_options;
    switch (don_parse_options(argc, argv, false, early_options))
    {
    case DonParseResult::Help:
        return 0;
    case DonParseResult::Error:
        return 2;
    case DonParseResult::Ok:
        break;
    }
    // --prepare <image>: the player's disc image becomes a prepared game module (T-692), then the program ends.
    if (!early_options.prepare_iso.empty())
    {
        don_host::crash_trace_set_phase(don_host::kPhasePrepare);
        don_host::crash_trace_diag("prepare");
        const int prepare_code = don_host::run_prepare(
            early_options.prepare_iso, don_host::install_layout(don_host::program_folder_of_executable()));
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(prepare_code);
    }

    bool setup_ended_with_play = false; // T-720: PLAY on the first-run screens starts the game with no start menu
#ifdef DON_NO_GENERATED
    // No game code is linked: the game is a module, loaded before the runtime exists (its constructor reads the
    // module's function table). --game-module names one; without it the installed, verified module of the installed
    // variant is the game, and its game/<variant>/ folder the game data.
    {
        std::string module_path = early_options.game_module;
        if (module_path.empty())
        {
            don_setup::ErrorId roots_error = don_setup::ErrorId::None;
            const don_setup::UserRoots roots = don_setup::user_roots(&roots_error);
            auto find = [&] {
                return roots_error == don_setup::ErrorId::None
                           ? don_setup::find_installed_module(roots.data, kDonGameModuleAbiVersion)
                           : don_setup::InstalledModule{};
            };
            don_setup::InstalledModule installed = find();
            if (!installed.found)
            {
                // Nothing prepared (or prepared by another version of the program): the first-run screens prepare the
                // game (T-649), and PLAY comes back here to start it in this same process.
                if (installed.abi_mismatch)
                    std::fprintf(stderr, "[don] the prepared game was built for another version of this program\n");
                // T-716: a headless run has no player to answer the setup screens, so it fails here instead of waiting.
                // T-720: unless DON_SETUP_SCRIPT is set: the script drives the screens and is that run's "player"
                // (tools/run/setup_check.py full-prepare-and-play).
                if (early_options.headless && std::getenv("DON_SETUP_SCRIPT") == nullptr)
                {
                    std::fprintf(stderr, "[don] the game is not prepared: run  don --prepare <your disc image>\n");
                    return 2;
                }
                don_host::crash_trace_set_phase(don_host::kPhaseSetup);
                don_host::crash_trace_diag("setup");
                if (don_host::setup_run(don_host::install_layout(don_host::program_folder_of_executable())) !=
                    don_host::SetupEnd::Play)
                    return 0;
                setup_ended_with_play = true;
                installed = find();
            }
            if (!installed.found)
            {
                std::fprintf(stderr, "[don] the game is not prepared: run  don --prepare <your disc image>\n");
                return 2;
            }
            module_path = installed.module_dir.string();
            std::fprintf(stderr, "[don] using the prepared module %s (variant %s)\n", installed.module_id.c_str(),
                         installed.variant.c_str());
            if (early_options.elf_path.empty())
            {
                std::error_code ec;
                for (const auto &entry : std::filesystem::directory_iterator(installed.game_dir, ec))
                {
                    std::string name = entry.path().filename().string();
                    for (char &c : name)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (name == "slps_256.75")
                        installed_elf = entry.path().string();
                }
                if (installed_elf.empty())
                {
                    std::fprintf(stderr, "[don] %s holds no SLPS_256.75: run  don --prepare <your disc image>  again\n",
                                 installed.game_dir.string().c_str());
                    return 2;
                }
                run_args.push_back(installed_elf.data());
            }
        }
        std::string module_error;
        if (!don_load_game_module(module_path, module_error))
        {
            std::fprintf(stderr, "[don] %s\n", module_error.c_str());
            return 2;
        }
    }
#endif

    DonHostGame game;
    game.title = "Battle Stadium D.O.N";
    game.needs_elf = true;
    game.skip_start_screen = setup_ended_with_play;
#ifdef DON_NO_GENERATED
    // The module's bind runs from loadELF; if it did not, the guest would crash later with no message (T-610).
    game.setup = beginModuleRun;
#endif
    run_args.push_back(nullptr);
    don_host::crash_trace_set_phase(don_host::kPhaseGame);
    don_host::crash_trace_diag("game");
    const int code = don_host_run(static_cast<int>(run_args.size()) - 1, run_args.data(), game);
#ifdef DON_NO_GENERATED
    don_game_module_end_run();
#endif
    std::fflush(stdout);
    std::fflush(stderr);
    // As the runtime's own main does: the runtime and the GS library keep threads and static state whose
    // destructors have nothing left to do once everything has been stopped.
    std::_Exit(code);
}
