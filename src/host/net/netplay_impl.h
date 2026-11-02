#pragma once
// Private implementation of Netplay (netplay.h): the state and the members, split by role across
// netplay.cpp (public wrapper, shared helpers), netplay_host.cpp, netplay_client.cpp and netplay_session.cpp.

#include "netplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "lockstep.h"
#include "protocol.h"

namespace don_net
{

using Steady = std::chrono::steady_clock;

inline uint64_t now_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Steady::now().time_since_epoch()).count());
}

inline bool is_type(const Message &m, MsgType t) { return static_cast<MsgType>(m.body.index() + 1) == t; }

inline int clamp_lobby_timeout(int ms) { return ms > 0 ? ms : 10000; }

// The host part of "host[:port]" (the port is handled by resolve()).
inline std::string join_host(const std::string &host_port)
{
    const std::size_t colon = host_port.rfind(':');
    return colon == std::string::npos ? host_port : host_port.substr(0, colon);
}

// A dotted-quad IPv4 address resolves with no network (getaddrinfo only parses it), so it is resolved in
// start(). A domain name may block for tens of seconds on a dead one, so it is resolved on a background
// thread (docs/knowledge/22-netplay.md section 7.2: the Connecting screen must keep drawing and Esc cancels).
inline bool is_numeric_ipv4(const std::string &host)
{
    if (host.empty())
        return false;
    int groups = 0;
    int digits = 0;
    for (char c : host)
    {
        if (c >= '0' && c <= '9')
        {
            if (++digits > 3)
                return false;
        }
        else if (c == '.')
        {
            if (digits == 0)
                return false; // an empty group ("1..2")
            ++groups;
            digits = 0;
        }
        else
        {
            return false;
        }
    }
    return groups == 3 && digits > 0;
}

// How long the host keeps repeating a DESYNC (every 20 ms) so that a lossy transport cannot strand a client that
// missed the single datagram: the session ends here anyway, and close() drains for 200 ms for the same reason.
constexpr int kDesyncFlushMs = 240;

struct Netplay::Impl
{
    NetConfig cfg;
    std::unique_ptr<Transport> transport;

    NetSession session;
    uint16_t session_id = 0;
    bool connected = false;
    bool ended = false;
    HandshakeState hs = HandshakeState::Connecting;
    std::string hs_error;       // the lobby's reason when hs == Failed
    std::string end_error;      // the playing phase's reason (step / close)
    bool start_called = false;

    // Host lobby (section 6.0): clients in compacted order, index i = wire player i+1.
    struct Client
    {
        PeerAddr addr;
        std::string name;
        int ping_ms = 0;
        uint64_t last_heard_us = 0;
    };
    std::vector<Client> clients;
    bool start_requested = false;
    uint64_t last_lobby_us = 0;

    // Client lobby: the last LOBBY list.
    std::vector<LobbyPeer> lobby_list;

    // Host playing phase, one entry per player (players 1..N-1 are the clients).
    std::vector<PeerAddr> peer_addr;
    std::vector<uint64_t> peer_last_heard_us;
    std::vector<bool> peer_started;      // per player, the client has begun sending (post-START)

    // Host hashes: one entry per frame the host (or a client first) has hashed. The host compares its own value with
    // each client's as soon as both are there, whatever frames the two were at when each sent it. An entry that never
    // sees every client's value is logged and dropped after abort_after_ms: a lost HASH is not fatal (the client
    // resends it, and a peer that is really gone is caught by the peer-silence check of step()).
    struct HashCheck
    {
        uint64_t own = 0;
        bool own_known = false;
        std::vector<uint64_t> client; // per player (index player); only [1..players) used
        std::vector<bool> got;        // per player, a HASH for this frame arrived
        uint64_t deadline_us = 0;
    };
    std::map<uint32_t, HashCheck> hash_checks;

    // Client hashes: the HASHes sent but not yet confirmed by the host's FRAMES (resent with every INPUT).
    struct PendingHash
    {
        uint32_t frame = 0;
        uint64_t hash = 0;
    };
    std::vector<PendingHash> pending_hashes;

    // Client.
    PeerAddr host_addr;
    bool welcome_received = false;
    bool start_received = false;
    std::string host_display_name;
    uint64_t host_last_heard_us = 0;

    // Client: the resolve of start() for a domain name runs on a background thread (a dead domain blocks
    // getaddrinfo for tens of seconds). The thread holds this shared state, never `this`, so cancel() and the
    // destructor do not join it and the screen never waits for it.
    struct ResolveState
    {
        std::mutex mutex;
        bool done = false;
        bool ok = false;
        PeerAddr addr;
        std::string error;
    };
    std::shared_ptr<ResolveState> resolve_state;

    // Host: the lobby notices ("Refused <name>: ..."), taken by take_notices().
    std::vector<std::string> notices;

    // Ping bookkeeping (both roles).
    uint64_t ping_seq = 0;
    std::map<uint32_t, uint64_t> pending_pings; // id -> sent us
    std::map<uint32_t, uint64_t> ping_rtts;     // id -> measured rtt us
    std::map<uint32_t, int> ping_client;        // id -> host lobby client index
    uint64_t last_rtt_us = 0;
    uint64_t last_ping_us = 0;
    uint64_t last_hello_us = 0;
    uint64_t handshake_start_us = 0;

    std::unique_ptr<Lockstep> ls;

    // ---- helpers ---------------------------------------------------------

    bool send_msg(const PeerAddr &to, const Message &m);

    int client_index_of(const PeerAddr &from) const;

    // Host: itself + the clients in the lobby.
    int lobby_count() const { return 1 + static_cast<int>(clients.size()); }

    void send_bye(uint8_t reason);

    void send_hello();

    void answer_ping(const PeerAddr &to, uint16_t sid, uint32_t id, uint64_t time_us);

    void record_pong(uint32_t id);

    // One PING to every known peer every second (the round trip is shown in LOBBY and in status()).
    void maybe_ping();

    // Pumps the transport for `ms` milliseconds so a simulated network's queued (latency-held) datagrams are
    // released. Needed on close() so the peers receive the last FRAMES before the session ends.
    void drain_ms(int ms);

    // ---- lobby (section 6.0) ---------------------------------------------

    void send_reject(const PeerAddr &to, uint8_t reason, const std::string &text);

    void send_welcome(int idx);

    void send_start_to(int idx);

    std::vector<LobbyPeer> host_lobby_list() const;

    void send_lobby();

    // A joiner the host refuses (another build, other data, full, started): REJECT to it, one line for the
    // host's log, and a notice for the start screen's lobby (take_notices()).
    void refuse_joiner(const PeerAddr &to, uint8_t reason, const std::string &text, const std::string &notice);

    void handle_hello(const PeerAddr &from, const Hello &h);

    // A client silent for lobby_timeout_ms in the lobby is removed (and LOBBY is resent).
    void remove_silent_clients();

    uint64_t measure_rtt(int idx, const std::function<bool()> &should_stop);

    // Compact the indices, measure the delay and send START to each client. Fewer than 2 connected: nothing.
    void do_request_start(const std::function<bool()> &should_stop = {});

    // ---- per-frame -------------------------------------------------------

    void send_frames();

    // Host: WAITING {frame, missing} while a frame is incomplete.
    void send_waiting(uint32_t frame);

    // Confirms every frame that became complete, and (re)sends FRAMES: the last FRAMES may have been lost, so a
    // duplicate INPUT is an opportunity to re-send the latest confirmed set.
    void host_confirm_new();

    void send_input();

    // Client: (re)send every HASH whose frame the host has not yet moved hash_every frames past.
    void send_pending_hashes();

    // A HASH is confirmed once the host's FRAMES have moved hash_every frames past its own frame: by then the host
    // either compared it or gave up on it. Bounded like the INPUT resends.
    void prune_pending_hashes();

    // Host: the per-frame hash record (our value, the clients' values, the give-up deadline).
    HashCheck &hash_check(uint32_t frame);

    // Host: compare every client value already present for this frame. True (and ended) on the first mismatch.
    bool host_compare_hash(uint32_t frame, HashCheck &c);

    // DESYNC is the host's last word and the session ends here anyway: repeat it over a short flush so that losing one
    // datagram does not strand a client waiting for FRAMES until its own 30 s timeout (the same reason close() drains).
    void send_desync(uint32_t frame, int player);

    // Host: a client's HASH (early or late) is compared as soon as our own value for that frame exists.
    void host_record_client_hash(int player, uint32_t frame, uint64_t hash);

    // Host: our own value for a frame the clients may already have sent.
    bool host_record_local_hash(uint32_t frame, uint64_t hash);

    // A HASH that never arrived in abort_after_ms is logged once and forgotten: not fatal.
    void host_expire_hash_checks();

    bool host_peer_silent(std::string &error);

    // ---- message dispatch -------------------------------------------------

    void handle_client_msg(const Message &msg);

    void handle_host_msg(int player, const Message &msg);

    bool recv_process(int timeout_ms);

    // ---- the non-blocking handshake --------------------------------------

    void fail(const std::string &e);

    bool poll_impl(NetSession &out, std::string &error);

    bool step_impl(uint32_t f, const NetPad &local, NetPad *out, std::string &error,
                   const std::function<bool()> &should_stop);
};

} // namespace don_net
