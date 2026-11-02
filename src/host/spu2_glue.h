// SPDX-License-Identifier: GPL-3.0-or-later
// The SPU2 library (libdon_spu2, the C API of src/spu2bridge/don_spu2.h) loaded at run time, and the
// ps2x::iop::IopHostSpu2 table with which the IOP emulator talks to it (patches/ps2recomp/0006-iop-host-spu2.patch).
//
// Why it is loaded at run time (SDL_LoadObject) and not linked: the same reasons as the GS library
// (gs_glue.h): a missing or unloadable library is an ordinary error the caller turns into "run without
// sound" instead of a loader failure before main() runs; on Windows there is no import library to build
// or ship; and the library sits in a folder of its own (build/spu2) that the host may point at with
// --spu2-dir or DON_SPU2_DIR without a relink. The cost is a table of function pointers (DonSpu2Api).
//
// Threads. The six required callbacks of IopHostSpu2 are called from the game thread, the one that runs
// the IOP emulator (see the patch's header). open() and close() must be called from that same thread:
// don_spu2.h requires every non-pull call to come from the thread that called don_spu2_open. pull() is
// the one exception the library allows: one other thread (the host's loop, or an audio callback) may take
// the produced audio while the game thread advances the IOP. Installing the table (iopSetHostSpu2) and
// removing it are done on the main thread while the IOP is not running: before the game thread is
// started and after stopGameThread(), and the table must be removed before the library is closed.
#pragma once

#include "don_spu2.h"
#include "ps2x/iop/iop_host_spu2.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <string>
#include <thread>

// The functions of libdon_spu2, resolved once. All of them are non-null once don_spu2_load returned
// true; the signatures are the ones of don_spu2.h.
struct DonSpu2Api
{
    void *library = nullptr; // SDL_SharedObject*
    std::filesystem::path path;

    int (*version)() = nullptr;
    int (*open)() = nullptr;
    void (*close)() = nullptr;
    void (*reset)() = nullptr;
    void (*write16)(uint32_t addr, uint16_t value) = nullptr;
    uint16_t (*read16)(uint32_t addr) = nullptr;
    int32_t (*dma_write)(int core, const uint8_t *data, uint32_t bytes) = nullptr;
    int32_t (*dma_read)(int core, uint8_t *dst, uint32_t bytes) = nullptr;
    void (*advance)(uint64_t iop_cycles) = nullptr;
    uint64_t (*cycle)() = nullptr;
    int (*poll_event)(int *type, uint64_t *iop_cycle) = nullptr;
    uint32_t (*pull_output)(float *stereo, uint32_t frames) = nullptr;
};

// Looks for the library in `spu2_dir` if given, else in $DON_SPU2_DIR if set (an explicit folder is the
// only one searched), else next to the executable, in a "spu2" folder next to it and in a sibling
// "../spu2" folder (the layout of build/). Fills `api` and returns true, or returns false with a
// complete message (starting with "[don-spu2] ", prefix included) in `err`.
bool don_spu2_load(const std::string &spu2_dir, DonSpu2Api &api, std::string &err);

// One instance per process, owned by the host program. It does not touch the runtime or any program
// state: it owns the library, opens and closes it and hands out the IopHostSpu2 table that forwards to
// the functions of DonSpu2Api.
class HostSpu2
{
public:
    explicit HostSpu2(const DonSpu2Api &api);
    ~HostSpu2();
    HostSpu2(const HostSpu2 &) = delete;
    HostSpu2 &operator=(const HostSpu2 &) = delete;

    // Game thread (the one that runs the IOP), before the IOP executes anything. false and a message in
    // `err` when the library refuses (already open, or an internal failure); the caller runs without sound.
    bool open(std::string &err);
    // DON_IOP_THREAD=1: the library is opened and closed by the IOP worker (the table's thread_begin/thread_end), the
    // thread that makes every call but the host's pull. close() from any other thread then does nothing.
    void set_worker_owned() { m_workerOwned = true; }
    // Game thread, after the table has been removed from the emulator. Safe to call when not open.
    void close();
    bool is_open() const { return m_open.load(std::memory_order_acquire); }

    // The table to install with ps2x::iop::iopSetHostSpu2(&iface()). `user` is this object, which must
    // outlive the installation. It is complete whether or not the SPU2 is open; before open and after
    // close the callbacks do nothing, as don_spu2.h promises of the library itself.
    const ps2x::iop::IopHostSpu2 &iface() const { return m_iface; }

    // Pulls the frames produced so far (interleaved stereo floats in -1..1). This is the one call the
    // library allows from a thread other than the one that opened it: the host's loop uses it. Returns 0
    // when the SPU2 is not open or nothing is queued.
    uint32_t pull(float *stereo, uint32_t frames);

private:
    static void write16(void *user, uint32_t addr, uint16_t value);
    static uint16_t read16(void *user, uint32_t addr);
    static void dma_write(void *user, int core, const uint8_t *data, uint32_t bytes);
    static void dma_read(void *user, int core, uint8_t *dst, uint32_t bytes);
    static void advance(void *user, uint64_t iop_cycles);
    static int poll_event(void *user, int *type, uint64_t *iop_cycle);
    static void reset(void *user);

    // DON_TRACE_SPU2=1 (T-304): what the IOP emulator asks of the chip, one [don-spu2] line per emulated
    // second and a summary when the object is destroyed. All of it is written on the game thread.
    void trace_register(uint32_t addr, bool write, uint16_t value);
    void trace_report(uint64_t cycles);
    void trace_summary();

    const DonSpu2Api &m_api;
    ps2x::iop::IopHostSpu2 m_iface;
    // Written by open()/close() on the game thread, read by pull() on the host's thread.
    std::atomic<bool> m_open{false};
    bool m_workerOwned = false;
    std::thread::id m_owner;
    static void thread_begin(void *user);
    static void thread_end(void *user);

    bool m_trace = false;
    // DON_SPU2_CALLLOG=<file> (T-303): every advance, register write and DMA with the chip's time, to compare two runs.
    std::FILE *m_callLog = nullptr;
    uint64_t m_chipCycles = 0;
    uint64_t m_traceCycles = 0;    // IOP cycles advanced since the last line
    uint64_t m_traceTotalCycles = 0;
    uint64_t m_writes = 0, m_reads = 0;
    uint64_t m_dmaWrites[2] = {}, m_dmaBytes[2] = {}, m_dmaReads = 0, m_dmaReadBytes = 0;
    // T-304 point 5: peak |int16| of the DMA data of this second / of the whole run, per core.
    int m_dmaPeak[2] = {}, m_dmaPeakMax[2] = {};
    // T-304 Part 2.3: which register the driver polls (reads per second + the last value it got) and
    // every write to the control registers that matter while the music changes.
    uint64_t m_readsByReg[0x800] = {};
    uint64_t m_readsByRegLast[0x800] = {};
    uint16_t m_lastRead[0x800] = {};
    struct SpecialWrite
    {
        uint64_t cycle;
        uint16_t addr;
        uint16_t value;
    };
    static constexpr int kSpecialWrites = 48;
    SpecialWrite m_special[kSpecialWrites] = {};
    int m_specialCount = 0; // total pushed; the ring keeps the last kSpecialWrites
    uint64_t m_events[8] = {};
    uint64_t m_frames = 0;         // frames pulled by the host (pull())
    uint16_t m_last[0x800] = {};   // last value written to each chip register
    uint8_t m_touched[0x800] = {}; // 1 when that register was ever written
    uint64_t m_group[10] = {};     // writes since the last line, by register group
    uint64_t m_groupTotal[10] = {};// the same, for the whole run
    uint16_t m_konEver = 0, m_konEver1 = 0, m_koffEver = 0, m_koffEver1 = 0;
    uint64_t m_voiceWrites = 0;
};
