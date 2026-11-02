// T-641: the HELLO carries the game module's id; the host refuses a joiner whose id differs, with the exact text.
// Equal ids connect, two empty ids (linked builds) match, an empty id against a non-empty one is refused.
#include "check.h"

#include <chrono>
#include <string>
#include <thread>

#include "netplay.h"
#include "protocol.h"
#include "transport.h"

using namespace don_net;

using Clock = std::chrono::steady_clock;
static int ms_since(Clock::time_point t0)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}
static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

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

// Connects one host (two players) and one client with the given module ids. Returns true when both are in.
static bool module_pair_connects(const std::string &host_id, const std::string &client_id, std::string &client_error)
{
    LoopbackHub hub;
    NetConfig hc = host_cfg(2, 3, 0, "build", 42, 5000);
    NetConfig cc = client_cfg("127.0.0.1:10000", "build", 42, 5000);
    hc.module_id = host_id;
    cc.module_id = client_id;
    Netplay host(hc, hub.endpoint(10000));
    Netplay client(cc, hub.endpoint(20000));
    std::string he, ce;
    CHECK(host.start(he));
    CHECK(client.start(ce));
    NetSession hs, cs;
    const auto t0 = Clock::now();
    HandshakeState st = HandshakeState::Connecting;
    while (ms_since(t0) < 3000 && (st = client.poll(cs, ce)) != HandshakeState::Failed && host.connected_players() < 2)
    {
        host.poll(hs, he);
        nap(1);
    }
    client_error = ce;
    return st != HandshakeState::Failed && host.connected_players() == 2;
}

static void test_module_id_handshake()
{
    std::string err;
    CHECK(module_pair_connects("abcdef0123456789", "abcdef0123456789", err));
    CHECK(module_pair_connects("", "", err));
    CHECK(!module_pair_connects("abcdef0123456789", "1111111122222222", err));
    CHECK(err.find("The host's game was prepared differently (module abcdef01).") != std::string::npos);
    CHECK(!module_pair_connects("abcdef0123456789", "", err));
    CHECK(err.find("(module abcdef01).") != std::string::npos);
    CHECK(!module_pair_connects("", "abcdef0123456789", err));
    // A linked host (empty id) names itself as built-in, not as an empty module.
    CHECK(err.find("The host's game was prepared differently (module built-in).") != std::string::npos);
}

int main()
{
    test_module_id_handshake();
    return check_result("test_net_module_id");
}
