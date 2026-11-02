// SPDX-License-Identifier: GPL-3.0-or-later
#include "gs_glue.h"

#include "fp_env.h"
#include "gs_thread.h"
#include "gs_trace.h"
#include "profile.h"
#include "screenshots.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
#if defined(_WIN32)
    constexpr const char *kLibraryName = "don_gs.dll";
#elif defined(__APPLE__)
    constexpr const char *kLibraryName = "libdon_gs.dylib";
#else
    constexpr const char *kLibraryName = "libdon_gs.so";
#endif

    bool env_flag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && value[0] == '1';
    }

    uint64_t since_ns(Clock::time_point from)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - from).count());
    }

    template <typename Fn>
    bool load_symbol(void *library, const char *name, Fn &out, std::string &err)
    {
        SDL_FunctionPointer fn = SDL_LoadFunction(static_cast<SDL_SharedObject *>(library), name);
        if (!fn)
        {
            err = std::string("the GS library has no function ") + name;
            return false;
        }
        out = reinterpret_cast<Fn>(fn);
        return true;
    }

} // namespace

bool don_gs_load(const std::string &gs_dir, DonGsApi &api, std::string &err)
{
    std::vector<fs::path> dirs;
    const char *env_dir = std::getenv("DON_GS_DIR");
    if (!gs_dir.empty())
        dirs.emplace_back(gs_dir);
    else if (env_dir && env_dir[0])
        dirs.emplace_back(env_dir);
    else
    {
        const char *base_text = SDL_GetBasePath();
        const fs::path base = base_text ? fs::path(base_text) : fs::path(".");
        dirs.push_back(base);
        dirs.push_back(base / "gs");
        dirs.push_back((base / ".." / "gs").lexically_normal());
    }

    err.clear();
    for (const fs::path &dir : dirs)
    {
        const fs::path candidate = dir / kLibraryName;
        std::error_code ec;
        if (!fs::exists(candidate, ec))
        {
            err += "  " + candidate.string() + ": not found\n";
            continue;
        }
        // An absolute path: the library finds its own resources and dependencies relative to itself.
        const fs::path absolute = fs::absolute(candidate, ec);
        SDL_SharedObject *library = SDL_LoadObject(absolute.string().c_str());
        if (!library)
        {
            err += "  " + absolute.string() + ": " + SDL_GetError() + "\n";
            continue;
        }

        DonGsApi loaded;
        loaded.library = library;
        loaded.path = absolute;
        std::string bind_error;
        int (*version)() = nullptr;
        const bool ok = load_symbol(library, "don_gs_version", version, bind_error) &&
                        load_symbol(library, "don_gs_open", loaded.open, bind_error) &&
                        load_symbol(library, "don_gs_close", loaded.close, bind_error) &&
                        load_symbol(library, "don_gs_reset", loaded.reset, bind_error) &&
                        load_symbol(library, "don_gs_gif_transfer", loaded.gif_transfer, bind_error) &&
                        load_symbol(library, "don_gs_vsync", loaded.vsync, bind_error) &&
                        load_symbol(library, "don_gs_read_fifo", loaded.read_fifo, bind_error) &&
                        load_symbol(library, "don_gs_snapshot_rgba", loaded.snapshot_rgba, bind_error) &&
                        load_symbol(library, "don_gs_resize", loaded.resize, bind_error);
        // Optional (T-520): present in newer libraries only; the host skips the device block when absent.
        load_symbol(library, "don_gs_device_info", loaded.device_info, bind_error);
        if (!ok || version() != 1)
        {
            err += "  " + absolute.string() + ": " + (ok ? "unsupported API version" : bind_error) + "\n";
            SDL_UnloadObject(library);
            continue;
        }
        api = loaded;
        err.clear();
        return true;
    }
    err = "cannot find the GS library (" + std::string(kLibraryName) + "); tried:\n" + err +
          "set --gs-dir or DON_GS_DIR to the folder that holds it";
    return false;
}

HostGs::HostGs(const DonGsApi &api, const HostGsParams &params, std::unique_ptr<ScreenshotWriter> shots)
    : m_api(api), m_params(params), m_shots(std::move(shots)), m_log(env_flag("DON_HOST_LOG")), m_pacing_log(std::getenv("DON_PACING_LOG") && std::getenv("DON_PACING_LOG")[0] != 0 && std::getenv("DON_PACING_LOG")[0] != 0x30),
      m_trace_regs(env_flag("DON_TRACE_GS_REGS"))
{
    if (const char *value = std::getenv("DON_PACING_LOG"); value != nullptr && m_pacing_log)
    {
        const unsigned long long from = std::strtoull(value, nullptr, 10);
        m_pacing_from = from > 1 ? from : 1;
    }
    m_iface.user = this;
    m_iface.thread_begin = &HostGs::thread_begin;
    m_iface.thread_end = &HostGs::thread_end;
    m_iface.gif_transfer = &HostGs::gif_transfer;
    m_iface.vsync = &HostGs::vsync;
    m_iface.reset = &HostGs::reset;
    m_iface.read_fifo = &HostGs::read_fifo;
}

HostGs::~HostGs() = default;

void HostGs::request_resize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    m_pending_resize.store((static_cast<uint64_t>(width) << 32) | static_cast<uint32_t>(height),
                           std::memory_order_release);
}

void HostGs::thread_begin(void *self) { static_cast<HostGs *>(self)->on_thread_begin(); }
void HostGs::thread_end(void *self) { static_cast<HostGs *>(self)->on_thread_end(); }

void HostGs::on_thread_begin()
{
    // T-206: this is the game thread: the game's floating point runs with the console's mode (round
    // toward zero, FTZ and DAZ on) unless DON_FP_MODE=host. The calls that enter the GS library get the
    // host's value back for their duration (fp_env::HostCsrScope below), because the GS in the reference
    // emulator runs on a thread with the default mode.
    fp_env::enter_game(fp_env::mode_from_env());

    // T-203: this is the game thread; DON_PROFILE=file arms the sampler for it (no cost when unset).
    profile_thread_begin("GameThread");

    // T-303: the SPU2 opens here (before any IOP module is loaded) and installs its table.
    if (m_on_thread_begin)
        m_on_thread_begin();

    const char *thread_env = std::getenv("DON_GS_THREAD");
    const bool threaded = !(thread_env && thread_env[0] == '0');
    bool opened = false;
    if (threaded)
    {
        GsThread::Callbacks cb;
        cb.open = [this] { return open_gs(); };
        cb.close = [this] { m_api.close(); };
        cb.gif = [this](int path, const uint8_t *data, uint32_t bytes) { exec_gif(path, data, bytes); };
        cb.vsync = [this](const uint64_t *regs, int field) { exec_vsync(regs, field); };
        cb.reset = [this] { m_api.reset(); };
        cb.resize = [this](int w, int h) { m_api.resize(w, h); };
        m_gs_thread = std::make_unique<GsThread>(std::move(cb));
        opened = m_gs_thread->start();
        if (!opened)
            m_gs_thread.reset();
    }
    else
    {
        opened = open_gs();
    }
    if (!opened)
    {
        m_state.store(Failed, std::memory_order_release);
        if (m_on_open_failed)
            m_on_open_failed();
        return;
    }
    m_state.store(Open, std::memory_order_release);
}

// Opens the GS and captures the device text. Runs on the thread that will use the library.
bool HostGs::open_gs()
{
    DonGsConfig cfg = m_params.window;
    cfg.renderer = m_params.renderer;
    cfg.upscale = m_params.upscale;
    cfg.vsync = (cfg.native_window != nullptr && m_params.vsync) ? 1 : 0;

    char err[512] = {};
    const auto start = Clock::now();
    if (m_api.open(&cfg, err, sizeof(err)) != 0)
    {
        m_error = err[0] ? err : "don_gs_open failed";
        return false;
    }
    if (m_log)
        std::fprintf(stderr, "[don] GS open in %.1f ms (%s renderer, %s)\n", since_ns(start) / 1.0e6,
                     cfg.renderer == DON_GS_RENDERER_SOFTWARE ? "software" : "vulkan",
                     cfg.native_window ? "window" : "headless");

    // T-520: the renderer and the Vulkan devices, for the [don] start block. The main thread reads it once it
    // sees Open (published after this returns).
    if (m_api.device_info)
    {
        char text[4096] = {};
        if (m_api.device_info(text, sizeof(text)) == 0)
            m_device_info = text;
    }
    return true;
}

void HostGs::on_thread_end()
{
    // Close on the thread that opened it. Nothing to close when the open failed.
    if (m_state.load(std::memory_order_acquire) == Open)
    {
        if (m_gs_thread)
        {
            m_gs_thread->stop(); // executes what is queued, closes on the GS thread
            m_gs_thread.reset();
        }
        else
            m_api.close();
    }
    m_state.store(m_state.load() == Failed ? Failed : Closed, std::memory_order_release);

    // T-203: the game thread ends here; the sampler is disarmed. profile_finish() (the main thread,
    // after stopGameThread()) writes the file.
    profile_thread_end();

    // T-303: the SPU2 removes its table and closes here, on the thread that opened it.
    if (m_on_thread_end)
        m_on_thread_end();

    // T-206: the game thread is over; the host's floating-point mode comes back.
    fp_env::leave_game();
}

void HostGs::gif_transfer(void *self, int path, const uint8_t *data, uint32_t bytes)
{
    auto *gs = static_cast<HostGs *>(self);
    if (gs->m_state.load(std::memory_order_relaxed) != Open)
        return;
    if (bytes == 0)
        return;
    // T-202 step 4a: one frame's transfers, for tools/verify/gif_stats.py. Off unless DON_GS_PACKETS
    // names a file (one getenv at startup); this is the only place that sees every transfer with the
    // path the runtime used.
    gs_trace::transfer(path, data, bytes);
    if (gs->m_gs_thread)
    {
        // The GS thread has the host's mode; here the packet is only copied into the ring.
        if (!gs->m_pacing_log)
        {
            gs->m_gs_thread->gif(path, data, bytes);
            return;
        }
        const auto begin = Clock::now();
        gs->m_gs_thread->gif(path, data, bytes);
        gs->m_pacing_gif_ns += since_ns(begin);
        return;
    }
    // T-206: the GS library is entered with the host's floating-point mode (as in the reference
    // emulator) and the game's mode is back the moment this returns.
    const fp_env::HostCsrScope fp_scope;
    if (!gs->m_pacing_log)
    {
        gs->exec_gif(path, data, bytes);
        return;
    }
    const auto begin = Clock::now(); // T-305: DON_PACING_LOG splits the work of a blank
    gs->exec_gif(path, data, bytes);
    gs->m_pacing_gif_ns += since_ns(begin);
}

void HostGs::exec_gif(int path, const uint8_t *data, uint32_t bytes)
{
    if (!m_log)
    {
        m_api.gif_transfer(path, data, bytes);
        return;
    }
    const auto start = Clock::now();
    m_api.gif_transfer(path, data, bytes);
    m_gs_ns_frame += since_ns(start);
    m_bytes_total += bytes;
}

void HostGs::vsync(void *self, const uint64_t regs[PS2_HOST_GS_PRIV_REG_COUNT], int field)
{
    static_cast<HostGs *>(self)->on_vsync(regs, field);
}

void HostGs::on_vsync(const uint64_t *regs, int field)
{
    if (m_state.load(std::memory_order_relaxed) != Open)
        return;

    // A resize noticed on the main thread reaches the GS here, on the game thread, before the blank is
    // composed.
    const uint64_t resize = m_pending_resize.exchange(0, std::memory_order_acquire);
    if (resize != 0)
    {
        if (m_gs_thread)
            m_gs_thread->resize(static_cast<int>(resize >> 32), static_cast<int>(resize & 0xFFFFFFFFu));
        else
        {
            const fp_env::HostCsrScope fp_scope; // T-206: the library sees the host's mode
            m_api.resize(static_cast<int>(resize >> 32), static_cast<int>(resize & 0xFFFFFFFFu));
        }
    }

    if (m_trace_regs)
        trace_registers(regs);

    const auto renderer_start = m_pacing_log ? Clock::now() : Clock::time_point();
    if (m_gs_thread)
        m_gs_thread->vsync(regs, PS2_HOST_GS_PRIV_REG_COUNT, field); // waits if the GS is too far behind
    else
    {
        const fp_env::HostCsrScope fp_scope; // T-206
        exec_vsync(regs, field); // composes the picture and, with a window, presents it
    }
    const uint64_t frame = m_vblanks.fetch_add(1, std::memory_order_relaxed) + 1;
    profile_set_blank(frame);

    // T-520: frame timing, the worst 1 s window and the slow-frame count (always on, cheap).
    perf_tick();

    // T-520: the audio-pacing wait is the time the vblank hook (audio pacing) holds the game back;
    // the worst of the current window goes into the perf line.
    if (m_vblank_hook)
    {
        const auto hook_start = Clock::now();
        m_vblank_hook();
        const uint64_t wait_us = since_ns(hook_start) / 1000u;
        uint64_t prev = m_perf_audio_wait_max_us.load(std::memory_order_relaxed);
        while (wait_us > prev &&
               !m_perf_audio_wait_max_us.compare_exchange_weak(prev, wait_us, std::memory_order_relaxed))
            ;
    }
    if (m_pacing_log)
    {
        // T-305: the interval is measured between the ends of two vsync calls; the first blank only starts the clock.
        const auto now = Clock::now();
        const uint64_t waited = m_pacing_wait_ns ? m_pacing_wait_ns() : 0;
        if (m_pacing_prev != Clock::time_point() && frame >= m_pacing_from)
        {
            pacing::Sample sample;
            sample.blank = frame;
            sample.interval_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_pacing_prev).count());
            sample.renderer_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - renderer_start).count());
            sample.pacing_ns = waited - m_pacing_prev_wait;
            sample.gif_ns = m_pacing_gif_ns;
            m_pacing_samples.push_back(sample);
        }
        m_pacing_gif_ns = 0;
        m_pacing_prev = now;
        m_pacing_prev_wait = waited;
    }
    // T-202 step 4a: the frame's transfers are those seen since the previous blank; the capture (when
    // DON_GS_PACKETS is set) closes its file at the end of the frame it was asked for.
    gs_trace::vsync(frame);
    if (m_shots && m_shots->enabled())
    {
        if (m_gs_thread)
            m_gs_thread->post([this, frame] { m_shots->on_vsync(frame); }); // queued behind this blank
        else
            m_shots->on_vsync(frame);
    }
    // T-111: the input script steps here, on the game thread, at the end of the blank it belongs to.
    if (m_on_vblank)
        m_on_vblank(frame);
}

// The GS side of a blank (the GS thread, or the game thread when DON_GS_THREAD=0): compose, and the
// DON_HOST_LOG statistics.
void HostGs::exec_vsync(const uint64_t *regs, int field)
{
    const auto start = m_log ? Clock::now() : Clock::time_point();
    m_api.vsync(regs, field);
    if (m_log)
    {
        const uint64_t frame = ++m_gs_frames;
        m_gs_ns_frame += since_ns(start);
        m_gs_ns_total += m_gs_ns_frame;
        if (m_gs_ns_frame > m_gs_ns_max)
            m_gs_ns_max = m_gs_ns_frame;
        m_gs_ns_frame = 0;
        if (frame % 300 == 0)
            std::fprintf(stderr, "[don] %llu vblanks, %.2f ms in the GS per vblank (max %.2f)\n",
                         static_cast<unsigned long long>(frame), m_gs_ns_total / 1.0e6 / frame, m_gs_ns_max / 1.0e6);
    }
}

// T-520: one step per vsync, on the game thread: frame time, the slow-frame count, the timestamp
// ring and the worst trailing 1 s window (the "min_fps" of the perf line). Cheap: a handful of
// integer updates and one short backward scan, no allocation.
void HostGs::perf_tick()
{
    const auto now = Clock::now();
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());

    if (m_perf_prev != Clock::time_point())
    {
        const uint64_t frame_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - m_perf_prev).count());
        uint64_t prev = m_perf_max_frame_us.load(std::memory_order_relaxed);
        while (frame_us > prev &&
               !m_perf_max_frame_us.compare_exchange_weak(prev, frame_us, std::memory_order_relaxed))
            ;
        uint64_t wprev = m_window_max_frame_us.load(std::memory_order_relaxed);
        while (frame_us > wprev &&
               !m_window_max_frame_us.compare_exchange_weak(wprev, frame_us, std::memory_order_relaxed))
            ;
        if (frame_us > 33000u)
            m_perf_slow_frames.fetch_add(1, std::memory_order_relaxed);
    }
    m_perf_prev = now;

    m_perf_ring[m_perf_ring_head] = now_ns;
    m_perf_ring_head = (m_perf_ring_head + 1) % kPerfRingSize;
    if (m_perf_ring_count < kPerfRingSize)
        ++m_perf_ring_count;

    // The worst 1 s window: count the frames of the last second, but only once the ring already
    // spans at least a full second (a partial first second would read as a false dip).
    const uint64_t one_s_ago = now_ns - 1000000000ull;
    const uint32_t oldest_idx = (m_perf_ring_head + kPerfRingSize - m_perf_ring_count) % kPerfRingSize;
    if (m_perf_ring_count >= 2 && m_perf_ring[oldest_idx] <= one_s_ago)
    {
        uint64_t count = 0;
        for (uint32_t i = 0; i < m_perf_ring_count; ++i)
        {
            const uint32_t idx = (m_perf_ring_head + kPerfRingSize - 1 - i) % kPerfRingSize;
            if (m_perf_ring[idx] < one_s_ago)
                break; // timestamps are non-decreasing backwards from the newest
            ++count;
        }
        uint64_t prev = m_perf_min_fps.load(std::memory_order_relaxed);
        while (count < prev &&
               !m_perf_min_fps.compare_exchange_weak(prev, count, std::memory_order_relaxed))
            ;
    }
}

// DON_TRACE_GS_REGS=1: one line per change of the privileged registers, in the order of
// runtime/gs/ps2_host_gs.h. The values are the ones the runtime hands to Ps2HostGs::vsync, i.e. what the
// guest's writes left in the registers of the runtime's GS at this vertical blank.
void HostGs::trace_registers(const uint64_t *regs)
{
    if (m_have_last_regs && std::memcmp(m_last_regs, regs, sizeof(m_last_regs)) == 0)
        return;
    std::memcpy(m_last_regs, regs, sizeof(m_last_regs));
    m_have_last_regs = true;

    static const char *const kNames[PS2_HOST_GS_PRIV_REG_COUNT] = {
        "PMODE", "SMODE1", "SMODE2", "SRFSH",   "SYNCH1",   "SYNCH2", "SYNCV",  "DISPFB1", "DISPLAY1", "DISPFB2",
        "DISPLAY2", "EXTBUF", "EXTDATA", "EXTWRITE", "BGCOLOR", "CSR",   "IMR",    "BUSDIR",  "SIGLBLID"};
    std::fprintf(stderr, "[don-gs] regs");
    for (int i = 0; i < PS2_HOST_GS_PRIV_REG_COUNT; ++i)
        std::fprintf(stderr, " %s=0x%llX", kNames[i], static_cast<unsigned long long>(regs[i]));
    std::fprintf(stderr, "\n");
}

bool HostGs::write_shot(const std::string &name)
{
    if (!m_shots)
        return false;
    if (!m_gs_thread)
        return m_shots->write_named(name);
    bool ok = false;
    m_gs_thread->run_sync([&] { ok = m_shots->write_named(name); }); // after every queued blank
    return ok;
}

void HostGs::reset(void *self)
{
    auto *gs = static_cast<HostGs *>(self);
    if (gs->m_state.load(std::memory_order_relaxed) != Open)
        return;
    if (gs->m_gs_thread)
    {
        gs->m_gs_thread->reset();
        return;
    }
    const fp_env::HostCsrScope fp_scope; // T-206
    gs->m_api.reset();
}

uint32_t HostGs::read_fifo(void *self, uint8_t *dst, uint32_t bytes)
{
    auto *gs = static_cast<HostGs *>(self);
    if (gs->m_state.load(std::memory_order_relaxed) != Open)
        return 0;
    if (gs->m_gs_thread)
    {
        // The game reads `dst` (guest RAM) as soon as this returns: wait for every queued command, read on
        // the GS thread into a host buffer, copy here.
        std::vector<uint8_t> tmp(bytes);
        uint32_t got = 0;
        gs->m_gs_thread->run_sync([&] { got = gs->m_api.read_fifo(tmp.data(), bytes); });
        if (got > 0)
            std::memcpy(dst, tmp.data(), got > bytes ? bytes : got);
        return got;
    }
    const fp_env::HostCsrScope fp_scope; // T-206
    return gs->m_api.read_fifo(dst, bytes);
}

void HostGs::print_summary() const
{
    if (!m_log)
        return;
    const uint64_t frames = m_vblanks.load();
    std::fprintf(stderr, "[don] GS: %llu vblanks, %.2f ms in the GS per vblank (max %.2f), %.1f MiB of GIF data\n",
                 static_cast<unsigned long long>(frames), frames ? m_gs_ns_total / 1.0e6 / frames : 0.0,
                 m_gs_ns_max / 1.0e6, m_bytes_total / (1024.0 * 1024.0));
}

void HostGs::print_pacing_summary(double audio_write_worst_ms) const
{
    if (!m_pacing_log)
        return;
    pacing::print(stderr, pacing::summarize(m_pacing_samples), audio_write_worst_ms);
}
