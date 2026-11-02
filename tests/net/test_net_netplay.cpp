// netplay.h: the Netplay coordinator (D14, knowledge 22 section 6): the lobby of section 6.0, the per-frame step,
// the hashes, over LoopbackHub / with_simulated_network, plus fake transports for the address and BYE checks.
#include "check.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "netplay.h"
#include "protocol.h"

using namespace don_net;

using Clock = std::chrono::steady_clock;
static int ms_since(Clock::time_point t0)
{
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}
static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A deterministic pad for player p at frame f, distinct across (p, f).
static NetPad pat(int p, uint32_t f)
{
    NetPad n = neutral_pad();
    n.buttons = static_cast<uint16_t>(0x8000u ^ ((p * 0x101 + f * 0x37) & 0xFFFFu));
    n.lx = static_cast<uint8_t>((f + p) & 0xFF);
    n.ly = static_cast<uint8_t>((f * 3 + p * 5) & 0xFF);
    n.connected = 1;
    return n;
}

static uint64_t same_hash(uint32_t f) { return f * 0x9E3779B97F4A7C15ull; }
static uint64_t diff_hash(uint32_t f) { return same_hash(f) + 1; }

// A transport wrapper that records the messages it sends and receives (for the BYE and WAITING checks).
class SpyTransport : public Transport
{
public:
    explicit SpyTransport(std::unique_ptr<Transport> inner) : m_inner(std::move(inner)) {}
    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        Message m;
        if (decode(data, size, m))
            sent.push_back(m);
        return m_inner->send(to, data, size);
    }
    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        if (!m_inner->receive(from, data, timeout_ms))
            return false;
        Message m;
        if (decode(data.data(), data.size(), m))
            got.push_back(m);
        return true;
    }
    uint16_t local_port() const override { return m_inner->local_port(); }
    // Receives whatever the peer left queued, so a test can inspect datagrams without a running thread.
    void drain()
    {
        PeerAddr from;
        std::vector<uint8_t> data;
        while (m_inner->receive(from, data, 0))
        {
            Message m;
            if (decode(data.data(), data.size(), m))
                got.push_back(m);
        }
    }
    std::vector<Message> sent, got;

private:
    std::unique_ptr<Transport> m_inner;
};

// A transport that silently drops every outgoing HASH (everything else passes through). Used to prove that a HASH
// that never reaches the host is not fatal: the session must run on without it.
class DropHashTransport : public Transport
{
public:
    explicit DropHashTransport(std::unique_ptr<Transport> inner) : m_inner(std::move(inner)) {}
    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        Message m;
        if (decode(data, size, m) && static_cast<MsgType>(m.body.index() + 1) == MsgType::Hash)
        {
            ++dropped;
            return true;
        }
        return m_inner->send(to, data, size);
    }
    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        return m_inner->receive(from, data, timeout_ms);
    }
    uint16_t local_port() const override { return m_inner->local_port(); }

    int dropped = 0;

private:
    std::unique_ptr<Transport> m_inner;
};

// A transport that sends nowhere and hands out datagrams the test queues (the address check of the client).
class FeedTransport : public Transport
{
public:
    bool send(const PeerAddr &, const uint8_t *, std::size_t) override { return true; }
    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int) override
    {
        if (m_queue.empty())
            return false;
        from = m_queue.front().first;
        data = m_queue.front().second;
        m_queue.pop_front();
        return true;
    }
    uint16_t local_port() const override { return 0; }
    void push(const PeerAddr &from, const Message &m)
    {
        auto bytes = encode(m);
        if (!bytes.empty())
            m_queue.emplace_back(from, std::move(bytes));
    }

private:
    std::deque<std::pair<PeerAddr, std::vector<uint8_t>>> m_queue;
};

struct PeerOutcome
{
    bool connected = false;
    bool ok = false;
    std::string error;
    NetSession session;
    std::vector<std::vector<NetPad>> outs; // outs[frame-1][player]
};

static void run_peer(Netplay &np, int frames, int hash_every, uint64_t (*hashfn)(uint32_t), PeerOutcome &o)
{
    std::string err;
    const auto stop = [] { return false; };
    if (!np.connect(o.session, err, stop))
    {
        o.connected = false;
        o.ok = false;
        o.error = err;
        return;
    }
    o.connected = true;
    const int players = o.session.players;
    const int lp = o.session.local_player;
    for (uint32_t f = 1; f <= static_cast<uint32_t>(frames); ++f)
    {
        NetPad local = pat(lp, f);
        std::vector<NetPad> out(players);
        if (!np.step(f, local, out.data(), err, stop))
        {
            o.ok = false;
            o.error = "step " + std::to_string(f) + ": " + err;
            return;
        }
        o.outs.push_back(out);
        if (hash_every > 0 && f % static_cast<uint32_t>(hash_every) == 0)
        {
            if (!np.report_hash(f, hashfn(f), err))
            {
                o.ok = false;
                o.error = "hash " + std::to_string(f) + ": " + err;
                return;
            }
        }
    }
    o.ok = true;
    np.close(0); // kByeQuit: a normal quit, so peers hear BYE and can finish
}

// Every peer's out set at every frame equals pat(p, f-delay) (or neutral for f <= delay).
static bool check_outs(const std::vector<PeerOutcome> &peers, int delay, int players, uint32_t frames)
{
    if (peers.size() != static_cast<std::size_t>(players))
        return false;
    for (uint32_t f = 1; f <= frames; ++f)
    {
        for (int p = 0; p < players; ++p)
        {
            const NetPad expected = (f <= static_cast<uint32_t>(delay)) ? neutral_pad() : pat(p, f - delay);
            for (const PeerOutcome &peer : peers)
            {
                if (peer.outs.size() < f || peer.outs[f - 1][p] != expected)
                    return false;
            }
        }
    }
    return true;
}

static NetConfig host_cfg(int players, int delay, int hash_every, const std::string &build, uint64_t hash,
                          int abort_ms)
{
    NetConfig c;
    c.host = true;
    c.players = players;
    c.delay = delay;
    c.hash_every = hash_every;
    c.build_id = build;
    c.data_hash = hash;
    c.abort_after_ms = abort_ms;
    return c;
}

static NetConfig client_cfg(const std::string &join, const std::string &build, uint64_t hash, int abort_ms)
{
    NetConfig c;
    c.host = false;
    c.join = join;
    c.build_id = build;
    c.data_hash = hash;
    c.abort_after_ms = abort_ms;
    return c;
}

static void test_loopback_2()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 120, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 600, 120, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 600, 120, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    if (!co.ok)
        std::fprintf(stderr, "client error: %s\n", co.error.c_str());
    CHECK_EQ(ho.session.players, 2);
    CHECK_EQ(ho.session.delay, 3);
    CHECK_EQ(co.session.delay, 3);
    CHECK(check_outs({ho, co}, 3, 2, 600));
}

static void test_loopback_4()
{
    LoopbackHub hub;
    Netplay host(host_cfg(4, 2, 120, "build", 42, 30000), hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));
    Netplay c2(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(30000));
    Netplay c3(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(40000));

    PeerOutcome ho, o1, o2, o3;
    std::thread th(run_peer, std::ref(host), 600, 120, same_hash, std::ref(ho));
    std::thread t1(run_peer, std::ref(c1), 600, 120, same_hash, std::ref(o1));
    std::thread t2(run_peer, std::ref(c2), 600, 120, same_hash, std::ref(o2));
    std::thread t3(run_peer, std::ref(c3), 600, 120, same_hash, std::ref(o3));
    th.join();
    t1.join();
    t2.join();
    t3.join();

    CHECK(ho.connected && ho.ok);
    CHECK(o1.connected && o1.ok);
    CHECK(o2.connected && o2.ok);
    CHECK(o3.connected && o3.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    CHECK_EQ(ho.session.players, 4);
    CHECK_EQ(ho.session.delay, 2);
    CHECK_EQ(o1.session.delay, 2);
    CHECK_EQ(o2.session.delay, 2);
    CHECK_EQ(o3.session.delay, 2);
    CHECK(check_outs({ho, o1, o2, o3}, 2, 4, 600));
}

// The memory of Lockstep is bounded: 1300 frames is well past the 600-frame window of section 4, and the game
// still runs and produces the right pads at the end.
static void test_long_session()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 1300, 0, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 1300, 0, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    if (!co.ok)
        std::fprintf(stderr, "client error: %s\n", co.error.c_str());
    CHECK(check_outs({ho, co}, 3, 2, 1300));
}

static void test_simulated_network()
{
    LoopbackHub hub;
    auto ht = with_simulated_network(hub.endpoint(10000), 40, 10, 0.05, 12345);
    auto ct = with_simulated_network(hub.endpoint(20000), 40, 10, 0.05, 54321);
    Netplay host(host_cfg(2, 0, 0, "build", 42, 30000), std::move(ht));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), std::move(ct));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 600, 0, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 600, 0, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    if (!co.ok)
        std::fprintf(stderr, "client error: %s\n", co.error.c_str());
    CHECK(ho.session.delay >= 1 && ho.session.delay <= 10); // auto delay
    CHECK_EQ(ho.session.delay, co.session.delay);
    CHECK(check_outs({ho, co}, ho.session.delay, 2, 600));
}

// Loss with hashes on (journal 040): the client repeats its HASH with its INPUTs, so 5% loss over 1200 frames must
// neither stall the session (the old host waited abort_after_ms for the one HASH and died) nor report a false desync.
static void test_simulated_network_hashes_4p()
{
    LoopbackHub hub;
    Netplay host(host_cfg(4, 2, 30, "build", 42, 30000),
                 with_simulated_network(hub.endpoint(10000), 5, 2, 0.05, 777));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(20000), 5, 2, 0.05, 778));
    Netplay c2(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(30000), 5, 2, 0.05, 779));
    Netplay c3(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(40000), 5, 2, 0.05, 780));

    PeerOutcome ho, o1, o2, o3;
    std::thread th(run_peer, std::ref(host), 1200, 30, same_hash, std::ref(ho));
    std::thread t1(run_peer, std::ref(c1), 1200, 30, same_hash, std::ref(o1));
    std::thread t2(run_peer, std::ref(c2), 1200, 30, same_hash, std::ref(o2));
    std::thread t3(run_peer, std::ref(c3), 1200, 30, same_hash, std::ref(o3));
    th.join();
    t1.join();
    t2.join();
    t3.join();

    CHECK(ho.connected && ho.ok);
    CHECK(o1.connected && o1.ok);
    CHECK(o2.connected && o2.ok);
    CHECK(o3.connected && o3.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    if (!o1.ok)
        std::fprintf(stderr, "c1 error: %s\n", o1.error.c_str());
    if (!o2.ok)
        std::fprintf(stderr, "c2 error: %s\n", o2.error.c_str());
    if (!o3.ok)
        std::fprintf(stderr, "c3 error: %s\n", o3.error.c_str());
    CHECK_EQ(ho.session.players, 4);
    CHECK(check_outs({ho, o1, o2, o3}, 2, 4, 1200));
}

// A real mismatch under loss is still found on every peer: the bad client's HASH is repeated until the host sees it
// and repeats its DESYNC until each client hears it.
static void test_simulated_desync_4p()
{
    LoopbackHub hub;
    Netplay host(host_cfg(4, 2, 30, "build", 42, 30000),
                 with_simulated_network(hub.endpoint(10000), 5, 2, 0.05, 811));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(20000), 5, 2, 0.05, 812));
    Netplay c2(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(30000), 5, 2, 0.05, 813));
    Netplay c3(client_cfg("127.0.0.1:10000", "build", 42, 30000),
               with_simulated_network(hub.endpoint(40000), 5, 2, 0.05, 814));

    PeerOutcome ho, o1, o2, o3;
    std::thread th(run_peer, std::ref(host), 400, 30, same_hash, std::ref(ho));
    std::thread t1(run_peer, std::ref(c1), 400, 30, same_hash, std::ref(o1));
    std::thread t2(run_peer, std::ref(c2), 400, 30, diff_hash, std::ref(o2));
    std::thread t3(run_peer, std::ref(c3), 400, 30, same_hash, std::ref(o3));
    th.join();
    t1.join();
    t2.join();
    t3.join();

    CHECK(ho.connected && !ho.ok && ho.error.find("desync") != std::string::npos);
    CHECK(o1.connected && !o1.ok && o1.error.find("desync") != std::string::npos);
    CHECK(o2.connected && !o2.ok && o2.error.find("desync") != std::string::npos);
    CHECK(o3.connected && !o3.ok && o3.error.find("desync") != std::string::npos);
    if (ho.ok)
        std::fprintf(stderr, "host did not see the mismatch: %s\n", ho.error.c_str());
}

// A HASH that never reaches the host is not fatal: the host logs it (on expiry) and the session runs on; the client
// still resends it (here all of them are dropped, so the host's side alone is proven).
static void test_lost_hash_is_not_fatal()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 30, "build", 42, 30000), hub.endpoint(10000));
    auto drop = std::make_unique<DropHashTransport>(hub.endpoint(20000));
    DropHashTransport *cap = drop.get();
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), std::move(drop));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 600, 30, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 600, 30, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
    if (!ho.ok)
        std::fprintf(stderr, "host error: %s\n", ho.error.c_str());
    if (!co.ok)
        std::fprintf(stderr, "client error: %s\n", co.error.c_str());
    CHECK(cap->dropped >= 20); // one (or more, via the resends) HASH per round, 600/30 = 20 rounds
    CHECK(check_outs({ho, co}, 3, 2, 600));
}

static void test_auto_delay()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 0, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 50, 0, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 50, 0, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
    CHECK(ho.session.delay >= 1 && ho.session.delay <= 10);
    CHECK_EQ(ho.session.delay, co.session.delay);
    CHECK(check_outs({ho, co}, ho.session.delay, 2, 50));
}

// Section 6.1: the 5 PINGs of the measurement are 100 ms apart, so the connect cannot finish before ~400 ms.
static void test_auto_delay_spacing()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 0, 0, "build", 42, 10000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 10000), hub.endpoint(20000));

    std::string ce;
    NetSession cs;
    const auto c0 = Clock::now();
    std::thread tc([&] { client.connect(cs, ce, [&] { return ms_since(c0) > 2000; }); });

    std::string he;
    NetSession hs;
    const auto t0 = Clock::now();
    const bool ok = host.connect(hs, he, [&] { return ms_since(t0) > 3000; });
    const int took = ms_since(t0);
    tc.join();

    CHECK(ok);
    CHECK(took >= 300); // 5 pings 100 ms apart: the last goes out around 400 ms
    CHECK(hs.delay >= 1 && hs.delay <= 10);
}

// The measurement honours should_stop: a stop request in the middle of it returns at once, not after the 5 pings.
static void test_auto_delay_stop()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 0, 0, "build", 42, 5000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 5000), hub.endpoint(20000));

    std::string ce;
    NetSession cs;
    const auto c0 = Clock::now();
    std::thread tc([&] { client.connect(cs, ce, [&] { return ms_since(c0) > 800; }); });

    std::string he;
    NetSession hs;
    const auto t0 = Clock::now();
    const bool ok = host.connect(hs, he, [&] { return ms_since(t0) > 60; });
    const int took = ms_since(t0);
    tc.join();

    CHECK(!ok);
    CHECK(he.find("stopped") != std::string::npos);
    CHECK(took < 400); // well before the 5 pings would have finished
}

static void test_reject_build()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 5000), hub.endpoint(10000));
    // Client with a different build id.
    Netplay bad(client_cfg("127.0.0.1:10000", "other", 42, 5000), hub.endpoint(20000));

    PeerOutcome bo, ho, go;
    std::thread tb(run_peer, std::ref(bad), 10, 0, same_hash, std::ref(bo));
    // The host waits for one good client; a good one arrives too.
    std::thread th(run_peer, std::ref(host), 10, 0, same_hash, std::ref(ho));
    nap(50);
    Netplay good(client_cfg("127.0.0.1:10000", "build", 42, 5000), hub.endpoint(30000));
    std::thread tg(run_peer, std::ref(good), 10, 0, same_hash, std::ref(go));

    tb.join();
    th.join();
    tg.join();

    CHECK(!bo.connected);
    CHECK(bo.error.find("build") != std::string::npos);
    CHECK(ho.connected && ho.ok);
    CHECK(go.connected && go.ok);
}

static void test_desync()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 10, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 20, 10, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 20, 10, diff_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected);
    CHECK(co.connected);
    CHECK(!ho.ok && ho.error.find("desync") != std::string::npos);
    CHECK(!co.ok && co.error.find("desync") != std::string::npos);
    // The client keeps the DESYNC's frame, so its error names it (section 7.2: "… at frame N").
    CHECK(co.error.find("frame 10") != std::string::npos);
}

static void test_equal_hashes()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 10, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    PeerOutcome ho, co;
    std::thread th(run_peer, std::ref(host), 25, 10, same_hash, std::ref(ho));
    std::thread tc(run_peer, std::ref(client), 25, 10, same_hash, std::ref(co));
    th.join();
    tc.join();

    CHECK(ho.connected && ho.ok);
    CHECK(co.connected && co.ok);
}

// report_hash must not ignore a stop request (it used to block up to abort_after_ms).
static void test_report_hash_stop()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    std::string ce;
    NetSession cs;
    std::thread tc([&] { client.connect(cs, ce, [] { return false; }); });

    std::string he;
    NetSession hs;
    const bool connected = host.connect(hs, he, [] { return false; });
    tc.join();
    CHECK(connected);

    std::string err;
    const auto t0 = Clock::now();
    const bool ok = host.report_hash(10, same_hash(10), err, [] { return true; });
    CHECK(!ok);
    CHECK(err == "stopped");
    CHECK(ms_since(t0) < 300); // it did not wait for the client's HASH
}

// The silent client also proves the WAITING message: the host tells the clients which player is late.
static void test_silent_client()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 1000), hub.endpoint(10000));
    auto spy = std::make_unique<SpyTransport>(hub.endpoint(20000));
    SpyTransport *cap = spy.get();
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 1000), std::move(spy));

    // The client connects, then goes silent (no step, no BYE).
    std::string client_err;
    NetSession cs;
    std::thread tc([&] { client.connect(cs, client_err, [] { return false; }); });

    NetSession hs;
    std::string host_err;
    bool connected = host.connect(hs, host_err, [] { return false; });
    tc.join();

    CHECK(connected);
    CHECK_EQ(hs.players, 2);
    // Steps 1..3 are neutral; step 4 needs the client's input and must time out.
    bool failed = false;
    std::string fail_err;
    for (uint32_t f = 1; f <= 100; ++f)
    {
        NetPad local = pat(0, f);
        NetPad out[4];
        std::string err;
        if (!host.step(f, local, out, err, [] { return false; }))
        {
            failed = true;
            fail_err = err;
            break;
        }
    }
    CHECK(failed);
    CHECK(fail_err.find("stopped sending") != std::string::npos ||
          fail_err.find("timed out") != std::string::npos);

    // WAITING {frame, missing} reached the client with its bit set for player 1.
    cap->drain();
    bool saw_waiting = false;
    for (const Message &m : cap->got)
        if (static_cast<MsgType>(m.body.index() + 1) == MsgType::Waiting)
        {
            const Waiting &w = std::get<Waiting>(m.body);
            if (w.frame == 4 && (w.missing & 0x02) != 0)
                saw_waiting = true;
        }
    CHECK(saw_waiting);
}

// ---- section 6.0: the lobby rules -------------------------------------------------------------

// Both peers polled until the predicate holds or the timeout runs out.
template <typename Pred>
static bool pump(Netplay &host, Netplay &client, NetSession &hs, NetSession &cs, std::string &he, std::string &ce,
                 int timeout_ms, Pred done)
{
    const auto t0 = Clock::now();
    while (ms_since(t0) < timeout_ms)
    {
        host.poll(hs, he);
        client.poll(cs, ce);
        if (done())
            return true;
        nap(1);
    }
    return false;
}

// A HELLO with a full lobby or after request_start is rejected; join and the compacted START are checked.
static void test_lobby_join_and_start()
{
    LoopbackHub hub;
    NetConfig hc = host_cfg(4, 3, 0, "build", 42, 30000);
    hc.name = "the host";
    Netplay host(hc, hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    std::string he, ce;
    CHECK(host.start(he));
    CHECK(c1.start(ce));
    NetSession hs, cs;
    CHECK(pump(host, c1, hs, cs, he, ce, 3000, [&] { return host.connected_players() == 2; }));
    CHECK_EQ(host.connected_players(), 2);
    CHECK(c1.host_name() == "the host");
    // The WELCOME index is exposed before START (the host is 0; the client's provisional index is 1).
    CHECK_EQ(host.welcome_player(), 0);
    CHECK_EQ(c1.welcome_player(), 1);

    const auto hl = host.lobby();
    CHECK_EQ(hl.size(), 2);
    CHECK_EQ(hl[0].player, 0);
    CHECK(hl[0].name == "the host");
    CHECK_EQ(hl[1].player, 1);
    const auto cl = c1.lobby();
    CHECK_EQ(cl.size(), 2);

    // Fewer than 2 connected does nothing (the host alone below); here 2, so Start works.
    host.request_start();
    HandshakeState cst = HandshakeState::Connecting;
    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && cst != HandshakeState::Started)
    {
        host.poll(hs, he);
        cst = c1.poll(cs, ce);
        nap(1);
    }
    CHECK_EQ(cst, HandshakeState::Started);
    CHECK_EQ(host.poll(hs, he), HandshakeState::Started);
    CHECK_EQ(hs.players, 2);
    CHECK_EQ(hs.layout, 0);
    CHECK_EQ(hs.delay, 3);
    CHECK_EQ(hs.local_player, 0);
    CHECK_EQ(cs.players, 2);
    CHECK_EQ(cs.local_player, 1);
    CHECK_EQ(cs.delay, 3);
    host.close(0);
    c1.close(0);
}

static void test_lobby_fewer_than_2_no_start()
{
    LoopbackHub hub;
    Netplay host(host_cfg(4, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    std::string e;
    CHECK(host.start(e));
    NetSession hs;
    for (int i = 0; i < 20; ++i)
    {
        host.poll(hs, e);
        nap(1);
    }
    CHECK_EQ(host.connected_players(), 1);
    CHECK_EQ(host.poll(hs, e), HandshakeState::Lobby);
    CHECK_EQ(host.lobby().size(), 1);

    host.request_start(); // fewer than 2: nothing happens
    CHECK_EQ(host.poll(hs, e), HandshakeState::Lobby);
    CHECK_EQ(host.connected_players(), 1);
    CHECK_EQ(host.lobby().size(), 1);
}

// A client leaving the lobby is removed and the indices are compacted for START.
static void test_lobby_leave_and_compact()
{
    LoopbackHub hub;
    Netplay host(host_cfg(4, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));
    Netplay c2(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(30000));

    std::string he, e1, e2;
    CHECK(host.start(he));
    CHECK(c1.start(e1));
    CHECK(c2.start(e2));
    NetSession hs, s1, s2;
    CHECK(pump(host, c1, hs, s1, he, e1, 3000, [&] { return host.connected_players() == 3; }));
    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && c2.poll(s2, e2) != HandshakeState::Lobby)
        nap(1);
    CHECK_EQ(c2.poll(s2, e2), HandshakeState::Lobby);
    CHECK_EQ(host.lobby().size(), 3);
    CHECK_EQ(host.lobby()[1].player, 1);
    CHECK_EQ(host.lobby()[2].player, 2);

    // The first client leaves: the second is compacted to player 1.
    c1.cancel();
    CHECK(pump(host, c2, hs, s2, he, e2, 3000, [&] { return host.connected_players() == 2; }));
    CHECK_EQ(host.lobby().size(), 2);
    CHECK_EQ(host.lobby()[1].player, 1);

    host.request_start();
    const auto t1 = Clock::now();
    while (ms_since(t1) < 3000 && c2.poll(s2, e2) != HandshakeState::Started)
        nap(1);
    CHECK_EQ(c2.poll(s2, e2), HandshakeState::Started);
    CHECK_EQ(s2.players, 2);
    CHECK_EQ(s2.local_player, 1); // the gap the leaving client left is gone
    host.close(0);
    c2.close(0);
}

// 10 s of silence in the lobby removes a client (the timeout is configurable, so the test is fast).
static void test_lobby_timeout()
{
    LoopbackHub hub;
    NetConfig hc = host_cfg(4, 3, 0, "build", 42, 30000);
    hc.lobby_timeout_ms = 200;
    Netplay host(hc, hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    std::string he, ce;
    CHECK(host.start(he));
    CHECK(c1.start(ce));
    NetSession hs, cs;
    CHECK(pump(host, c1, hs, cs, he, ce, 3000, [&] { return host.connected_players() == 2; }));

    // The client stops polling: it goes silent and is removed after lobby_timeout_ms.
    const auto t0 = Clock::now();
    while (ms_since(t0) < 2000 && host.connected_players() > 1)
    {
        host.poll(hs, he);
        nap(1);
    }
    CHECK_EQ(host.connected_players(), 1);
    CHECK_EQ(host.lobby().size(), 1);
}

// A second client on a full lobby gets REJECT (reason 3).
static void test_lobby_full()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));
    Netplay c2(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(30000));

    std::string he, e1, e2;
    CHECK(host.start(he));
    CHECK(c1.start(e1));
    CHECK(c2.start(e2));
    NetSession hs, s1, s2;
    CHECK(pump(host, c1, hs, s1, he, e1, 3000, [&] { return host.connected_players() == 2; }));
    CHECK_EQ(host.lobby().size(), 2);

    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && c2.poll(s2, e2) != HandshakeState::Failed)
    {
        host.poll(hs, he);
        nap(1);
    }
    CHECK_EQ(c2.poll(s2, e2), HandshakeState::Failed);
    CHECK(e2.find("full") != std::string::npos);
}

// A refused joiner fills the host's notices (section 7.2): the host lobby shows them, and taking them empties
// the queue.
static void test_lobby_refusal_notices()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    NetConfig bc = client_cfg("127.0.0.1:10000", "other", 42, 30000);
    bc.name = "Bob";
    Netplay bad(bc, hub.endpoint(20000));

    std::string he, be;
    CHECK(host.start(he));
    CHECK(bad.start(be));
    NetSession hs, bs;
    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && bad.poll(bs, be) != HandshakeState::Failed)
    {
        host.poll(hs, he);
        nap(1);
    }
    CHECK_EQ(bad.poll(bs, be), HandshakeState::Failed);
    CHECK(be.find("build") != std::string::npos);
    CHECK_EQ(host.connected_players(), 1); // refused: not in the lobby

    const auto notices = host.take_notices();
    CHECK_EQ(notices.size(), 1);
    CHECK(notices[0].find("Refused Bob") != std::string::npos);
    CHECK(notices[0].find("another build") != std::string::npos);
    CHECK(notices[0].find("(other)") != std::string::npos);
    CHECK(host.take_notices().empty()); // taken once, now empty
}

// A HELLO after request_start gets REJECT (reason 4).
static void test_lobby_after_start()
{
    LoopbackHub hub;
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(20000));

    std::string he, e1;
    CHECK(host.start(he));
    CHECK(c1.start(e1));
    NetSession hs, s1;
    CHECK(pump(host, c1, hs, s1, he, e1, 3000, [&] { return host.connected_players() == 2; }));
    host.request_start();
    CHECK_EQ(host.poll(hs, he), HandshakeState::Started);

    Netplay late(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(30000));
    std::string e2;
    CHECK(late.start(e2));
    NetSession s2;
    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && late.poll(s2, e2) != HandshakeState::Failed)
    {
        host.poll(hs, he); // the host keeps listening after START
        nap(1);
    }
    CHECK_EQ(late.poll(s2, e2), HandshakeState::Failed);
    CHECK(e2.find("already started") != std::string::npos);
    host.close(0);
    c1.close(0);
}

// cancel() sends BYE to every peer, both while connecting and from the lobby.
static void test_cancel_sends_bye()
{
    // A client cancelling while connecting sends BYE to the host (it has no WELCOME yet).
    LoopbackHub hub;
    auto spy = std::make_unique<SpyTransport>(hub.endpoint(20000));
    SpyTransport *cap = spy.get();
    Netplay client(client_cfg("127.0.0.1:10000", "build", 42, 30000), std::move(spy));
    std::string e;
    CHECK(client.start(e));
    NetSession cs;
    CHECK_EQ(client.poll(cs, e), HandshakeState::Connecting);
    client.cancel();
    bool saw_bye = false;
    for (const Message &m : cap->sent)
        if (static_cast<MsgType>(m.body.index() + 1) == MsgType::Bye &&
            std::get<Bye>(m.body).reason == kByeQuit)
            saw_bye = true;
    CHECK(saw_bye);

    // The host cancelling tells the client its BYE arrives (the lobby ends with "host left").
    Netplay host(host_cfg(2, 3, 0, "build", 42, 30000), hub.endpoint(10000));
    Netplay c1(client_cfg("127.0.0.1:10000", "build", 42, 30000), hub.endpoint(30000));
    std::string he, e1;
    CHECK(host.start(he));
    CHECK(c1.start(e1));
    NetSession hs, s1;
    CHECK(pump(host, c1, hs, s1, he, e1, 3000, [&] { return host.connected_players() == 2; }));
    host.cancel();
    const auto t0 = Clock::now();
    while (ms_since(t0) < 3000 && c1.poll(s1, e1) != HandshakeState::Failed)
        nap(1);
    CHECK_EQ(c1.poll(s1, e1), HandshakeState::Failed);
    CHECK(e1.find("host left") != std::string::npos);
}

// The client only takes WELCOME/START (and the rest) from the host's address.
static void test_client_ignores_other_sources()
{
    auto ft = std::make_unique<FeedTransport>();
    FeedTransport *feed = ft.get();
    Netplay client(client_cfg("127.0.0.1:5000", "build", 42, 30000), std::move(ft));
    std::string e;
    CHECK(client.start(e));

    PeerAddr host;
    host.ipv4 = 0x7F000001;
    host.port = 5000;
    PeerAddr other;
    other.ipv4 = 0x7F000001;
    other.port = 5999;

    NetSession s;
    CHECK_EQ(client.poll(s, e), HandshakeState::Connecting);

    // A WELCOME from another source with the right shape is ignored.
    feed->push(other, Message{0x1234, Welcome{1, 2, 0, 0, 120, 1, "h"}});
    CHECK_EQ(client.poll(s, e), HandshakeState::Connecting);

    // The host's WELCOME brings it to the lobby.
    feed->push(host, Message{0x1234, Welcome{1, 2, 0, 0, 120, 1, "h"}});
    CHECK_EQ(client.poll(s, e), HandshakeState::Lobby);
    CHECK(client.host_name() == "h");

    // A START with the right session id from another source must not start it.
    feed->push(other, Message{0x1234, Start{1, 2, 0, 3, 1}});
    CHECK_EQ(client.poll(s, e), HandshakeState::Lobby);
    CHECK(!client.status().empty());

    // The host's START does.
    feed->push(host, Message{0x1234, Start{1, 2, 0, 3, 1}});
    CHECK_EQ(client.poll(s, e), HandshakeState::Started);
    CHECK_EQ(s.local_player, 1);
    CHECK_EQ(s.players, 2);
    CHECK_EQ(s.delay, 3);
    client.close(kByeQuit);
}

// A dead domain name: the resolve runs on a background thread, so start() and poll() return at once and Esc
// (cancel()) does not hang (section 7.2). A numeric address still resolves synchronously in start(), which is
// what keeps the tests above deterministic.
static void test_client_resolve_is_nonblocking()
{
    LoopbackHub hub;
    Netplay client(client_cfg("does-not-exist.invalid:10000", "build", 42, 5000), hub.endpoint(20000));

    std::string e;
    const auto t0 = Clock::now();
    CHECK(client.start(e)); // the resolve is on a thread: no block here
    NetSession s;
    const HandshakeState st = client.poll(s, e);
    CHECK(st == HandshakeState::Connecting || st == HandshakeState::Failed);
    CHECK(ms_since(t0) < 2000); // the Connecting screen kept drawing

    client.cancel(); // Esc while resolving (or just after): no hang, back to nothing
    CHECK_EQ(client.poll(s, e), HandshakeState::Failed);
    CHECK(ms_since(t0) < 3000);
}

int main()
{
    test_loopback_2();
    test_loopback_4();
    test_auto_delay();
    test_equal_hashes();
    test_desync();
    test_reject_build();
    test_silent_client();
    test_simulated_network();
    test_simulated_network_hashes_4p();
    test_simulated_desync_4p();
    test_lost_hash_is_not_fatal();
    test_lobby_join_and_start();
    test_lobby_fewer_than_2_no_start();
    test_lobby_leave_and_compact();
    test_lobby_timeout();
    test_lobby_full();
    test_lobby_refusal_notices();
    test_lobby_after_start();
    test_cancel_sends_bye();
    test_client_ignores_other_sources();
    test_client_resolve_is_nonblocking();
    test_report_hash_stop();
    test_auto_delay_spacing();
    test_auto_delay_stop();
    test_long_session();
    return check_result("test_net_netplay");
}
