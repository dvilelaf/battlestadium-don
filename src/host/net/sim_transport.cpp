// The simulated network (D14, docs/knowledge/22-netplay.md section 5): with_simulated_network delays and drops the
// datagrams of a real transport; LoopbackHub is an in-process network for the tests. No operating system calls here.
#include "transport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <random>

namespace don_net
{

struct LoopbackHub::State
{
    struct Datagram
    {
        PeerAddr from;
        std::vector<uint8_t> data;
    };

    std::mutex mutex;
    std::condition_variable arrived;
    std::map<uint16_t, std::deque<Datagram>> queues; // one queue per endpoint port
};

namespace
{

using Clock = std::chrono::steady_clock;

// The address every LoopbackHub endpoint has (127.0.0.1).
constexpr uint32_t kLoopbackIp = 0x7F000001u;

class SimTransport final : public Transport
{
public:
    SimTransport(std::unique_ptr<Transport> inner, int latency_ms, int jitter_ms, double loss, uint64_t seed)
        : m_inner(std::move(inner)), m_latency_ms(std::max(0, latency_ms)), m_jitter_ms(std::max(0, jitter_ms)),
          m_loss(std::clamp(loss, 0.0, 1.0)), m_rng(seed)
    {
    }

    // Each datagram is dropped with probability loss, else queued for latency + up to jitter milliseconds.
    // A dropped datagram is not an error (returns true). A datagram with no delay goes out at once.
    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        const double roll = std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
        if (roll < m_loss)
            return release_due();

        int delay_ms = m_latency_ms;
        if (m_jitter_ms > 0)
            delay_ms += std::uniform_int_distribution<int>(0, m_jitter_ms)(m_rng);
        m_queue.push_back(Pending{Clock::now() + std::chrono::milliseconds(delay_ms), to,
                                  std::vector<uint8_t>(data, data + size)});
        return release_due();
    }

    // Releases the due datagrams first. Waits no longer than the next release, so a datagram held here goes out on
    // time even while the caller waits for one.
    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
        for (;;)
        {
            release_due();
            Clock::time_point until = deadline;
            for (const Pending &p : m_queue)
                until = std::min(until, p.release);
            const Clock::duration left = until - Clock::now();
            const int wait_ms =
                left <= Clock::duration::zero() ? 0 : static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());

            if (m_inner->receive(from, data, wait_ms))
                return true;
            if (Clock::now() >= deadline)
                return false;
        }
    }

    uint16_t local_port() const override { return m_inner->local_port(); }

private:
    struct Pending
    {
        Clock::time_point release;
        PeerAddr to;
        std::vector<uint8_t> data;
    };

    // Sends the queued datagrams that are due, in queue order. False if the inner transport refused one of them.
    bool release_due()
    {
        bool ok = true;
        const Clock::time_point now = Clock::now();
        for (auto it = m_queue.begin(); it != m_queue.end();)
        {
            if (it->release <= now)
            {
                ok = m_inner->send(it->to, it->data.data(), it->data.size()) && ok;
                it = m_queue.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return ok;
    }

    std::unique_ptr<Transport> m_inner;
    int m_latency_ms;
    int m_jitter_ms;
    double m_loss;
    std::mt19937_64 m_rng;
    std::deque<Pending> m_queue;
};

class LoopbackEndpoint final : public Transport
{
public:
    LoopbackEndpoint(std::shared_ptr<LoopbackHub::State> state, uint16_t port) : m_state(std::move(state)), m_port(port)
    {
    }

    ~LoopbackEndpoint() override
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->queues.erase(m_port);
    }

    // Delivers to the endpoint at to.port on 127.0.0.1. Nothing listening there: the datagram is lost, as on UDP.
    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        if (to.ipv4 != kLoopbackIp)
            return false;
        std::lock_guard<std::mutex> lock(m_state->mutex);
        const auto it = m_state->queues.find(to.port);
        if (it == m_state->queues.end())
            return true;
        it->second.push_back(LoopbackHub::State::Datagram{PeerAddr{kLoopbackIp, m_port},
                                                          std::vector<uint8_t>(data, data + size)});
        m_state->arrived.notify_all();
        return true;
    }

    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        // The endpoint's own destructor removed the queue (or another endpoint took the port): no datagrams.
        // at() would throw here, which the netplay loop cannot handle.
        const auto it = m_state->queues.find(m_port);
        if (it == m_state->queues.end())
            return false;
        auto &queue = it->second;
        if (queue.empty() && timeout_ms > 0)
            m_state->arrived.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&queue] { return !queue.empty(); });
        if (queue.empty())
            return false;
        from = queue.front().from;
        data = std::move(queue.front().data);
        queue.pop_front();
        return true;
    }

    uint16_t local_port() const override { return m_port; }

private:
    std::shared_ptr<LoopbackHub::State> m_state;
    uint16_t m_port;
};

} // namespace

std::unique_ptr<Transport> with_simulated_network(std::unique_ptr<Transport> inner, int latency_ms, int jitter_ms,
                                                  double loss, uint64_t seed)
{
    if (!inner)
        return nullptr;
    return std::make_unique<SimTransport>(std::move(inner), latency_ms, jitter_ms, loss, seed);
}

LoopbackHub::LoopbackHub() : m_state(std::make_shared<State>()) {}

LoopbackHub::~LoopbackHub() = default;

// Returns null for port 0 and for a port that another endpoint of this hub already uses.
std::unique_ptr<Transport> LoopbackHub::endpoint(uint16_t port)
{
    if (port == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_state->mutex);
    if (!m_state->queues.emplace(port, std::deque<State::Datagram>{}).second)
        return nullptr;
    return std::make_unique<LoopbackEndpoint>(m_state, port);
}

} // namespace don_net
