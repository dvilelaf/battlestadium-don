// How netplay sends and receives datagrams (D14, docs/knowledge/22-netplay.md). This header is the contract:
// udp_socket_posix.cpp / udp_socket_win32.cpp implement open_udp and resolve for real sockets (T-503),
// sim_transport.cpp implements the in-process network the tests use and the latency/loss wrapper (T-503).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace don_net
{

// An IPv4 endpoint, host byte order. IPv6 is not supported in this version.
struct PeerAddr
{
    uint32_t ipv4 = 0;
    uint16_t port = 0;

    bool operator==(const PeerAddr &o) const { return ipv4 == o.ipv4 && port == o.port; }
    bool operator!=(const PeerAddr &o) const { return !(*this == o); }
    // "a.b.c.d:port"
    std::string text() const
    {
        return std::to_string((ipv4 >> 24) & 0xFF) + "." + std::to_string((ipv4 >> 16) & 0xFF) + "." +
               std::to_string((ipv4 >> 8) & 0xFF) + "." + std::to_string(ipv4 & 0xFF) + ":" + std::to_string(port);
    }
};

// The largest datagram netplay ever sends (fits every path MTU in practice).
constexpr std::size_t kMaxDatagram = 1200;

class Transport
{
public:
    virtual ~Transport() = default;
    // Sends one datagram (size <= kMaxDatagram). False if the system refused it; a lost datagram is not an error.
    virtual bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) = 0;
    // Waits up to timeout_ms (0 = do not wait) for one datagram. False when none arrived in time.
    virtual bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) = 0;
    // The local port this transport is bound to (after open_udp(0) the system's choice).
    virtual uint16_t local_port() const = 0;
};

// A UDP socket bound to port on every IPv4 interface (0 = any free port). Null on failure, with error set.
std::unique_ptr<Transport> open_udp(uint16_t port, std::string &error);

// "host:port" (a name or a dotted IPv4 address) to an address. False with error set when it cannot be resolved.
bool resolve(const std::string &host_port, PeerAddr &out, std::string &error);

// This PC's LAN IPv4 address ("192.168.x.y"), found with the routing trick: a UDP socket connect()ed to a
// public address (1.1.1.1:53, nothing sent) and then getsockname(), which picks the default-route interface
// and not the docker/vpn bridges. Empty when there is no such address. Used by the host's lobby when UPnP
// gives no local address (the router does not answer SSDP).
std::string lan_ipv4();

// Wraps a transport: each datagram sent is delayed by latency_ms (+ up to jitter_ms) and dropped with
// probability loss (0..1), from a pseudo-random generator seeded with seed. Used by the tests and, in a real
// run, when DON_NET_SIM_LATENCY_MS / DON_NET_SIM_JITTER_MS / DON_NET_SIM_LOSS are set.
std::unique_ptr<Transport> with_simulated_network(std::unique_ptr<Transport> inner, int latency_ms, int jitter_ms,
                                                  double loss, uint64_t seed);

// An in-process network for tests: endpoints made by the same hub exchange datagrams through memory.
class LoopbackHub
{
public:
    LoopbackHub();
    ~LoopbackHub();
    // A new endpoint at 127.0.0.1:port (port must be unused on this hub).
    std::unique_ptr<Transport> endpoint(uint16_t port);

    struct State;

private:
    std::shared_ptr<State> m_state;
};

} // namespace don_net
