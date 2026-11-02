#include "netplay_impl.h"

namespace don_net
{
Netplay::Netplay(NetConfig config, std::unique_ptr<Transport> transport)
    : m(std::make_unique<Impl>())
{
    m->cfg = std::move(config);
    m->transport = std::move(transport);
    m->cfg.players = std::clamp(m->cfg.players, 2, kMaxPlayers);
}


HandshakeState Netplay::poll(NetSession &out, std::string &error)
{
    NetSession s;
    std::string e;
    if (m->poll_impl(s, e))
    {
        out = s;
        return HandshakeState::Started;
    }
    if (m->hs == HandshakeState::Failed)
    {
        error = e;
        return HandshakeState::Failed;
    }
    out = s;
    return m->hs;
}


void Netplay::request_start() { m->do_request_start(); }


std::vector<LobbyPeer> Netplay::lobby() const
{
    if (m->cfg.host)
        return m->host_lobby_list();
    return m->lobby_list;
}


int Netplay::connected_players() const
{
    if (m->cfg.host)
        return 1 + static_cast<int>(m->clients.size());
    return static_cast<int>(m->lobby_list.size());
}


std::string Netplay::host_name() const { return m->cfg.host ? m->cfg.name : m->host_display_name; }


int Netplay::welcome_player() const
{
    if (m->cfg.host)
        return 0;
    return m->welcome_received ? m->session.local_player : -1;
}


std::vector<std::string> Netplay::take_notices()
{
    std::vector<std::string> out;
    out.swap(m->notices);
    return out;
}


void Netplay::cancel()
{
    m->send_bye(kByeQuit);
    m->fail("cancelled");
}


bool Netplay::step(uint32_t f, const NetPad &local, NetPad *out, std::string &error,
                   const std::function<bool()> &should_stop)
{
    return m->step_impl(f, local, out, error, should_stop);
}


std::string Netplay::status() const
{
    std::string s = m->cfg.host ? "host" : "client";
    if (m->hs == HandshakeState::Lobby)
    {
        s += " lobby=" + std::to_string(m->lobby_count());
    }
    s += " players=" + std::to_string(m->session.players);
    s += " delay=" + std::to_string(m->session.delay);
    if (m->connected)
        s += " session=" + std::to_string(m->session_id);
    if (m->last_rtt_us != 0)
        s += " rtt=" + std::to_string(m->last_rtt_us / 1000) + "ms";
    return s;
}


bool Netplay::Impl::send_msg(const PeerAddr &to, const Message &m)
{
    const std::vector<uint8_t> bytes = encode(m);
    if (bytes.empty())
        return false;
    return transport->send(to, bytes.data(), bytes.size());
}

int Netplay::Impl::client_index_of(const PeerAddr &from) const
{
    for (std::size_t i = 0; i < clients.size(); ++i)
        if (clients[i].addr == from)
            return static_cast<int>(i);
    return -1;
}

void Netplay::Impl::send_bye(uint8_t reason)
{
    Message m;
    m.session = session_id;
    m.body = Bye{reason};
    if (cfg.host)
    {
        for (const Client &c : clients)
            send_msg(c.addr, m);
    }
    else if (host_addr.port != 0)
    {
        send_msg(host_addr, m);
    }
}

void Netplay::Impl::send_hello()
{
    Message m;
    m.session = 0;
    Hello h;
    h.build_id = cfg.build_id;
    h.data_hash = cfg.data_hash;
    h.module_id = cfg.module_id;
    h.name = cfg.name;
    m.body = std::move(h);
    send_msg(host_addr, m);
}

void Netplay::Impl::answer_ping(const PeerAddr &to, uint16_t sid, uint32_t id, uint64_t time_us)
{
    Message m;
    m.session = sid;
    m.body = Pong{id, time_us};
    send_msg(to, m);
}

void Netplay::Impl::record_pong(uint32_t id)
{
    const auto it = pending_pings.find(id);
    if (it == pending_pings.end())
        return;
    const uint64_t rtt = now_us() - it->second;
    ping_rtts[id] = rtt;
    last_rtt_us = rtt;
    const auto owner = ping_client.find(id);
    if (owner != ping_client.end() && owner->second >= 0 && owner->second < static_cast<int>(clients.size()))
        clients[owner->second].ping_ms = static_cast<int>((rtt + 500) / 1000);
    pending_pings.erase(it);
}

// One PING to every known peer every second (the round trip is shown in LOBBY and in status()).
void Netplay::Impl::maybe_ping()
{
    const uint64_t now = now_us();
    if (now - last_ping_us < 1000000)
        return;
    last_ping_us = now;
    if (cfg.host)
    {
        for (std::size_t i = 0; i < clients.size(); ++i)
        {
            const uint32_t id = static_cast<uint32_t>(++ping_seq);
            const uint64_t t = now_us();
            pending_pings[id] = t;
            ping_client[id] = static_cast<int>(i);
            Message m;
            m.session = session_id;
            m.body = Ping{id, t};
            send_msg(clients[i].addr, m);
        }
    }
    else if (host_addr.port != 0)
    {
        const uint32_t id = static_cast<uint32_t>(++ping_seq);
        const uint64_t t = now_us();
        pending_pings[id] = t;
        Message m;
        m.session = session_id;
        m.body = Ping{id, t};
        send_msg(host_addr, m);
    }
}

// Pumps the transport for `ms` milliseconds so a simulated network's queued (latency-held) datagrams are
// released. Needed on close() so the peers receive the last FRAMES before the session ends.
void Netplay::Impl::drain_ms(int ms)
{
    const uint64_t end = now_us() + static_cast<uint64_t>(ms) * 1000;
    while (now_us() < end)
        recv_process(1);
}

} // namespace don_net
