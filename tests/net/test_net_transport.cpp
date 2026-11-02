// Transports (D14, knowledge 22 section 5): real UDP sockets, the in-process LoopbackHub, and the simulated
// network wrapper (latency, jitter and loss).
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "transport.h"

using namespace don_net;

namespace
{

constexpr uint32_t kLoopbackIp = 0x7F000001u; // 127.0.0.1

double ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Two open_udp(0) sockets on 127.0.0.1 exchange datagrams of many sizes; the sender address is reported correctly.
void test_udp_exchange()
{
    std::string error;
    auto a = open_udp(0, error);
    auto b = open_udp(0, error);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (!a || !b)
        return;
    const PeerAddr to_b{kLoopbackIp, b->local_port()};
    const PeerAddr to_a{kLoopbackIp, a->local_port()};

    for (int i = 0; i < 100; ++i)
    {
        const std::size_t size = 1 + static_cast<std::size_t>(i) * 11; // 1 .. 1090, all <= kMaxDatagram
        std::vector<uint8_t> payload(size);
        for (std::size_t j = 0; j < size; ++j)
            payload[j] = static_cast<uint8_t>(i + j);

        CHECK(a->send(to_b, payload.data(), payload.size()));
        PeerAddr from;
        std::vector<uint8_t> got;
        CHECK(b->receive(from, got, 2000));
        CHECK_EQ(got.size(), size);
        CHECK(got == payload);
        CHECK(from.ipv4 == kLoopbackIp);
        CHECK_EQ(from.port, a->local_port());

        // And back the other way.
        CHECK(b->send(to_a, payload.data(), payload.size()));
        CHECK(a->receive(from, got, 2000));
        CHECK_EQ(got.size(), size);
        CHECK(got == payload);
        CHECK_EQ(from.port, b->local_port());
    }
}

// A receive with a 50 ms timeout and nothing arriving returns false after roughly 50 ms (within 40..200).
void test_udp_timeout()
{
    std::string error;
    auto a = open_udp(0, error);
    CHECK(a != nullptr);
    if (!a)
        return;
    PeerAddr from;
    std::vector<uint8_t> data;
    const auto start = std::chrono::steady_clock::now();
    CHECK(!a->receive(from, data, 50));
    const double ms = ms_since(start);
    CHECK(ms >= 40.0 && ms <= 200.0);
}

void test_resolve()
{
    PeerAddr out;
    std::string error;

    CHECK(resolve("127.0.0.1:7877", out, error));
    CHECK(out.ipv4 == kLoopbackIp);
    CHECK_EQ(out.port, 7877);
    CHECK(out.text() == "127.0.0.1:7877");

    CHECK(resolve("localhost:7877", out, error));
    CHECK(out.ipv4 == kLoopbackIp);
    CHECK_EQ(out.port, 7877);

    // Without ":port" the join default 7877 applies (section 7.1: --net-join HOST).
    CHECK(resolve("127.0.0.1", out, error));
    CHECK(out.ipv4 == kLoopbackIp);
    CHECK_EQ(out.port, 7877);
    CHECK(out.text() == "127.0.0.1:7877");

    CHECK(resolve("localhost", out, error));
    CHECK(out.ipv4 == kLoopbackIp);
    CHECK_EQ(out.port, 7877);

    CHECK(!resolve("x:0", out, error));
    CHECK(!resolve("x:70000", out, error));
    CHECK(!resolve("", out, error));
    CHECK(!resolve("no-such-host.invalid", out, error)); // RFC 6761: .invalid never resolves
    CHECK(!resolve("no-such-host.invalid:7877", out, error));
    CHECK(!resolve(":7877", out, error));
    CHECK(!resolve("127.0.0.1:", out, error));
    CHECK(!resolve("127.0.0.1:abc", out, error));
}

// A hub with 4 endpoints delivers to the right one and nowhere else.
void test_hub_delivery()
{
    LoopbackHub hub;
    auto e1 = hub.endpoint(1001);
    auto e2 = hub.endpoint(1002);
    auto e3 = hub.endpoint(1003);
    auto e4 = hub.endpoint(1004);
    CHECK(e1 && e2 && e3 && e4);
    CHECK(hub.endpoint(1001) == nullptr); // port already used
    CHECK(hub.endpoint(0) == nullptr);    // port 0 rejected
    if (!e1 || !e2 || !e3 || !e4)
        return;

    const uint8_t payload[] = {'h', 'i'};
    CHECK(e1->send({kLoopbackIp, 1003}, payload, sizeof(payload)));

    PeerAddr from;
    std::vector<uint8_t> got;
    CHECK(e3->receive(from, got, 2000));
    CHECK_EQ(got.size(), sizeof(payload));
    CHECK(std::memcmp(got.data(), payload, sizeof(payload)) == 0);
    CHECK_EQ(from.port, 1001);

    // The others receive nothing.
    CHECK(!e2->receive(from, got, 0));
    CHECK(!e4->receive(from, got, 0));
    CHECK(!e1->receive(from, got, 0));

    // Sending to a port with no endpoint is a silent loss, not an error (as on UDP).
    CHECK(e1->send({kLoopbackIp, 9999}, payload, sizeof(payload)));
}

// with_simulated_network(latency 30, no jitter, no loss) delays by 30 ms (+-10).
void test_sim_latency()
{
    LoopbackHub hub;
    auto inner = hub.endpoint(2001);
    auto peer = hub.endpoint(2002);
    CHECK(inner && peer);
    if (!inner || !peer)
        return;
    auto sim = with_simulated_network(std::move(inner), 30, 0, 0.0, 1);

    const uint8_t payload[] = {1, 2, 3};
    const auto start = std::chrono::steady_clock::now();
    CHECK(sim->send({kLoopbackIp, 2002}, payload, sizeof(payload)));

    // The simulated transport holds the datagram for 30 ms and only releases it when the caller keeps pumping it
    // (through send/receive, as the netplay coordinator's loop does). Pump until the peer has it.
    PeerAddr from;
    std::vector<uint8_t> got;
    double ms = -1.0;
    while (ms < 0.0 && ms_since(start) <= 1000.0)
    {
        if (peer->receive(from, got, 0))
        {
            ms = ms_since(start);
            break;
        }
        PeerAddr dummy;
        std::vector<uint8_t> ignored;
        sim->receive(dummy, ignored, 0); // releases the datagram once 30 ms have passed
    }
    CHECK(ms >= 20.0 && ms <= 40.0);
    CHECK_EQ(got.size(), sizeof(payload));
    CHECK(std::memcmp(got.data(), payload, sizeof(payload)) == 0);
}

// with_simulated_network(loss 0.5) over 1000 datagrams drops 400..600, reproducibly for a given seed.
std::size_t count_losses(uint64_t seed)
{
    LoopbackHub hub;
    auto inner = hub.endpoint(3001);
    auto peer = hub.endpoint(3002);
    auto sim = with_simulated_network(std::move(inner), 0, 0, 0.5, seed);

    const uint8_t payload = 0xAB;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i)
        CHECK(sim->send({kLoopbackIp, 3002}, &payload, 1));

    std::size_t arrived = 0;
    PeerAddr from;
    std::vector<uint8_t> got;
    while (peer->receive(from, got, 0))
        ++arrived;
    return arrived;
}

void test_sim_loss()
{
    const std::size_t first = count_losses(42);
    CHECK(first >= 400 && first <= 600);

    // The same seed gives the same losses; a different seed is independent.
    CHECK_EQ(count_losses(42), first);
    CHECK_EQ(count_losses(42), first);
}

} // namespace

int main()
{
    test_udp_exchange();
    test_udp_timeout();
    test_resolve();
    test_hub_delivery();
    test_sim_latency();
    test_sim_loss();
    return check_result("test_net_transport");
}
