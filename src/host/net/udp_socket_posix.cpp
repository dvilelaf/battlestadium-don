// UDP sockets for netplay on POSIX systems (D14, docs/knowledge/22-netplay.md section 5): open_udp and resolve.
// The Windows twin is udp_socket_win32.cpp; the build compiles only one of them.
#ifndef _WIN32

#include "transport.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace don_net
{
namespace
{

// Largest datagram the socket can hand over in one receive (the UDP maximum); bigger ones are cut by the kernel.
constexpr std::size_t kReceiveBuffer = 65536;

// The port a join address without ":port" means (docs/knowledge/22-netplay.md 7.1: --net-join HOST uses 7877).
constexpr uint16_t kDefaultJoinPort = 7877;

class PosixUdp final : public Transport
{
public:
    PosixUdp(int fd, uint16_t port) : m_fd(fd), m_port(port), m_buffer(kReceiveBuffer) {}
    ~PosixUdp() override { ::close(m_fd); }

    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(to.ipv4);
        sa.sin_port = htons(to.port);
        const ssize_t sent = ::sendto(m_fd, data, size, 0, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa));
        return sent == static_cast<ssize_t>(size);
    }

    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        pollfd pfd{};
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
        if (ready <= 0 || !(pfd.revents & POLLIN))
            return false;

        sockaddr_in sa{};
        socklen_t length = sizeof(sa);
        const ssize_t n = ::recvfrom(m_fd, m_buffer.data(), m_buffer.size(), 0, reinterpret_cast<sockaddr *>(&sa),
                                     &length);
        if (n < 0)
            return false;
        from.ipv4 = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        data.assign(m_buffer.data(), m_buffer.data() + n);
        return true;
    }

    uint16_t local_port() const override { return m_port; }

private:
    int m_fd;
    uint16_t m_port;
    std::vector<uint8_t> m_buffer;
};

} // namespace

std::unique_ptr<Transport> open_udp(uint16_t port, std::string &error)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        error = std::string("socket: ") + std::strerror(errno);
        return nullptr;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa)) != 0)
    {
        error = "bind port " + std::to_string(port) + ": " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        error = std::string("fcntl O_NONBLOCK: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }

    socklen_t length = sizeof(sa);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &length) != 0)
    {
        error = std::string("getsockname: ") + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<PosixUdp>(fd, ntohs(sa.sin_port));
}

bool resolve(const std::string &host_port, PeerAddr &out, std::string &error)
{
    // "host" or "host:port". Without the colon the join default port applies, so "--net-join HOST" works.
    const std::size_t colon = host_port.rfind(':');
    std::string host;
    uint16_t port = kDefaultJoinPort;
    if (colon == std::string::npos)
    {
        host = host_port;
    }
    else
    {
        if (colon == 0)
        {
            error = "expected host[:port], got '" + host_port + "'";
            return false;
        }
        host = host_port.substr(0, colon);
        const std::string port_text = host_port.substr(colon + 1);
        // Digits only, at most 5 of them, so stoul cannot throw; the range is checked after.
        if (port_text.empty() || port_text.size() > 5 || port_text.find_first_not_of("0123456789") != std::string::npos)
        {
            error = "bad port in '" + host_port + "'";
            return false;
        }
        const unsigned long parsed = std::stoul(port_text);
        if (parsed < 1 || parsed > 65535)
        {
            error = "port out of range 1..65535 in '" + host_port + "'";
            return false;
        }
        port = static_cast<uint16_t>(parsed);
    }
    if (host.empty())
    {
        error = "expected host[:port], got '" + host_port + "'";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (rc != 0 || found == nullptr)
    {
        error = "cannot resolve '" + host + "': " + ::gai_strerror(rc);
        if (found)
            ::freeaddrinfo(found);
        return false;
    }
    const auto *sa = reinterpret_cast<const sockaddr_in *>(found->ai_addr);
    out.ipv4 = ntohl(sa->sin_addr.s_addr);
    out.port = port;
    ::freeaddrinfo(found);
    return true;
}

std::string lan_ipv4()
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return std::string();
    // The routing trick: connect()ing to a public address (nothing is ever sent) makes the kernel pick the
    // default-route interface, so getsockname() returns this PC's LAN address and not a docker bridge's.
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x01010101); // 1.1.1.1
    std::string result;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&dst), sizeof(dst)) == 0)
    {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &length) == 0)
        {
            char text[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
            {
                const uint32_t ip = ntohl(local.sin_addr.s_addr);
                if (ip != 0 && (ip >> 24) != 0x7F) // not 0.0.0.0 (no route) or 127.x (loopback)
                    result = text;
            }
        }
    }
    ::close(fd);
    return result;
}

} // namespace don_net

#endif // !_WIN32
