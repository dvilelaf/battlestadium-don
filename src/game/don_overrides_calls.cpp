// Diagnostics of Battle Stadium D.O.N: thread, IOP and state dumps, and the guest RAM hash (moved out of
// don_overrides.cpp, T-608). The dump functions are enabled from there by DON_TRACE_* variables.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace don_diag
{
    static bool envFlag(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    }

    // Temporary diagnostic for the boot loop (T-016): the state of every guest thread and semaphore.
    // DON_TRACE_THREADS=1 prints a table every 60 vertical blanks. Remove when the boot loop is over.
    static const char *threadStatusName(EeThreadStatus status)
    {
        switch (status)
        {
        case EeThreadStatus::Running: return "run";
        case EeThreadStatus::Ready: return "ready";
        case EeThreadStatus::Waiting: return "wait";
        case EeThreadStatus::WaitingSuspended: return "waitsusp";
        case EeThreadStatus::Suspended: return "susp";
        case EeThreadStatus::Dormant: return "dormant";
        }
        return "?";
    }

    void dumpThreads(PS2Runtime &runtime, uint64_t tick)
    {
        static const bool enabled = envFlag("DON_TRACE_THREADS");
        if (!enabled || tick % 60u != 0u)
        {
            return;
        }
        const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
        std::fprintf(stderr, "[don-threads] vsync %llu running=%d threads=%zu\n",
                     static_cast<unsigned long long>(tick), snap.runningThreadId, snap.threads.size());
        for (const EeThreadSnapshot &t : snap.threads)
        {
            std::fprintf(stderr,
                         "[don-threads]   t%d %s prio %d/%d pc=0x%08X ra=0x%08X entry=0x%08X "
                         "wait=%d/%d susp=%d wake=%u stack=0x%08X+0x%X gp=0x%08X\n",
                         t.id, threadStatusName(t.status), t.initialPriority, t.currentPriority,
                         t.pc, t.ra, t.entry, static_cast<int>(t.waitReason), t.waitId,
                         t.suspendCount, t.wakeupCount, t.stack, t.stackSize, t.gp);
        }
        for (const EeSemaphoreSnapshot &s : snap.semaphores)
        {
            std::fprintf(stderr, "[don-threads]   sema %d count=%d max=%d waiters=%u\n",
                         s.id, s.count, s.maxCount, s.waiters);
        }
    }

    // Temporary diagnostic for the IOP's load at the memory-card dialog (T-111, part 1): how much the IOP
    // emulator has executed and which of its services are alive. DON_TRACE_IOP=1 prints one [don-iop] line
    // every 60 vertical blanks, with the wall-clock interval and the rate so that the instructions per
    // second are real numbers and not per-vblank ones.
    //
    // The runtime's snapshot has no per-thread state (only counts), so the thread that keeps the IOP busy
    // is identified by sampling the stopped IOP with gdb (see the report of T-111); this line is what gives
    // its rate.
    void dumpIop(PS2Runtime &runtime, uint64_t tick)
    {
        static const bool enabled = envFlag("DON_TRACE_IOP");
        if (!enabled || tick % 60u != 0u)
        {
            return;
        }
        using Clock = std::chrono::steady_clock;
        static uint64_t lastCycles = 0;
        static uint64_t lastInstructions = 0;
        static Clock::time_point lastTime = Clock::time_point();
        const ps2x::iop::DebugSnapshot snap = runtime.iopDebugSnapshot();
        const auto now = Clock::now();
        const double seconds = lastTime == Clock::time_point()
                                   ? 0.0
                                   : std::chrono::duration<double>(now - lastTime).count();
        const uint64_t dCycles = snap.emulatorCycles - lastCycles;
        const uint64_t dInstr = snap.emulatorInstructions - lastInstructions;
        std::fprintf(stderr,
                     "[don-iop] vsync %llu cycles=%llu (+%llu) instructions=%llu (+%llu) dt=%.2fs "
                     "instrPerSecond=%llu modules=%u threads=%u rpcServers=%u\n",
                     static_cast<unsigned long long>(tick), static_cast<unsigned long long>(snap.emulatorCycles),
                     static_cast<unsigned long long>(dCycles),
                     static_cast<unsigned long long>(snap.emulatorInstructions),
                     static_cast<unsigned long long>(dInstr), seconds,
                     seconds > 0.0 ? static_cast<unsigned long long>(dInstr / seconds) : 0ull,
                     snap.emulatorLoadedModules, snap.emulatorThreads, snap.emulatorRpcServers);
        for (const ps2x::iop::DebugService &service : snap.services)
        {
            std::string metrics;
            for (const ps2x::iop::DebugMetric &metric : service.metrics)
            {
                if (metric.value == 0u)
                {
                    continue;
                }
                char text[96];
                std::snprintf(text, sizeof(text), " %s=%s", metric.name.c_str(),
                              metric.hexadecimal ? "0x" : "");
                metrics += text;
                if (metric.hexadecimal)
                {
                    char number[32];
                    std::snprintf(number, sizeof(number), "%llX", static_cast<unsigned long long>(metric.value));
                    metrics += number;
                }
                else
                {
                    metrics += std::to_string(metric.value);
                }
            }
            std::fprintf(stderr, "[don-iop]   service %-12s active=%d sids=%zu%s\n", service.name.c_str(),
                         service.active ? 1 : 0, service.sids.size(), metrics.c_str());
        }
        lastCycles = snap.emulatorCycles;
        lastInstructions = snap.emulatorInstructions;
        lastTime = now;

        // DON_IOP_DUMP=<address> dumps 0x100 bytes of IOP RAM once, so that the loop a busy IOP thread sits
        // in can be disassembled without a patched emulator (readIopMemory is the runtime's public path).
        static const bool dumped = [&runtime]() {
            const char *text = std::getenv("DON_IOP_DUMP");
            if (text == nullptr)
            {
                return false;
            }
            const unsigned long address = std::strtoul(text, nullptr, 0);
            uint8_t bytes[0x100] = {};
            if (!runtime.readIopMemory(static_cast<uint32_t>(address), bytes, sizeof(bytes)))
            {
                std::fprintf(stderr, "[don-iop] cannot read 0x%lX from IOP RAM\n", address);
                return true;
            }
            for (size_t offset = 0; offset < sizeof(bytes); offset += 16u)
            {
                std::fprintf(stderr, "[don-iop-dump] 0x%08lX:", address + offset);
                for (size_t i = 0; i < 16u; i += 4u)
                {
                    uint32_t word = 0;
                    std::memcpy(&word, bytes + offset + i, sizeof(word));
                    std::fprintf(stderr, " 0x%08X", word);
                }
                std::fprintf(stderr, "\n");
            }
            return true;
        }();
        (void)dumped;
    }

    // DON_TRACE_STATE=<n>: every n-th vertical blank, one line with everything two runs of the same script
    // can be compared on: the blank, the emulated EE cycle, the IOP's cycle and instruction counters, and a
    // 64-bit hash of the 32 MiB of guest RAM. The first blank where two runs differ is where the run stops
    // being a function of its inputs; the hash says whether the guest's own state (not only the cycles) has
    // parted. It is the tool for journals 009-011 (docs/journal/).
    //
    // The variable is read once, on the first blank. Unset or 0: one comparison per blank and nothing else
    // (no memory is touched, no clock is read). Cost when on: the hash walks the whole 32 MiB in 64-bit
    // FNV-1a steps, which is a few milliseconds (printed once, on the first line, so the figure is the one
    // of the machine that ran); one line per n blanks therefore costs that per n/60 s of emulated time,
    // ~0.3% of the game thread at n=60. The hash reads memory and writes none, so a traced run is the same
    // run as an untraced one apart from the wall time it takes.
    uint64_t hashGuestRam(const uint8_t *ram)
    {
        uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a, 64 bit
        for (size_t offset = 0; offset + sizeof(uint64_t) <= PS2_RAM_SIZE; offset += sizeof(uint64_t))
        {
            uint64_t word = 0;
            std::memcpy(&word, ram + offset, sizeof(word));
            hash ^= word;
            hash *= 0x00000100000001B3ull;
        }
        return hash;
    }

    void dumpState(PS2Runtime &runtime, uint64_t tick)
    {
        static const uint64_t interval = []() -> uint64_t {
            const char *text = std::getenv("DON_TRACE_STATE");
            if (text == nullptr || text[0] == '\0')
            {
                return 0ull;
            }
            return std::strtoull(text, nullptr, 0);
        }();
        if (interval == 0ull || tick % interval != 0ull)
        {
            return;
        }

        const uint8_t *ram = runtime.memory().getRDRAM();
        const auto hashStart = std::chrono::steady_clock::now();
        const uint64_t hash = hashGuestRam(ram);
        static bool costPrinted = false;
        const double hashMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - hashStart)
                                  .count();
        if (!costPrinted)
        {
            costPrinted = true;
            std::fprintf(stderr, "[don] state: hashing guest RAM costs %.1f ms per line\n", hashMs);
        }

        // DON_STATE_DUMP=<file> and DON_STATE_DUMP_AT=<blank>: write the 32 MiB of guest RAM once, at the
        // very instant the state line of that blank is printed, so that two runs can be compared at the
        // exact moment they first differ (the accumulated dumps of the end of a run cannot: a value that
        // differs for one frame is overwritten by the time the run ends). The file is game memory: it
        // belongs under work/ and never in git (AGENTS.md, rule 1).
        if (const char *dumpPath = std::getenv("DON_STATE_DUMP"); dumpPath != nullptr && dumpPath[0] != '\0')
        {
            const char *dumpAt = std::getenv("DON_STATE_DUMP_AT");
            const uint64_t at = dumpAt != nullptr ? std::strtoull(dumpAt, nullptr, 0) : 0ull;
            if (at != 0ull && tick == at)
            {
                if (std::FILE *file = std::fopen(dumpPath, "wb"))
                {
                    const size_t written = std::fwrite(ram, 1, PS2_RAM_SIZE, file);
                    std::fclose(file);
                    std::fprintf(stderr, "[don] state: dump %s at blank %llu (%zu bytes)\n", dumpPath,
                                 static_cast<unsigned long long>(tick), written);
                }
                else
                {
                    std::fprintf(stderr, "[don] state: cannot write %s\n", dumpPath);
                }
            }
        }

        const ps2x::iop::DebugSnapshot iop = runtime.iopDebugSnapshot();
        uint64_t eeCycle = 0u;
#if defined(PS2X_EE_SCHEDULER_STATS)
        // The cycle counter of patch 0034 (a plain read on the game thread). Without it the field is 0:
        // snapshot() is not used here because asking for a snapshot changes when the scheduler runs.
        eeCycle = runtime.eeScheduler().eeCycle();
#endif
        std::fprintf(stderr,
                     "[don] state blank=%llu eeCycle=%llu iopCycles=%llu iopInstructions=%llu "
                     "ramHash=%016llx\n",
                     static_cast<unsigned long long>(tick), static_cast<unsigned long long>(eeCycle),
                     static_cast<unsigned long long>(iop.emulatorCycles),
                     static_cast<unsigned long long>(iop.emulatorInstructions),
                     static_cast<unsigned long long>(hash));
    }
}
