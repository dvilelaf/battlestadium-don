// UDP sockets for netplay on Windows (D14, docs/knowledge/22-netplay.md section 5): open_udp and resolve.
// Same structure as udp_socket_posix.cpp; compiled by the Windows build only (links ws2_32).
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
// WSAPoll (and WSAPOLLFD, POLLRDNORM) exist in winsock2.h only from Windows Vista on. The Windows SDK
// defaults _WIN32_WINNT on its own, but a build that defines it lower would hide them; 0x0601 (Windows 7)
// keeps the socket code below valid. Must come before winsock2.h, which is the first thing that reads it.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "transport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

namespace don_net
{
namespace
{

// Largest datagram the socket can hand over in one receive (the UDP maximum); bigger ones are cut by the system.
constexpr std::size_t kReceiveBuffer = 65536;

// The port a join address without ":port" means (docs/knowledge/22-netplay.md 7.1: --net-join HOST uses 7877).
constexpr uint16_t kDefaultJoinPort = 7877;

// Starts Winsock once for the process; false with error set when it fails.
bool winsock_ready(std::string &error)
{
    static const int status = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (status != 0)
        error = "WSAStartup failed: " + std::to_string(status);
    return status == 0;
}

class WinUdp final : public Transport
{
public:
    WinUdp(SOCKET socket, uint16_t port) : m_socket(socket), m_port(port), m_buffer(kReceiveBuffer) {}
    ~WinUdp() override { closesocket(m_socket); }

    bool send(const PeerAddr &to, const uint8_t *data, std::size_t size) override
    {
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(to.ipv4);
        sa.sin_port = htons(to.port);
        const int sent = sendto(m_socket, reinterpret_cast<const char *>(data), static_cast<int>(size), 0,
                                reinterpret_cast<const sockaddr *>(&sa), sizeof(sa));
        return sent == static_cast<int>(size);
    }

    bool receive(PeerAddr &from, std::vector<uint8_t> &data, int timeout_ms) override
    {
        WSAPOLLFD pfd{};
        pfd.fd = m_socket;
        pfd.events = POLLRDNORM;
        const int ready = WSAPoll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
        if (ready <= 0 || !(pfd.revents & POLLRDNORM))
            return false;

        sockaddr_in sa{};
        int length = sizeof(sa);
        const int n = recvfrom(m_socket, reinterpret_cast<char *>(m_buffer.data()), static_cast<int>(m_buffer.size()),
                               0, reinterpret_cast<sockaddr *>(&sa), &length);
        if (n < 0)
            return false; // includes WSAECONNRESET after a send to a closed port
        from.ipv4 = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        data.assign(m_buffer.data(), m_buffer.data() + n);
        return true;
    }

    uint16_t local_port() const override { return m_port; }

private:
    SOCKET m_socket;
    uint16_t m_port;
    std::vector<uint8_t> m_buffer;
};

} // namespace

std::unique_ptr<Transport> open_udp(uint16_t port, std::string &error)
{
    if (!winsock_ready(error))
        return nullptr;

    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        error = "socket: WSA error " + std::to_string(WSAGetLastError());
        return nullptr;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa)) == SOCKET_ERROR)
    {
        error = "bind port " + std::to_string(port) + ": WSA error " + std::to_string(WSAGetLastError());
        closesocket(sock);
        return nullptr;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonblocking) == SOCKET_ERROR)
    {
        error = "ioctlsocket FIONBIO: WSA error " + std::to_string(WSAGetLastError());
        closesocket(sock);
        return nullptr;
    }

    int length = sizeof(sa);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&sa), &length) == SOCKET_ERROR)
    {
        error = "getsockname: WSA error " + std::to_string(WSAGetLastError());
        closesocket(sock);
        return nullptr;
    }
    return std::make_unique<WinUdp>(sock, ntohs(sa.sin_port));
}

bool resolve(const std::string &host_port, PeerAddr &out, std::string &error)
{
    const std::size_t colon = host_port.rfind(':');
    // "host" or "host:port". Without the colon the join default port applies, so "--net-join HOST" works.
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
    if (!winsock_ready(error))
        return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (rc != 0 || found == nullptr)
    {
        error = "cannot resolve '" + host + "': WSA error " + std::to_string(rc);
        if (found)
            freeaddrinfo(found);
        return false;
    }
    const auto *sa = reinterpret_cast<const sockaddr_in *>(found->ai_addr);
    out.ipv4 = ntohl(sa->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(found);
    return true;
}

std::string lan_ipv4()
{
    std::string error;
    if (!winsock_ready(error))
        return std::string();
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return std::string();
    // The routing trick (see udp_socket_posix.cpp): connect()ing to a public address, nothing sent, then
    // getsockname() returns the default-route interface's address, not a docker/vpn bridge's.
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x01010101); // 1.1.1.1
    std::string result;
    if (connect(sock, reinterpret_cast<const sockaddr *>(&dst), sizeof(dst)) == 0)
    {
        sockaddr_in local{};
        int length = sizeof(local);
        if (getsockname(sock, reinterpret_cast<sockaddr *>(&local), &length) == 0)
        {
            char text[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
            {
                const uint32_t ip = ntohl(local.sin_addr.s_addr);
                if (ip != 0 && (ip >> 24) != 0x7F) // not 0.0.0.0 or 127.x
                    result = text;
            }
        }
    }
    closesocket(sock);
    return result;
}

} // namespace don_net

#endif // _WIN32
