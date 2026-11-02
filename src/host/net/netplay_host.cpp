#include "netplay_impl.h"

namespace don_net
{

void Netplay::Impl::send_reject(const PeerAddr &to, uint8_t reason, const std::string &text)
{
    Message m;
    m.session = session_id;
    m.body = Reject{reason, text};
    send_msg(to, m);
}

void Netplay::Impl::send_welcome(int idx)
{
    Message m;
    m.session = session_id;
    Welcome w;
    w.player = static_cast<uint8_t>(idx + 1);
    w.players = static_cast<uint8_t>(session.players); // provisional: the host's maximum
    w.layout = 0;                                      // provisional: the final one comes in START
    w.delay = 0;                                       // provisional: measured at request_start
    w.hash_every = static_cast<uint16_t>(cfg.hash_every);
    w.session_epoch = session.session_epoch;
    w.host_name = cfg.name;
    m.body = std::move(w);
    send_msg(clients[idx].addr, m);
}

void Netplay::Impl::send_start_to(int idx)
{
    Message m;
    m.session = session_id;
    Start s;
    s.first_frame = 1;
    s.players = static_cast<uint8_t>(session.players);
    s.layout = static_cast<uint8_t>(session.layout);
    s.delay = static_cast<uint8_t>(session.delay);
    s.your_player = static_cast<uint8_t>(idx + 1);
    m.body = s;
    send_msg(clients[idx].addr, m);
}

std::vector<LobbyPeer> Netplay::Impl::host_lobby_list() const
{
    std::vector<LobbyPeer> out;
    LobbyPeer host;
    host.player = 0;
    host.name = cfg.name;
    host.ping_ms = 0;
    out.push_back(host);
    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        LobbyPeer p;
        p.player = static_cast<int>(i) + 1;
        p.name = clients[i].name;
        p.ping_ms = clients[i].ping_ms;
        out.push_back(p);
    }
    return out;
}

void Netplay::Impl::send_lobby()
{
    Message m;
    m.session = session_id;
    Lobby l;
    for (const LobbyPeer &p : host_lobby_list())
    {
        LobbyEntry e;
        e.player = static_cast<uint8_t>(p.player);
        e.ping_ms = static_cast<uint16_t>(std::clamp(p.ping_ms, 0, 65535));
        e.name = p.name;
        l.players.push_back(std::move(e));
    }
    m.body = std::move(l);
    for (const Client &c : clients)
        send_msg(c.addr, m);
}

// A joiner the host refuses (another build, other data, full, started): REJECT to it, one line for the
// host's log, and a notice for the start screen's lobby (take_notices()).
void Netplay::Impl::refuse_joiner(const PeerAddr &to, uint8_t reason, const std::string &text, const std::string &notice)
{
    send_reject(to, reason, text);
    notices.push_back(notice);
    std::fprintf(stderr, "[don-net] %s\n", notice.c_str());
}

void Netplay::Impl::handle_hello(const PeerAddr &from, const Hello &h)
{
    const int idx = client_index_of(from);
    if (idx >= 0)
    {
        // The client resends HELLO every 500 ms: a retransmission, not a new join. Answer with its WELCOME
        // (lobby) or START (started) so a lost datagram does not strand it.
        clients[idx].last_heard_us = now_us();
        if (hs == HandshakeState::Started)
            send_start_to(idx);
        else
            send_welcome(idx);
        return;
    }
    // The joiner's name for the lobby's notice (HELLO may carry an empty name).
    const std::string who = h.name.empty() ? std::string("a player") : h.name;
    if (h.build_id != cfg.build_id)
    {
        refuse_joiner(from, kRejectBuild, "host " + cfg.build_id + " / you " + h.build_id,
                      "Refused " + who + ": another build (" + h.build_id + ")");
        return;
    }
    if (h.data_hash != cfg.data_hash)
    {
        refuse_joiner(from, kRejectData, "the host uses other game data (another language variant)",
                      "Refused " + who + ": other game data (another language variant)");
        return;
    }
    if (h.module_id != cfg.module_id)
    {
        // A linked host has no module id: the player sees "built-in" instead of an empty name.
        const std::string shown = cfg.module_id.empty() ? "built-in" : cfg.module_id.substr(0, 8);
        const std::string text = "The host's game was prepared differently (module " + shown + ").";
        refuse_joiner(from, kRejectModule, text, "Refused " + who + ": game prepared differently");
        return;
    }
    if (hs == HandshakeState::Started)
    {
        refuse_joiner(from, kRejectStarted, "the game has already started",
                      "Refused " + who + ": the game has already started");
        return;
    }
    if (static_cast<int>(clients.size()) >= session.players - 1)
    {
        refuse_joiner(from, kRejectFull, "the game is full",
                      "Refused " + who + ": the game is full");
        return;
    }
    Client c;
    c.addr = from;
    c.name = h.name;
    c.ping_ms = 0;
    c.last_heard_us = now_us();
    clients.push_back(std::move(c));
    const int joined = static_cast<int>(clients.size()) - 1;
    send_welcome(joined);
    send_lobby();
    std::fprintf(stderr, "[don-net] player %d (%s) joined the lobby\n", joined + 1, clients[joined].name.c_str());
}

// A client silent for lobby_timeout_ms in the lobby is removed (and LOBBY is resent).
void Netplay::Impl::remove_silent_clients()
{
    const uint64_t now = now_us();
    const uint64_t limit = static_cast<uint64_t>(clamp_lobby_timeout(cfg.lobby_timeout_ms)) * 1000;
    bool removed = false;
    for (std::size_t i = 0; i < clients.size();)
    {
        if (now - clients[i].last_heard_us > limit)
        {
            std::fprintf(stderr, "[don-net] player %d (%s) timed out in the lobby\n", static_cast<int>(i) + 1,
                         clients[i].name.c_str());
            clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        }
        else
        {
            ++i;
        }
    }
    if (removed)
        send_lobby();
}

uint64_t Netplay::Impl::measure_rtt(int idx, const std::function<bool()> &should_stop)
{
    std::vector<uint64_t> rtts;
    for (int i = 0; i < 5; ++i)
    {
        if (should_stop && should_stop())
            return rtts.empty() ? 0 : rtts[rtts.size() / 2]; // stop in time, do not wait out the 5 pings
        const uint32_t id = static_cast<uint32_t>(++ping_seq);
        const uint64_t t = now_us();
        pending_pings[id] = t;
        Message m;
        m.session = session_id;
        m.body = Ping{id, t};
        send_msg(clients[idx].addr, m);
        const uint64_t window_end = t + 200000; // 200 ms window per ping
        while (now_us() < window_end)
        {
            if (should_stop && should_stop())
                return rtts.empty() ? 0 : rtts[rtts.size() / 2];
            const uint64_t left = window_end - now_us();
            recv_process(static_cast<int>(std::min<uint64_t>(left / 1000 + 1, 50)));
            const auto it = ping_rtts.find(id);
            if (it != ping_rtts.end())
            {
                rtts.push_back(it->second);
                break;
            }
        }
        const uint64_t next = t + 100000; // the 5 pings are 100 ms apart (section 6.1)
        while (now_us() < next && !(should_stop && should_stop()))
            std::this_thread::sleep_for(std::chrono::microseconds(
                std::min<uint64_t>(next - now_us(), 2000)));
    }
    if (rtts.empty())
        return 0;
    std::sort(rtts.begin(), rtts.end());
    return rtts[rtts.size() / 2];
}

// Compact the indices, measure the delay and send START to each client. Fewer than 2 connected: nothing.
void Netplay::Impl::do_request_start(const std::function<bool()> &should_stop)
{
    if (hs != HandshakeState::Lobby || start_requested)
        return;
    if (lobby_count() < 2)
        return;

    session.players = 1 + static_cast<int>(clients.size());
    session.local_player = 0;
    session.layout = (session.players == 2) ? 0 : 1;
    if (cfg.delay != 0)
    {
        session.delay = std::clamp(cfg.delay, 1, 10);
    }
    else
    {
        double max_rtt_ms = 0.0;
        std::string rtts;
        for (std::size_t i = 0; i < clients.size(); ++i)
        {
            if (should_stop && should_stop())
            {
                fail("stopped");
                return;
            }
            const uint64_t rtt_us = measure_rtt(static_cast<int>(i), should_stop);
            const double rtt_ms = static_cast<double>(rtt_us) / 1000.0;
            if (rtt_ms > max_rtt_ms)
                max_rtt_ms = rtt_ms;
            if (!rtts.empty())
                rtts += ", ";
            rtts += "p" + std::to_string(i + 1) + " " + std::to_string(static_cast<int>(rtt_ms + 0.5)) + " ms";
        }
        session.delay = std::clamp(static_cast<int>(std::ceil(max_rtt_ms / 16.7)) + 1, 1, 10);
        std::fprintf(stderr, "[don-net] delay %d frames (round trips: %s)\n", session.delay, rtts.c_str());
    }
    if (should_stop && should_stop())
    {
        fail("stopped");
        return;
    }

    peer_addr.assign(session.players, PeerAddr{});
    peer_last_heard_us.assign(session.players, now_us());
    peer_started.assign(session.players, false);
    hash_checks.clear();
    pending_hashes.clear();
    for (std::size_t i = 0; i < clients.size(); ++i)
    {
        peer_addr[i + 1] = clients[i].addr;
        peer_last_heard_us[i + 1] = clients[i].last_heard_us;
    }

    ls = std::make_unique<Lockstep>(session.players, session.local_player, session.delay);
    for (std::size_t i = 0; i < clients.size(); ++i)
        send_start_to(static_cast<int>(i));

    start_requested = true;
    connected = true;
    hs = HandshakeState::Started;
    std::fprintf(stderr, "[don-net] starting with %d players, delay %d\n", session.players, session.delay);
}

// Host: the per-frame hash record (our value, the clients' values, the give-up deadline).
Netplay::Impl::HashCheck &Netplay::Impl::hash_check(uint32_t frame)
{
    HashCheck &c = hash_checks[frame];
    if (c.client.size() != static_cast<std::size_t>(session.players))
    {
        c.client.assign(static_cast<std::size_t>(session.players), 0);
        c.got.assign(static_cast<std::size_t>(session.players), false);
    }
    if (c.deadline_us == 0)
        c.deadline_us = now_us() + static_cast<uint64_t>(cfg.abort_after_ms) * 1000;
    return c;
}

// Host: compare every client value already present for this frame. True (and ended) on the first mismatch.
bool Netplay::Impl::host_compare_hash(uint32_t frame, HashCheck &c)
{
    if (!c.own_known)
        return false;
    for (int p = 1; p < static_cast<int>(c.got.size()); ++p)
    {
        if (!c.got[p] || c.client[p] == c.own)
            continue;
        std::fprintf(stderr, "[don-net] desync at frame %u (player %d)\n", frame, p);
        ended = true;
        end_error = "desync at frame " + std::to_string(frame);
        send_desync(frame, p);
        return true;
    }
    return false;
}

// Host: a client's HASH (early or late) is compared as soon as our own value for that frame exists.
void Netplay::Impl::host_record_client_hash(int player, uint32_t frame, uint64_t hash)
{
    HashCheck &c = hash_check(frame);
    if (player > 0 && player < static_cast<int>(c.got.size()))
    {
        c.client[player] = hash;
        c.got[player] = true;
    }
    host_compare_hash(frame, c);
}

// Host: our own value for a frame the clients may already have sent.
bool Netplay::Impl::host_record_local_hash(uint32_t frame, uint64_t hash)
{
    HashCheck &c = hash_check(frame);
    c.own = hash;
    c.own_known = true;
    return host_compare_hash(frame, c);
}

// A HASH that never arrived in abort_after_ms is logged once and forgotten: not fatal.
void Netplay::Impl::host_expire_hash_checks()
{
    const uint64_t now = now_us();
    for (auto it = hash_checks.begin(); it != hash_checks.end();)
    {
        HashCheck &c = it->second;
        if (c.deadline_us != 0 && now > c.deadline_us)
        {
            if (c.own_known)
                for (int p = 1; p < static_cast<int>(c.got.size()); ++p)
                    if (!c.got[p])
                        std::fprintf(stderr, "[don-net] no hash from player %d for frame %u (waited %d ms)\n",
                                     p, it->first, cfg.abort_after_ms);
            it = hash_checks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool Netplay::Impl::host_peer_silent(std::string &error)
{
    const uint64_t now = now_us();
    for (int p = 1; p < session.players; ++p)
    {
        if (peer_last_heard_us[p] == 0)
            continue;
        const uint64_t silent = now - peer_last_heard_us[p];
        if (silent > static_cast<uint64_t>(cfg.abort_after_ms) * 1000)
        {
            error = "player " + std::to_string(p) + " stopped sending (" + std::to_string(silent / 1000) + " ms)";
            return true;
        }
    }
    return false;
}

} // namespace don_net
