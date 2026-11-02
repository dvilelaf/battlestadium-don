#include "netplay_impl.h"

namespace don_net
{

bool Netplay::report_hash(uint32_t f, uint64_t hash, std::string &error,
                          const std::function<bool()> &should_stop)
{
    if (m->ended)
    {
        error = m->end_error;
        return false;
    }
    if (!m->cfg.host)
    {
        // Client: send HASH and keep it pending. It is resent with every INPUT (send_input) until the host's FRAMES
        // have moved hash_every frames past it, so losing the datagram is covered without any new message type. The
        // result comes back asynchronously: on a mismatch the host sends DESYNC, which the client observes on its next
        // step(); there is no HASH-OK message, so a matching host stays silent here.
        m->pending_hashes.push_back(Impl::PendingHash{f, hash});
        m->prune_pending_hashes();
        m->send_pending_hashes();
        return true;
    }

    // Host: record our own value and compare the clients' values that have already arrived. Never blocks: a client's
    // HASH for this frame may still be in flight and is compared when it lands (host_record_client_hash), and one
    // that never lands is only logged (host_expire_hash_checks).
    if (should_stop && should_stop())
    {
        m->fail("stopped");
        error = "stopped";
        return false;
    }
    if (m->host_record_local_hash(f, hash))
    {
        error = m->end_error;
        return false;
    }
    m->host_expire_hash_checks();
    return true;
}


void Netplay::close(uint8_t bye_reason)
{
    if (m->connected)
    {
        m->drain_ms(200); // flush latency-held FRAMES so peers reach their last frames before the BYE
        m->send_bye(bye_reason);
    }
    m->ended = true;
}


void Netplay::Impl::send_frames()
{
    if (!ls)
        return;
    const uint32_t delay = static_cast<uint32_t>(session.delay);
    if (ls->last_confirmed() <= delay)
        return;
    const uint32_t last = ls->last_confirmed();
    const uint32_t count = std::min<uint32_t>(12, last - delay);
    const uint32_t first = last - count + 1;
    Message m;
    m.session = session_id;
    Frames fr;
    fr.players = static_cast<uint8_t>(session.players);
    fr.first_frame = first;
    for (uint32_t g = first; g <= last; ++g)
        for (int p = 0; p < session.players; ++p)
            fr.pads.push_back(ls->confirmed_pad(g, p));
    m.body = std::move(fr);
    for (int p = 1; p < session.players; ++p)
        if (peer_addr[p].port != 0)
            send_msg(peer_addr[p], m);
}

// Host: WAITING {frame, missing} while a frame is incomplete.
void Netplay::Impl::send_waiting(uint32_t frame)
{
    Message m;
    m.session = session_id;
    Waiting w;
    w.frame = frame;
    w.missing = ls ? ls->missing(frame) : 0;
    m.body = w;
    for (int p = 1; p < session.players; ++p)
        if (peer_addr[p].port != 0)
            send_msg(peer_addr[p], m);
}

// Confirms every frame that became complete, and (re)sends FRAMES: the last FRAMES may have been lost, so a
// duplicate INPUT is an opportunity to re-send the latest confirmed set.
void Netplay::Impl::host_confirm_new()
{
    for (;;)
    {
        const uint32_t g = ls->last_confirmed() + 1;
        if (!ls->complete(g))
            break;
        ls->confirm(g, nullptr);
    }
    send_frames();
}

void Netplay::Impl::send_input()
{
    if (!ls)
        return;
    prune_pending_hashes();
    const auto unacked = ls->unacked_local(kMaxInputFrames);
    if (!unacked.empty())
    {
        Message m;
        m.session = session_id;
        Input in;
        in.player = static_cast<uint8_t>(session.local_player);
        in.first_frame = unacked.front().first;
        for (const auto &fp : unacked)
            in.pads.push_back(fp.second);
        m.body = std::move(in);
        send_msg(host_addr, m);
    }
    send_pending_hashes(); // the last unconfirmed HASHes travel with every INPUT (a lost one must not stall)
}

// Client: (re)send every HASH whose frame the host has not yet moved hash_every frames past.
void Netplay::Impl::send_pending_hashes()
{
    for (const PendingHash &p : pending_hashes)
    {
        Message m;
        m.session = session_id;
        m.body = Hash{p.frame, p.hash};
        send_msg(host_addr, m);
    }
}

// A HASH is confirmed once the host's FRAMES have moved hash_every frames past its own frame: by then the host
// either compared it or gave up on it. Bounded like the INPUT resends.
void Netplay::Impl::prune_pending_hashes()
{
    if (!ls)
        return;
    const uint32_t through = ls->last_confirmed();
    const uint32_t window = static_cast<uint32_t>(std::max(cfg.hash_every, 1));
    pending_hashes.erase(std::remove_if(pending_hashes.begin(), pending_hashes.end(),
                                        [through, window](const PendingHash &p) {
                                            return through >= p.frame + window;
                                        }),
                         pending_hashes.end());
    while (pending_hashes.size() > kMaxInputFrames)
        pending_hashes.erase(pending_hashes.begin());
}

// DESYNC is the host's last word and the session ends here anyway: repeat it over a short flush so that losing one
// datagram does not strand a client waiting for FRAMES until its own 30 s timeout (the same reason close() drains).
void Netplay::Impl::send_desync(uint32_t frame, int player)
{
    Message d;
    d.session = session_id;
    d.body = Desync{frame, static_cast<uint8_t>(player)};
    const uint64_t end = now_us() + static_cast<uint64_t>(kDesyncFlushMs) * 1000;
    for (;;)
    {
        for (int q = 1; q < session.players; ++q)
            if (peer_addr[q].port != 0)
                send_msg(peer_addr[q], d); // every send also releases the simulated network's due datagrams
        if (now_us() >= end)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void Netplay::Impl::fail(const std::string &e)
{
    ended = true;
    hs = HandshakeState::Failed;
    if (hs_error.empty())
        hs_error = e;
    if (end_error.empty())
        end_error = e;
}

bool Netplay::Impl::poll_impl(NetSession &out, std::string &error)
{
    if (hs == HandshakeState::Failed || ended)
    {
        error = hs_error.empty() ? end_error : hs_error;
        hs = HandshakeState::Failed;
        return false;
    }

    while (recv_process(0))
    {
    } // drain everything that is waiting

    const uint64_t now = now_us();
    if (cfg.host)
    {
        if (hs == HandshakeState::Lobby)
        {
            remove_silent_clients();
            if (now - last_lobby_us >= 1000000)
            {
                last_lobby_us = now;
                send_lobby();
            }
        }
        maybe_ping();
        if (hs == HandshakeState::Started)
        {
            out = session;
            return true;
        }
        return false; // Connecting/Lobby: nothing to report yet
    }

    // Client.
    // The resolve of start() (a domain name) runs on a background thread; its result turns into the first
    // HELLO or a Failed state here, while poll() itself never waits for the network.
    if (resolve_state)
    {
        bool done = false, ok = false;
        PeerAddr addr;
        std::string rerr;
        {
            std::lock_guard<std::mutex> lock(resolve_state->mutex);
            done = resolve_state->done;
            ok = resolve_state->ok;
            addr = resolve_state->addr;
            rerr = resolve_state->error;
        }
        if (done)
        {
            resolve_state.reset();
            if (ok)
            {
                host_addr = addr;
                handshake_start_us = now;
                last_hello_us = now;
                send_hello();
            }
            else
            {
                fail("resolve: " + rerr);
            }
        }
    }

    if (hs == HandshakeState::Connecting || hs == HandshakeState::Lobby)
    {
        if (host_addr.port != 0 && now - last_hello_us >= 500000)
        {
            last_hello_us = now;
            send_hello();
        }
        if (hs == HandshakeState::Connecting && handshake_start_us != 0 && host_addr.port != 0 &&
            now - handshake_start_us > static_cast<uint64_t>(cfg.abort_after_ms) * 1000)
        {
            fail("no WELCOME from the host");
        }
        else if (hs == HandshakeState::Lobby && host_last_heard_us != 0 &&
                 now - host_last_heard_us > static_cast<uint64_t>(clamp_lobby_timeout(cfg.lobby_timeout_ms)) * 1000)
        {
            fail("the host left the lobby");
        }
    }
    maybe_ping();
    if (hs == HandshakeState::Started)
    {
        out = session;
        return true;
    }
    if (hs == HandshakeState::Failed)
    {
        error = hs_error;
        return false;
    }
    return false;
}

bool Netplay::Impl::step_impl(uint32_t f, const NetPad &local, NetPad *out, std::string &error,
               const std::function<bool()> &should_stop)
{
    if (ended)
    {
        error = end_error;
        return false;
    }
    if (!ls)
    {
        error = "not connected";
        return false;
    }

    ls->submit_local(f, local);
    if (cfg.host)
        host_confirm_new(); // the host's own pad may complete a frame; (re)send FRAMES every step for redundancy
    else
        send_input();

    const uint64_t deadline = now_us() + static_cast<uint64_t>(cfg.abort_after_ms) * 1000;
    const uint64_t wait_start = now_us();
    bool warned = false;
    uint64_t last_send = 0;
    if (cfg.host && !ls->confirmed(f))
        send_waiting(f); // tell the clients what is missing right away
    while (!ls->confirmed(f))
    {
        if (should_stop())
        {
            error = "stopped";
            return false;
        }
        if (ended)
        {
            error = end_error;
            return false;
        }
        if (cfg.host && host_peer_silent(error))
        {
            ended = true;
            end_error = error;
            return false;
        }
        if (!cfg.host && host_last_heard_us != 0 &&
            now_us() - host_last_heard_us > static_cast<uint64_t>(cfg.abort_after_ms) * 1000)
        {
            error = "host stopped sending";
            ended = true;
            end_error = error;
            return false;
        }
        if (now_us() > deadline)
        {
            error = "timed out waiting for frame " + std::to_string(f);
            ended = true;
            end_error = error;
            return false;
        }

        recv_process(16);

        const uint64_t now = now_us();
        if (cfg.host && !warned && now - wait_start > static_cast<uint64_t>(cfg.warn_after_ms) * 1000)
        {
            warned = true; // log once per frame, and say which player is late
            const uint8_t miss = ls->missing(f);
            std::string who;
            for (int p = 1; p < session.players; ++p)
                if (miss & (1u << p))
                    who += (who.empty() ? "" : ", ") + std::to_string(p);
            std::fprintf(stderr, "[don-net] frame %u incomplete after %d ms (missing: player %s)\n", f,
                         cfg.warn_after_ms, who.empty() ? "?" : who.c_str());
        }
        if (now - last_send >= 16000)
        {
            last_send = now;
            if (cfg.host)
            {
                send_frames();
                send_waiting(f);
            }
            else
            {
                send_input();
            }
        }
        maybe_ping();
    }

    // Bound the memory: the frames are no longer needed 600 frames back (section 4), and the local pads are
    // acknowledged by the confirmed frame on both roles (on the host nobody sends FRAMES back to it).
    const uint32_t confirmed_through = ls->last_confirmed();
    ls->ack_local(confirmed_through);
    ls->forget_before(confirmed_through > 600 ? confirmed_through - 600 : 1);

    for (int p = 0; p < session.players; ++p)
        out[p] = ls->confirmed_pad(f, p);
    return true;
}

} // namespace don_net
