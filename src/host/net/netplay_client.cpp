#include "netplay_impl.h"

namespace don_net
{

Netplay::~Netplay() = default;

bool Netplay::start(std::string &error)
{
    if (m->start_called)
    {
        if (m->hs == HandshakeState::Failed)
        {
            error = m->hs_error;
            return false;
        }
        return true;
    }
    m->start_called = true;
    m->handshake_start_us = now_us();

    if (m->cfg.host)
    {
        // The transport is already bound by the caller (open_udp / the tests' hub endpoint).
        std::mt19937_64 rng(static_cast<uint64_t>(now_us()) ^ 0x9E3779B97F4A7C15ull);
        m->session_id = static_cast<uint16_t>(1 + (rng() % 65535));
        m->session.players = std::clamp(m->cfg.players, 2, kMaxPlayers);
        m->session.local_player = 0;
        m->session.layout = 0; // provisional: the final one is set at request_start
        m->session.delay = 0;
        m->session.session_epoch = static_cast<uint64_t>(std::time(nullptr));
        m->last_lobby_us = now_us();
        m->hs = HandshakeState::Lobby;
        return true;
    }

    if (is_numeric_ipv4(join_host(m->cfg.join)))
    {
        // A dotted-quad IP has no network behind it: resolve it here, as before.
        if (!resolve(m->cfg.join, m->host_addr, error))
        {
            m->fail(error);
            return false;
        }
        m->session.local_player = 0;
        m->hs = HandshakeState::Connecting;
        m->last_hello_us = now_us();
        m->send_hello();
        return true;
    }

    // A domain name may block getaddrinfo for tens of seconds on a dead one: resolve it on a background
    // thread so the Connecting screen keeps drawing and Esc cancels (section 7.2). The first HELLO goes out
    // when the resolve lands, in poll().
    m->resolve_state = std::make_shared<Impl::ResolveState>();
    const std::string join = m->cfg.join;
    std::thread([state = m->resolve_state, join]() {
        PeerAddr addr;
        std::string err;
        const bool ok = resolve(join, addr, err);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ok = ok;
        state->addr = addr;
        state->error = err;
        state->done = true;
    }).detach();
    m->session.local_player = 0;
    m->hs = HandshakeState::Connecting;
    return true;
}


bool Netplay::connect(NetSession &out, std::string &error, const std::function<bool()> &should_stop)
{
    if (m->connected)
    {
        out = m->session;
        return true;
    }
    if (m->ended || m->hs == HandshakeState::Failed)
    {
        error = m->hs_error.empty() ? m->end_error : m->hs_error;
        return false;
    }
    if (!start(error))
        return false;

    const uint64_t deadline = now_us() + static_cast<uint64_t>(m->cfg.abort_after_ms) * 1000;
    for (;;)
    {
        if (should_stop())
        {
            m->fail("stopped");
            error = "stopped";
            return false;
        }
        if (m->hs == HandshakeState::Failed || m->ended)
        {
            error = m->hs_error.empty() ? m->end_error : m->hs_error;
            return false;
        }

        NetSession s;
        std::string perr;
        if (m->poll_impl(s, perr))
        {
            out = s;
            return true;
        }

        // Host: as soon as the expected number of clients is in the lobby, start.
        if (m->cfg.host && m->hs == HandshakeState::Lobby && m->lobby_count() >= m->cfg.players)
            m->do_request_start(should_stop);

        if (now_us() > deadline)
        {
            std::string e;
            if (!m->cfg.host)
                e = (m->hs == HandshakeState::Lobby) ? "no START from the host" : "no WELCOME from the host";
            else
                e = "timed out waiting for " +
                    std::to_string(m->cfg.players - m->lobby_count()) + " player(s)";
            m->fail(e);
            error = e;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}


void Netplay::Impl::handle_client_msg(const Message &msg)
{
    switch (static_cast<MsgType>(msg.body.index() + 1))
    {
    case MsgType::Start:
    {
        if (start_received)
            break; // a retransmitted START must not reset the lockstep engine
        const Start &s = std::get<Start>(msg.body);
        session.local_player = s.your_player;
        session.players = s.players;
        session.layout = s.layout;
        session.delay = s.delay;
        ls = std::make_unique<Lockstep>(session.players, session.local_player, session.delay);
        start_received = true;
        connected = true;
        hs = HandshakeState::Started;
        break;
    }
    case MsgType::Lobby:
    {
        const Lobby &l = std::get<Lobby>(msg.body);
        lobby_list.clear();
        for (const LobbyEntry &e : l.players)
        {
            LobbyPeer p;
            p.player = e.player;
            p.name = e.name;
            p.ping_ms = e.ping_ms;
            lobby_list.push_back(std::move(p));
        }
        break;
    }
    case MsgType::Frames:
    {
        const Frames &fr = std::get<Frames>(msg.body);
        for (std::size_t i = 0; i < fr.pads.size() / fr.players; ++i)
        {
            const uint32_t frame = fr.first_frame + static_cast<uint32_t>(i);
            ls->confirm(frame, &fr.pads[i * fr.players]);
        }
        const uint32_t through = fr.first_frame + static_cast<uint32_t>(fr.pads.size() / fr.players) - 1;
        ls->ack_local(through);
        break;
    }
    case MsgType::Desync:
    {
        // The DESYNC carries the frame the host measured: the client keeps it so the session-end box can
        // say "The game went out of sync at frame N" (section 7.2).
        const Desync &d = std::get<Desync>(msg.body);
        ended = true;
        end_error = "desync at frame " + std::to_string(d.frame);
        break;
    }
    case MsgType::Bye:
        fail("the host left the lobby");
        break;
    case MsgType::Ping:
        answer_ping(host_addr, msg.session, std::get<Ping>(msg.body).id, std::get<Ping>(msg.body).time_us);
        break;
    case MsgType::Pong:
        record_pong(std::get<Pong>(msg.body).id);
        break;
    case MsgType::Waiting:
    default:
        break;
    }
}

void Netplay::Impl::handle_host_msg(int player, const Message &msg)
{
    peer_last_heard_us[player] = now_us();
    if (player < static_cast<int>(peer_started.size()))
        peer_started[player] = true;
    switch (static_cast<MsgType>(msg.body.index() + 1))
    {
    case MsgType::Input:
    {
        const Input &in = std::get<Input>(msg.body);
        for (std::size_t i = 0; i < in.pads.size(); ++i)
            ls->record(player, in.first_frame + static_cast<uint32_t>(i), in.pads[i]);
        host_confirm_new();
        break;
    }
    case MsgType::Hash:
    {
        const Hash &h = std::get<Hash>(msg.body);
        host_record_client_hash(player, h.frame, h.hash);
        break;
    }
    case MsgType::Ping:
        answer_ping(peer_addr[player], msg.session, std::get<Ping>(msg.body).id,
                    std::get<Ping>(msg.body).time_us);
        break;
    case MsgType::Pong:
        record_pong(std::get<Pong>(msg.body).id);
        break;
    case MsgType::Bye:
        ended = true;
        end_error = "player " + std::to_string(player) + " left";
        break;
    default:
        break;
    }
}

bool Netplay::Impl::recv_process(int timeout_ms)
{
    PeerAddr from;
    std::vector<uint8_t> data;
    if (!transport->receive(from, data, timeout_ms))
        return false;
    Message msg;
    if (!decode(data.data(), data.size(), msg))
        return true;

    if (cfg.host)
    {
        if (is_type(msg, MsgType::Hello))
        {
            handle_hello(from, std::get<Hello>(msg.body));
            return true;
        }
        const int idx = client_index_of(from);
        if (hs == HandshakeState::Lobby)
        {
            if (idx < 0 || msg.session != session_id)
                return true;
            clients[idx].last_heard_us = now_us();
            if (is_type(msg, MsgType::Bye))
            {
                std::fprintf(stderr, "[don-net] player %d (%s) left the lobby\n", idx + 1,
                             clients[idx].name.c_str());
                clients.erase(clients.begin() + idx);
                send_lobby();
            }
            else if (is_type(msg, MsgType::Ping))
            {
                const Ping &p = std::get<Ping>(msg.body);
                answer_ping(from, msg.session, p.id, p.time_us);
            }
            else if (is_type(msg, MsgType::Pong))
            {
                record_pong(std::get<Pong>(msg.body).id);
            }
            return true;
        }
        if (msg.session != session_id || idx < 0)
            return true;
        handle_host_msg(idx + 1, msg);
    }
    else
    {
        // Everything in the lobby and the game comes from the host's address only; a datagram with the right
        // session id from anywhere else is ignored.
        if (from != host_addr)
            return true;
        if (!welcome_received)
        {
            if (is_type(msg, MsgType::Welcome))
            {
                const Welcome &w = std::get<Welcome>(msg.body);
                session.local_player = w.player;
                session.players = w.players; // provisional until START
                session.layout = w.layout;
                session.delay = w.delay;
                session.session_epoch = w.session_epoch;
                host_display_name = w.host_name;
                if (w.hash_every > 0)
                    cfg.hash_every = w.hash_every; // the HASH resend window must match the host's round
                session_id = msg.session;
                welcome_received = true;
                host_last_heard_us = now_us();
                hs = HandshakeState::Lobby;
            }
            else if (is_type(msg, MsgType::Reject))
            {
                const Reject &r = std::get<Reject>(msg.body);
                fail("rejected: " + r.text);
            }
            else if (is_type(msg, MsgType::Ping))
            {
                const Ping &p = std::get<Ping>(msg.body);
                answer_ping(from, msg.session, p.id, p.time_us);
            }
            return true;
        }
        if (msg.session != session_id)
            return true;
        host_last_heard_us = now_us();
        handle_client_msg(msg);
    }
    return true;
}

} // namespace don_net
