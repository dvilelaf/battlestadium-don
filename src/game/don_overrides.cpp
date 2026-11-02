// Behavior of specific functions of Battle Stadium D.O.N (SLPS_256.75) and per-game runtime setup.
// Registered with the PS2Recomp runtime, which calls applyDon() from loadELF() when the entry
// point and the CRC-32 of the ELF match.

#include "game_overrides.h"
#include "video_skip.h"
#include "net_hash.h"
#include "pad_layout_flag.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

// Defined in don_overrides_calls.cpp.
namespace don_diag
{
    void dumpThreads(PS2Runtime &runtime, uint64_t tick);
    void dumpIop(PS2Runtime &runtime, uint64_t tick);
    void dumpState(PS2Runtime &runtime, uint64_t tick);
    uint64_t hashGuestRam(const uint8_t *ram);
}

namespace
{
    // Functions of the game bound by address to a runtime handler of the same public SDK
    // function (the analyzer did not recognise them). The arguments the callers pass were
    // checked against what the handler reads (journal entry in the last column).
    //   address     SDK function      handler           journal
    //   0x0011A7A8  sceCdSearchFile   sceCdSearchFile   002
    struct HandlerBinding
    {
        uint32_t address;
        const char *handler;
    };
    constexpr HandlerBinding kHandlerBindings[] = {
        {0x0011A7A8u, "sceCdSearchFile"},
        // The game's sceSifFreeIopHeap goes through an RPC to a heap server that nobody serves, so every free was
        // lost: the sound-bank loader's 0x8000 blocks piled up until the heap refused one (journal 037).
        {0x001146C8u, "sceSifFreeIopHeap"},
    };

    // The game's own functions this file replaces with a host implementation (T-112): the four multitap
    // wrappers of docs/knowledge/14-iop-modules.md section E, see the comment on donMtapInit below.
    struct FunctionReplacement
    {
        uint32_t address;
        PS2Runtime::RecompiledFunction function;
        const char *name;
    };

    constexpr uint32_t kWriteAddress = 0x0010C868u;
    constexpr uint32_t kFrameLoopCounterAddress = 0x00138B00u;
    constexpr uint32_t kVblankHandlerAddress = 0x00138EE0u;

    // The recompiled originals, looked up by guest address in replaceKeepingOriginal() before the override
    // replaces them (afterwards the table returns the override itself). Set only when the override is installed.
    PS2Runtime::RecompiledFunction g_originalWrite = nullptr;
    PS2Runtime::RecompiledFunction g_originalFrameCounter = nullptr;
    PS2Runtime::RecompiledFunction g_originalVblank = nullptr;

    // The game's own multitap wrappers (docs/knowledge/14-iop-modules.md section E, **Measured** by
    // decompiling 0x255C40-0x2560C3 and their callers). They are thin SIF RPC wrappers to `mtapman`, which
    // probes the SIO2 hardware at 0x1F808200 - a port the port does not model - so the real code answers
    // nothing: the per-frame pad poll 0x138FB0 got 0 from GetConnection (0x255EA0), and 0 makes it "skip the
    // whole update (no pad input)". With these overrides the poll stays in the normal two-port mode and reads
    // port 0 slot 0 and port 1 slot 0 through scePadRead (the replaced handler, fed by the host's pad table),
    // which is what the input layer publishes by default. `*a` non-zero asks for the four-pad mode: only with
    // `--players 4` (T-306, see donMtapGetConnection). Section E's table has the return value of each of the four.
    void donMtapInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void donMtapPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void donMtapPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    // (port, *a, *b): return 1, *a = 0, *b = 0 (section E). The table mtapman would fill is not read by the
    // caller of this replacement.
    // T-306: with the host's four-player layout (--players 4, src/host/pad_layout_flag.h) port 0 reports a
    // multitap: *a = 1 (the game only tests it for non-zero, section E), and the poll opens port 0 slots 0 to 3.
    void donMtapGetConnection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t port = getRegU32(ctx, 4);
        const uint32_t a = getRegU32(ctx, 5);
        const uint32_t b = getRegU32(ctx, 6);
        const uint32_t multitap = (port == 0u && don_pads::multitap_in_port0().load()) ? 1u : 0u;
        if (a != 0u && getMemPtr(rdram, a) != nullptr)
        {
            Ps2FastWrite32(rdram, a, multitap);
        }
        if (b != 0u && getMemPtr(rdram, b) != nullptr)
        {
            Ps2FastWrite32(rdram, b, 0u);
        }
        setReturnS32(ctx, 1);
    }

    // The game's video player (0x36A820, docs/knowledge/19-videos.md section 1, **Measured**). The script
    // opcode 0x50 (handler 0x36AC70, index 0 = OP, 1 = ED) calls it with a1 = 1 (the opening and the attract
    // loop: its loop ends when Start is pressed) or 2 (the ending: Circle). The port cannot decode the stream,
    // so the player sits in its loop. A first cut replaced the handler 0x36AC70 and never ran the player
    // (journal 023): the title then froze a few seconds after it appeared (journal 024), so the player's own
    // entry and exit do something the title needs. The player runs unchanged now and the host presses the
    // button for it (src/host/video_skip.h). The port knows the player is running from the game's own state:
    // 0x55A860 holds the pool block that the player takes before its loop and zeroes after it (the one
    // writer, see the decompilation of 0x36A820). A wrapper that calls the original could not be used: the
    // runtime's functions return early at a scheduler checkpoint and are resumed at a label by the
    // dispatcher, so a wrapper sees the call return long before the player has finished.
    PS2Runtime *g_probeRuntime = nullptr;
    bool donVideoPlayerRunning()
    {
        const uint8_t *ram = g_probeRuntime != nullptr ? g_probeRuntime->memory().getRDRAM() : nullptr;
        if (ram == nullptr)
        {
            return false;
        }
        uint32_t value = 0;
        std::memcpy(&value, ram + 0x0055A860u, sizeof(value));
        return value != 0;
    }

    bool envFlag(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    }

    // The game's C library `write` (0x10C868): $a0 = descriptor, $a1 = buffer, $a2 = length.
    // For descriptors 1 and 2 the original sends the text to the development kit's debug
    // console, which does not exist here. Those are handled on the host: with DON_TTY=1 the
    // text goes to stderr with every line prefixed "[don-tty] "; in every case the length is
    // returned. Any other descriptor runs the original recompiled function (decision Q-TTYIO).
    void donWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t fd = getRegU32(ctx, 4);
        if (fd != 1u && fd != 2u)
        {
            g_originalWrite(rdram, ctx, runtime);
            return;
        }

        const uint32_t buffer = getRegU32(ctx, 5);
        const uint32_t length = getRegU32(ctx, 6);

        static const bool enabled = envFlag("DON_TTY");
        if (enabled)
        {
            static bool atLineStart = true;
            for (uint32_t i = 0; i < length; ++i)
            {
                if (atLineStart)
                {
                    std::fputs("[don-tty] ", stderr);
                    atLineStart = false;
                }
                const char c = static_cast<char>(Ps2FastRead8(rdram, buffer + i));
                std::fputc(c, stderr);
                if (c == '\n')
                {
                    atLineStart = true;
                }
            }
            std::fflush(stderr);
        }

        setReturnU32(ctx, length);
    }

    // DON_TRACE_FRAMES=1: signal for "the game is in its frame loop" (docs/journal/README.md).
    // `[don] frame <n>` counts iterations of the frame loop 0x131CA0 (each one calls 0x138B00
    // once); `[don] vsync <n>` counts calls of the game's vertical-blank handler 0x138EE0.
    // Both print every 60 calls and then run the original recompiled function.
    void donFrameCounter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint64_t> count{0};
        const uint64_t n = ++count;
        if (n % 60u == 0u)
        {
            std::fprintf(stderr, "[don] frame %llu\n", static_cast<unsigned long long>(n));
        }
        g_originalFrameCounter(rdram, ctx, runtime);
    }

    void donVblankHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<uint64_t> count{0};
        const uint64_t n = ++count;
        if (n % 60u == 0u)
        {
            std::fprintf(stderr, "[don] vsync %llu\n", static_cast<unsigned long long>(n));
        }
        don_diag::dumpThreads(*runtime, n);
        don_diag::dumpIop(*runtime, n);
        don_diag::dumpState(*runtime, n);
        g_originalVblank(rdram, ctx, runtime);
    }

    // config/native_overrides.txt (the list the recompiler reads so that no direct call bypasses a replacement),
    // compiled in by CMakeLists.txt. Every replaceFunction below goes through replaceListed().
    constexpr uint32_t kNativeOverrides[] = {DON_NATIVE_OVERRIDES 0u};
    bool g_overrideMissingFromList = false;

    void checkListed(uint32_t address)
    {
        bool listed = false;
        for (const uint32_t candidate : kNativeOverrides)
        {
            listed = listed || (candidate != 0u && candidate == address);
        }
        if (!listed)
        {
            std::fprintf(stderr, "[don] replaceFunction target 0x%08X is not in config/native_overrides.txt\n", address);
            g_overrideMissingFromList = true;
        }
    }

    bool replaceListed(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction function)
    {
        checkListed(address);
        return runtime.replaceFunction(address, function);
    }

    // Resolves the function that is in the table at `address` into `original`, then replaces it. The lookup is
    // findFunction (it records a dispatch in the diagnostic history only when that is enabled, and this runs once
    // at startup). A table without a function there is a wrong address or config: stop loudly.
    bool replaceKeepingOriginal(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction function,
                                PS2Runtime::RecompiledFunction &original)
    {
        original = runtime.findFunction(address);
        if (original == nullptr)
        {
            std::fprintf(stderr, "[don] no recompiled function at 0x%08X to keep as the original\n", address);
            std::abort();
        }
        return replaceListed(runtime, address, function);
    }

    void applyDon(PS2Runtime &runtime)
    {
        if (const char *iso = std::getenv("DON_ISO"))
        {
            PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
            paths.cdImage = std::filesystem::path(iso);
            PS2Runtime::setIoPaths(paths);
        }

        if (!replaceKeepingOriginal(runtime, kWriteAddress, donWrite, g_originalWrite))
        {
            std::fprintf(stderr, "[don] could not replace the function at 0x%08X\n", kWriteAddress);
        }

        if (envFlag("DON_TRACE_FRAMES") || envFlag("DON_TRACE_THREADS") || envFlag("DON_TRACE_IOP") ||
            std::getenv("DON_TRACE_STATE") != nullptr)
        {
            if (!replaceKeepingOriginal(runtime, kFrameLoopCounterAddress, donFrameCounter, g_originalFrameCounter) ||
                !replaceKeepingOriginal(runtime, kVblankHandlerAddress, donVblankHandler, g_originalVblank))
            {
                std::fprintf(stderr, "[don] could not install the frame counters\n");
            }
        }

        const FunctionReplacement replacements[] = {
            {0x00255CA0u, donMtapInit, "the multitap init"},
            {0x00255DC0u, donMtapPortOpen, "the multitap port open"},
            {0x00255E30u, donMtapPortClose, "the multitap port close"},
            {0x00255EA0u, donMtapGetConnection, "the multitap connection query"},
        };
        for (const FunctionReplacement &replacement : replacements)
        {
            if (!replaceListed(runtime, replacement.address, replacement.function))
            {
                std::fprintf(stderr, "[don] could not replace %s at 0x%08X\n", replacement.name,
                             replacement.address);
            }
        }

        // T-509: the host reads this to send the netplay hashes (docs/knowledge/22-netplay.md section 7).
        don_net::ram_hash() = don_diag::hashGuestRam;

        if (envFlag("DON_PLAY_MOVIES"))
        {
            std::fprintf(stderr, "[don] DON_PLAY_MOVIES=1: the game's own video player runs and nothing presses Start for it\n");
        }
        else
        {
            g_probeRuntime = &runtime;
            don_video::player_probe() = donVideoPlayerRunning;
#if defined(DON_WITH_FFMPEG)
            std::fprintf(stderr, "[don] videos play; press Start to skip one (DON_MOVIE_SKIP_AFTER=<blanks> makes the port skip them)\n");
#else
            std::fprintf(stderr, "[don] videos are skipped: the port presses Start for the game's player (set DON_PLAY_MOVIES=1 to play them)\n");
#endif
        }

        for (const HandlerBinding &binding : kHandlerBindings)
        {
            checkListed(binding.address);
            if (!ps2_game_overrides::bindAddressHandler(runtime, binding.address, binding.handler))
            {
                std::fprintf(stderr, "[don] could not bind 0x%08X to %s\n", binding.address, binding.handler);
            }
        }

        std::fprintf(stderr, "[don] override applied\n");
#if defined(DON_DEV_BUILD)
        if (g_overrideMissingFromList)
        {
            std::fprintf(stderr, "[don] a replaced function is missing from config/native_overrides.txt: exiting (dev build)\n");
            std::exit(2);
        }
#endif
    }
}

PS2_REGISTER_GAME_OVERRIDE("Battle Stadium D.O.N", "SLPS_256.75", 0x00100008u, 0x7E09D26Du, applyDon);
