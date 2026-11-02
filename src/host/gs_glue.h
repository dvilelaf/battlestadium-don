// SPDX-License-Identifier: GPL-3.0-or-later
// The GS library (libdon_gs, src/gsbridge/don_gs.h) loaded at run time, and the Ps2HostGs that connects it to
// the runtime.
//
// Why it is loaded at run time (SDL_LoadObject) and not linked:
//   * a missing or unloadable library is an ordinary error with a message and exit code 3, not a loader
//     failure before main() runs;
//   * on Windows there is no import library to build or ship: the DLL is found by name in a folder and nothing
//     else (the host and the library never share a C++ runtime: the API is plain C);
//   * the library sits in a folder of its own (build/gs: it brings PCSX2's resources, the shader compiler and
//     other shared libraries, all found relative to the library itself), which the host may point at with
//     --gs-dir or DON_GS_DIR without a relink.
// The cost is a table of function pointers (DonGsApi), filled once.
//
// Threads: by default the GS library lives on a thread of its own (gs_thread.h): thread_begin starts it and waits
// for the open, thread_end drains it and closes there; the game thread only queues commands, and readbacks and
// screenshots wait for the queue to drain. DON_GS_THREAD=0 keeps the older synchronous path (every call and the
// close on the game thread). The main thread only talks to HostGs through atomics (state, resize, error text
// after the state has been published).
#pragma once

#include "don_gs.h"
#include "options.h"
#include "pacing_stats.h"
#include "runtime/gs/ps2_host_gs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ScreenshotWriter;
class GsThread;

struct DonGsApi
{
    void *library = nullptr; // SDL_SharedObject*
    std::filesystem::path path;
    int (*open)(const DonGsConfig *, char *, int) = nullptr;
    void (*close)() = nullptr;
    void (*reset)() = nullptr;
    void (*gif_transfer)(int, const uint8_t *, uint32_t) = nullptr;
    void (*vsync)(const uint64_t *, int) = nullptr;
    uint32_t (*read_fifo)(uint8_t *, uint32_t) = nullptr;
    int (*snapshot_rgba)(uint8_t *, int *, int *, int) = nullptr;
    void (*resize)(int, int) = nullptr;
    // T-520: the renderer and the Vulkan devices (don_gs_device_info), optional (older libraries lack it).
    int (*device_info)(char *, int) = nullptr;
};

// Looks for the library in `gs_dir` if given, else in $DON_GS_DIR if set (an explicit folder is the only one
// searched), else next to the executable, in a "gs" folder next to it and in a sibling "../gs" folder (the
// layout of build/). Fills `api` and returns true, or returns false with the folders tried in `err`.
bool don_gs_load(const std::string &gs_dir, DonGsApi &api, std::string &err);

struct HostGsParams
{
    int renderer = DON_GS_RENDERER_VULKAN;
    int upscale = 1;
    bool vsync = true;
    // Native window for the GS, or window_type DON_GS_WINDOW_NONE for headless (see window.h).
    DonGsConfig window{}; // zero: headless (an uninitialised window_type was read by the GS on Windows)
};

class HostGs
{
public:
    enum State : int
    {
        Pending, // thread_begin has not run yet
        Open,
        Failed, // don_gs_open failed on the game thread; error() has the message
        Closed, // thread_end ran
    };

    HostGs(const DonGsApi &api, const HostGsParams &params, std::unique_ptr<ScreenshotWriter> shots);
    ~HostGs();

    // The interface to install with PS2Runtime::setHostGs() (the runtime copies it; `this` must outlive the
    // runtime).
    const Ps2HostGs &iface() const { return m_iface; }

    // Called on the game thread from thread_begin when don_gs_open fails, to ask the runtime to stop.
    void set_on_open_failed(std::function<void()> fn) { m_on_open_failed = std::move(fn); }

    // Any thread.
    State state() const { return m_state.load(std::memory_order_acquire); }
    const std::string &error() const { return m_error; } // valid once state() is Failed
    uint64_t vblanks() const { return m_vblanks.load(std::memory_order_relaxed); }

    // T-520: the renderer and the Vulkan devices (don_gs_device_info), filled on the game thread
    // right after the GS opens. Valid once state() is Open (empty when the library lacks the symbol).
    const std::string &device_info() const { return m_device_info; }

    // T-520: performance counters, written by the game thread at every vsync and read by the main
    // thread for the "[don] perf" line. Always on, cheap: a few counters and one small timestamp ring.
    uint64_t perf_slow_frames() const { return m_perf_slow_frames.load(std::memory_order_relaxed); }
    uint64_t perf_max_frame_us() const { return m_perf_max_frame_us.load(std::memory_order_relaxed); }
    // T-757: worst frame time since the previous call (resets it); the run-long maximum above is untouched.
    uint64_t take_perf_max_frame_us() { return m_window_max_frame_us.exchange(0, std::memory_order_relaxed); }
    uint64_t perf_min_fps() const { return m_perf_min_fps.load(std::memory_order_relaxed); }
    // Returns and clears the worst audio-pacing wait since the previous call (the main thread calls
    // this once per 10 s window).
    uint64_t take_audio_wait_max_us() { return m_perf_audio_wait_max_us.exchange(0, std::memory_order_relaxed); }

    // Main thread: the window surface now has this size in pixels. Applied by the game thread at its next
    // vertical blank.
    void request_resize(int width, int height);

    // Summary (vblanks, time inside the GS calls) for DON_HOST_LOG.
    void print_summary() const;

private:
    static void thread_begin(void *self);
    static void thread_end(void *self);
    static void gif_transfer(void *self, int path, const uint8_t *data, uint32_t bytes);
    static void vsync(void *self, const uint64_t regs[PS2_HOST_GS_PRIV_REG_COUNT], int field);
    static void reset(void *self);
    static uint32_t read_fifo(void *self, uint8_t *dst, uint32_t bytes);

    void on_thread_begin();
    void on_thread_end();
    void on_vsync(const uint64_t *regs, int field);
    // Run where the GS library is used (the GS thread, or the game thread when DON_GS_THREAD=0).
    bool open_gs();
    void exec_gif(int path, const uint8_t *data, uint32_t bytes);
    void exec_vsync(const uint64_t *regs, int field);
    void trace_registers(const uint64_t *regs);

public:
    // Called on the game thread at the end of every vertical blank, with the number of the blank (1, 2, ...).
    // T-111: this is where the input script is stepped and the pad states are published, so that a button
    // pressed "at frame N" is visible to the game from that blank on. Must be set before the game thread
    // starts (it is read on the game thread and written on the main one before startGameThread()).
    void set_on_vblank(std::function<void(uint64_t)> callback) { m_on_vblank = std::move(callback); }

    // T-303: the two ends of the game thread, for the pieces that must run on it (the SPU2 library is
    // opened on the thread that uses it, before any IOP module is loaded, and closed there). The begin
    // hook runs first in thread_begin (before the GS opens) and the end hook last in thread_end (after
    // the GS closed). Both must be set before startGameThread().
    void set_on_thread_begin(std::function<void()> callback) { m_on_thread_begin = std::move(callback); }
    void set_on_thread_end(std::function<void()> callback) { m_on_thread_end = std::move(callback); }

    // T-305, DON_PACING_LOG=1: `idle_ns` returns the scheduler's total pacing wait so far (called on the game
    // thread at each blank). print_pacing_summary() is called once the game thread has stopped.
    // T-303: called on the game thread at every vertical blank, after the picture is composed. The host uses it
    // to hold the game back while too much sound is queued (it never changes what the game computes).
    void set_vblank_hook(std::function<void()> hook) { m_vblank_hook = std::move(hook); }
    void set_pacing_wait_source(std::function<uint64_t()> idle_ns) { m_pacing_wait_ns = std::move(idle_ns); }
    bool pacing_log() const { return m_pacing_log; }
    void print_pacing_summary(double audio_write_worst_ms) const;

    // The script's `shot NAME`: one PNG of this moment (game thread, at a vertical blank).
    bool write_shot(const std::string &name);

private:
    const DonGsApi &m_api;
    HostGsParams m_params;
    std::unique_ptr<ScreenshotWriter> m_shots;
    Ps2HostGs m_iface;
    std::function<void()> m_on_open_failed;
    std::function<void(uint64_t)> m_on_vblank;
    std::function<void()> m_on_thread_begin;
    std::function<void()> m_on_thread_end;

    std::unique_ptr<GsThread> m_gs_thread; // null: synchronous path
    uint64_t m_gs_frames = 0;              // blanks executed by the GS (GS side, for the DON_HOST_LOG line)
    std::atomic<State> m_state{Pending};
    std::string m_error;
    std::atomic<uint64_t> m_vblanks{0};
    std::atomic<uint64_t> m_pending_resize{0}; // (width << 32) | height, 0 = none

    // DON_HOST_LOG statistics (game thread writes, the summary reads after the thread was joined).
    bool m_log = false;
    // T-305: DON_PACING_LOG=1. Game thread writes; the summary reads after the thread was joined.
    bool m_pacing_log = false;
    uint64_t m_pacing_from = 1; // DON_PACING_LOG=N records from blank N on (1 or any flag: from the start)
    std::function<uint64_t()> m_pacing_wait_ns;
    std::function<void()> m_vblank_hook;
    std::vector<pacing::Sample> m_pacing_samples;
    std::chrono::steady_clock::time_point m_pacing_prev{};
    uint64_t m_pacing_prev_wait = 0;
    uint64_t m_pacing_gif_ns = 0; // game thread: time in the GS transfers since the last blank
    // DON_TRACE_GS_REGS=1: one [don-gs] line, hexadecimal, each time any of the 19 privileged registers changes
    // at a vertical blank, in the order of runtime/gs/ps2_host_gs.h. Game thread only.
    bool m_trace_regs = false;
    bool m_have_last_regs = false;
    uint64_t m_last_regs[PS2_HOST_GS_PRIV_REG_COUNT] = {};
    uint64_t m_gs_ns_total = 0;
    uint64_t m_gs_ns_max = 0;
    uint64_t m_gs_ns_frame = 0; // time in the GS calls since the last vblank
    uint64_t m_bytes_total = 0;

    // T-520: the renderer and the Vulkan devices (don_gs_device_info), captured on the game thread.
    std::string m_device_info;

    // T-520: always-on performance counters (game thread writes, main thread reads).
    std::atomic<uint64_t> m_perf_slow_frames{0};  // frames over 33 ms
    std::atomic<uint64_t> m_perf_max_frame_us{0}; // worst frame time
    std::atomic<uint64_t> m_window_max_frame_us{0}; // worst frame time of the current perf window
    std::atomic<uint64_t> m_perf_min_fps{~0ull};  // worst trailing 1 s window, in frames; ~0ull = none yet
    std::atomic<uint64_t> m_perf_audio_wait_max_us{0}; // worst audio-pacing wait in the current window
    // Game thread only: frame timing and the timestamp ring for the 1 s window.
    static constexpr uint32_t kPerfRingSize = 512; // >= 1 s of frames at any frame rate the game reaches
    std::chrono::steady_clock::time_point m_perf_prev{};
    uint64_t m_perf_ring[kPerfRingSize] = {}; // vsync timestamps, nanoseconds since epoch
    uint32_t m_perf_ring_count = 0;
    uint32_t m_perf_ring_head = 0;
    void perf_tick(); // one step per vsync: frame time, ring, worst 1 s window, slow frames
};
