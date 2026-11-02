// SPDX-License-Identifier: GPL-3.0-or-later
// The GS on a thread of its own (like PCSX2's MTGS, much smaller): the game thread writes commands into a
// bounded single-producer single-consumer byte ring and the GS thread executes them in order. The GS library
// is opened, used and closed on that thread (it owns the Vulkan device).
//
// What travels through the ring: GIF transfers (copied in), the vsync with its register snapshot (by value),
// reset and resize. Anything that needs an answer (a readback, a screenshot) goes through run_sync(), which
// waits until every earlier command has been executed and then until the given function has run on the GS
// thread. Nothing here decides what the game computes: the order of the commands is the order of the calls.
//
// Back-pressure: the producer waits when the ring is full, and also when more than kMaxFramesAhead vsyncs
// are queued (so a present that blocks still paces the game, and the picture does not lag many frames).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

class GsThread
{
public:
    struct Callbacks
    {
        // All run on the GS thread. `open` returns false if the GS could not be opened (the thread then ends
        // at once); `close` runs after the last command.
        std::function<bool()> open;
        std::function<void()> close;
        std::function<void(int path, const uint8_t *data, uint32_t bytes)> gif;
        std::function<void(const uint64_t *regs, int field)> vsync;
        std::function<void()> reset;
        std::function<void(int width, int height)> resize;
    };

    static constexpr size_t kRingBytes = size_t(32) << 20; // power of two
    static constexpr uint32_t kDefaultFramesAhead = 2;

    explicit GsThread(Callbacks callbacks);
    ~GsThread();
    GsThread(const GsThread &) = delete;
    GsThread &operator=(const GsThread &) = delete;

    // Starts the thread and waits for `open`. Returns its result. Game thread.
    bool start();
    // Executes everything queued, runs `close` on the GS thread and joins it. Game thread. Idempotent.
    void stop();
    bool running() const { return m_running; }

    // Producer side (one thread only).
    void gif(int path, const uint8_t *data, uint32_t bytes);
    void vsync(const uint64_t *regs, size_t count, int field);
    void reset();
    void resize(int width, int height);
    // Waits for the ring to drain, then runs `fn` on the GS thread and waits for it to finish.
    void run_sync(const std::function<void()> &fn);
    // Queues `fn` to run on the GS thread in order, without waiting for it.
    void post(std::function<void()> fn);

private:
    struct Header
    {
        uint32_t type;
        uint32_t size; // payload bytes
        uint64_t a;
    };
    static_assert(sizeof(Header) == 16);
    enum Type : uint32_t
    {
        Pad,
        Gif,
        GifBatch, // payload: consecutive small transfers of one path, each [u32 bytes, pad to 16][data, padded to 16]
        Vsync,
        Reset,
        Resize,
        Sync,
        Post,
        Stop,
    };
    struct SyncJob
    {
        const std::function<void()> *fn;
        std::atomic<bool> done{false};
    };

    void thread_main();
    uint8_t *reserve(uint32_t payload, uint32_t type, uint64_t a); // returns the payload pointer
    void publish();
    void flush_batch(); // publishes the open GifBatch record, if any (before any other command)
    template <typename Ready> void wait_producer(Ready ready); // defined in gs_thread.cpp, used only there
    void consumer_advance(uint64_t read_pos);
    void notify_producer();

    Callbacks m_cb;
    std::thread m_thread;
    bool m_running = false;
    std::atomic<int> m_open_result{-1}; // -1 pending, 0 failed, 1 ok

    uint8_t *m_ring = nullptr;
    alignas(64) std::atomic<uint64_t> m_write{0};
    alignas(64) std::atomic<uint64_t> m_read{0};
    uint8_t *m_batch = nullptr;   // producer: payload of the open GifBatch record (not yet published)
    uint32_t m_batch_used = 0;
    int m_batch_path = 0;
    uint64_t batch_start_ = 0;
    uint64_t m_pending_write = 0; // producer: position of the record being built

    std::atomic<bool> m_consumer_sleeping{false};
    std::atomic<bool> m_producer_waiting{false};
    std::mutex m_mutex;
    std::condition_variable m_producer_cv;

    uint32_t m_frames_ahead = kDefaultFramesAhead; // DON_GS_FRAMES_AHEAD (1..4), read once in start()
    uint64_t m_vsync_queued = 0;             // producer only
    std::atomic<uint64_t> m_vsync_done{0};   // consumer writes
    std::atomic<bool> m_ring_waiting{false}; // producer sleeps on m_read (ring full)
};
