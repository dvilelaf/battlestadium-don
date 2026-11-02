// SPDX-License-Identifier: GPL-3.0-or-later
#include "gs_thread.h"

#include "fp_env.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define DON_CPU_RELAX() _mm_pause()
#else
#define DON_CPU_RELAX() std::this_thread::yield()
#endif

namespace
{
    constexpr uint64_t align16(uint64_t v) { return (v + 15u) & ~uint64_t(15u); }
    constexpr uint32_t kBatchLimit = 64u << 10;     // payload of one batch record
    constexpr uint32_t kMaxChunk = 4u << 20;        // a bigger transfer travels as several records (multiples of 16)
    constexpr auto kPoll = std::chrono::milliseconds(1);
} // namespace

GsThread::GsThread(Callbacks callbacks) : m_cb(std::move(callbacks)) {}

GsThread::~GsThread() { stop(); }

namespace
{
    void name_this_thread()
    {
#if defined(_WIN32)
        // SetThreadDescription exists from Windows 10 1607: looked up at run time so older systems still start.
        using SetDesc = HRESULT(WINAPI *)(HANDLE, PCWSTR);
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
            if (auto fn = reinterpret_cast<SetDesc>(reinterpret_cast<void *>(GetProcAddress(k32, "SetThreadDescription"))))
                fn(GetCurrentThread(), L"GsThread");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "GsThread");
#endif
    }
} // namespace

bool GsThread::start()
{
    if (const char *env = std::getenv("DON_GS_FRAMES_AHEAD"))
    {
        const int n = std::atoi(env);
        if (n >= 1 && n <= 4)
            m_frames_ahead = static_cast<uint32_t>(n);
    }
    m_ring = static_cast<uint8_t *>(::operator new(kRingBytes, std::align_val_t(64)));
    m_thread = std::thread(&GsThread::thread_main, this);
    m_running = true;
    // Wait for the open (it can take seconds: the Vulkan device and the shaders).
    while (m_open_result.load(std::memory_order_acquire) < 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (m_open_result.load(std::memory_order_acquire) == 0)
    {
        m_thread.join();
        m_running = false;
        return false;
    }
    return true;
}

void GsThread::stop()
{
    if (m_running)
    {
        flush_batch();
        reserve(0, Stop, 0);
        publish();
        m_thread.join();
        m_running = false;
    }
    if (m_ring)
    {
        ::operator delete(m_ring, std::align_val_t(64));
        m_ring = nullptr;
    }
}

// ----- producer ------------------------------------------------------------------------------------------

template <typename Ready> void GsThread::wait_producer(Ready ready)
{
    for (int i = 0; i < 2000; ++i)
    {
        if (ready())
            return;
        DON_CPU_RELAX();
    }
    m_producer_waiting.store(true, std::memory_order_seq_cst);
    while (!ready())
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_producer_cv.wait_for(lock, kPoll);
    }
    m_producer_waiting.store(false, std::memory_order_relaxed);
}

uint8_t *GsThread::reserve(uint32_t payload, uint32_t type, uint64_t a)
{
    const uint64_t total = sizeof(Header) + align16(payload);
    uint64_t wp = m_write.load(std::memory_order_relaxed);
    const uint64_t off = wp & (kRingBytes - 1);
    const uint64_t pad = (off + total > kRingBytes) ? kRingBytes - off : 0;
    const uint64_t need = pad + total;
    for (uint64_t r = m_read.load(std::memory_order_acquire); kRingBytes - (wp - r) < need;
         r = m_read.load(std::memory_order_acquire))
    {
        for (int i = 0; i < 2000 && kRingBytes - (wp - r) < need; ++i)
        {
            DON_CPU_RELAX();
            r = m_read.load(std::memory_order_acquire);
        }
        if (kRingBytes - (wp - r) >= need)
            break;
        m_ring_waiting.store(true, std::memory_order_seq_cst);
        if (m_read.load(std::memory_order_seq_cst) == r)
            m_read.wait(r, std::memory_order_acquire); // futex: woken by the consumer's advance
        m_ring_waiting.store(false, std::memory_order_relaxed);
    }
    if (pad)
    {
        Header h{Pad, 0, 0};
        std::memcpy(m_ring + off, &h, sizeof(h));
        wp += pad;
    }
    Header h{type, payload, a};
    std::memcpy(m_ring + (wp & (kRingBytes - 1)), &h, sizeof(h));
    m_pending_write = wp + total;
    return m_ring + (wp & (kRingBytes - 1)) + sizeof(Header);
}

void GsThread::publish()
{
    m_write.store(m_pending_write, std::memory_order_seq_cst);
    if (m_consumer_sleeping.load(std::memory_order_seq_cst))
        m_write.notify_one();
}

void GsThread::flush_batch()
{
    if (!m_batch)
        return;
    uint8_t *header = m_batch - sizeof(Header);
    const uint32_t size = m_batch_used;
    std::memcpy(header + offsetof(Header, size), &size, sizeof(size));
    m_batch = nullptr;
    m_pending_write = batch_start_ + sizeof(Header) + align16(size);
    m_batch_used = 0;
    publish();
}

void GsThread::gif(int path, const uint8_t *data, uint32_t bytes)
{
    const uint32_t entry = 16 + static_cast<uint32_t>(align16(bytes));
    if (bytes <= kBatchLimit / 4)
    {
        if (m_batch && (m_batch_path != path || m_batch_used + entry > kBatchLimit))
            flush_batch();
        if (!m_batch)
        {
            m_batch = reserve(kBatchLimit, GifBatch, static_cast<uint64_t>(path));
            batch_start_ = m_pending_write - sizeof(Header) - align16(kBatchLimit);
            m_batch_path = path;
            m_batch_used = 0;
        }
        uint8_t *dst = m_batch + m_batch_used;
        std::memcpy(dst, &bytes, sizeof(bytes));
        std::memcpy(dst + 16, data, bytes);
        m_batch_used += entry;
        if (m_batch_used + 16 + 16 > kBatchLimit)
            flush_batch();
        return;
    }
    flush_batch();
    while (bytes > kMaxChunk)
    {
        uint8_t *dst = reserve(kMaxChunk, Gif, static_cast<uint64_t>(path));
        std::memcpy(dst, data, kMaxChunk);
        publish();
        data += kMaxChunk;
        bytes -= kMaxChunk;
    }
    uint8_t *dst = reserve(bytes, Gif, static_cast<uint64_t>(path));
    std::memcpy(dst, data, bytes);
    publish();
}

void GsThread::vsync(const uint64_t *regs, size_t count, int field)
{
    flush_batch();
    uint8_t *dst = reserve(static_cast<uint32_t>(count * sizeof(uint64_t)), Vsync, static_cast<uint64_t>(field));
    std::memcpy(dst, regs, count * sizeof(uint64_t));
    publish();
    ++m_vsync_queued;
    wait_producer([&] { return m_vsync_queued - m_vsync_done.load(std::memory_order_acquire) <= m_frames_ahead; });
}

void GsThread::reset()
{
    flush_batch();
    reserve(0, Reset, 0);
    publish();
}

void GsThread::resize(int width, int height)
{
    flush_batch();
    reserve(0, Resize, (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height));
    publish();
}

void GsThread::run_sync(const std::function<void()> &fn)
{
    flush_batch();
    SyncJob job;
    job.fn = &fn;
    reserve(0, Sync, reinterpret_cast<uint64_t>(&job));
    publish();
    wait_producer([&] { return job.done.load(std::memory_order_acquire); });
}

void GsThread::post(std::function<void()> fn)
{
    flush_batch();
    auto *copy = new std::function<void()>(std::move(fn));
    reserve(0, Post, reinterpret_cast<uint64_t>(copy));
    publish();
}

// ----- consumer ------------------------------------------------------------------------------------------

void GsThread::notify_producer()
{
    if (m_producer_waiting.load(std::memory_order_seq_cst))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_producer_cv.notify_all();
    }
}

void GsThread::consumer_advance(uint64_t read_pos)
{
    m_read.store(read_pos, std::memory_order_seq_cst);
    if (m_ring_waiting.load(std::memory_order_seq_cst))
        m_read.notify_all();
    notify_producer();
}

void GsThread::thread_main()
{
    name_this_thread();
    // The GS runs with the host's default floating-point mode (as in the reference emulator), not the
    // game's: the thread inherits the creator's mode, so the host's is put back for the thread's life.
    const fp_env::HostCsrScope fp_scope;

    const bool ok = m_cb.open ? m_cb.open() : false;
    m_open_result.store(ok ? 1 : 0, std::memory_order_release);
    if (!ok)
        return;

    uint64_t rp = 0;
    for (;;)
    {
        uint64_t wp = m_write.load(std::memory_order_acquire);
        if (rp == wp)
        {
            int spins = 0;
            while ((wp = m_write.load(std::memory_order_acquire)) == rp && spins++ < 4000)
                DON_CPU_RELAX();
            if (rp == wp)
            {
                m_consumer_sleeping.store(true, std::memory_order_seq_cst);
                if (m_write.load(std::memory_order_seq_cst) == rp)
                    m_write.wait(rp, std::memory_order_acquire); // futex: woken by publish()
                m_consumer_sleeping.store(false, std::memory_order_relaxed);
                continue;
            }
        }

        const uint64_t off = rp & (kRingBytes - 1);
        Header h;
        std::memcpy(&h, m_ring + off, sizeof(h));
        const uint8_t *payload = m_ring + off + sizeof(Header);
        uint64_t total = sizeof(Header) + align16(h.size);
        switch (h.type)
        {
        case Pad:
            total = kRingBytes - off;
            break;
        case Gif:
            m_cb.gif(static_cast<int>(h.a), payload, h.size);
            break;
        case GifBatch:
        {
            for (uint32_t pos = 0; pos < h.size;)
            {
                uint32_t n;
                std::memcpy(&n, payload + pos, sizeof(n));
                m_cb.gif(static_cast<int>(h.a), payload + pos + 16, n);
                pos += 16 + static_cast<uint32_t>(align16(n));
            }
            break;
        }
        case Vsync:
            m_cb.vsync(reinterpret_cast<const uint64_t *>(payload), static_cast<int>(h.a));
            m_vsync_done.fetch_add(1, std::memory_order_release);
            break;
        case Reset:
            m_cb.reset();
            break;
        case Resize:
            m_cb.resize(static_cast<int>(h.a >> 32), static_cast<int>(h.a & 0xFFFFFFFFu));
            break;
        case Sync:
        {
            auto *job = reinterpret_cast<SyncJob *>(h.a);
            (*job->fn)();
            job->done.store(true, std::memory_order_release);
            break;
        }
        case Post:
        {
            auto *fn = reinterpret_cast<std::function<void()> *>(h.a);
            (*fn)();
            delete fn;
            break;
        }
        case Stop:
            consumer_advance(rp + total);
            if (m_cb.close)
                m_cb.close();
            return;
        }
        rp += total;
        consumer_advance(rp);
    }
}
