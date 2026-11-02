// The netplay coordinator (D14, docs/knowledge/22-netplay.md section 6): handshake and lobby (section 6.0),
// per-frame step, hashes. Built on protocol.h, lockstep.h and transport.h. No SDL, no runtime headers.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net_pad.h"
#include "transport.h"

namespace don_net
{

struct NetConfig
{
    bool host = false;
    uint16_t port = 7877; // host: listen port; client: 0 (any)
    std::string join;     // client: "host:port"
    int players = 2;      // host: total players expected, 2..4 (the lobby's maximum)
    int delay = 0;        // host decides (0 = automatic from the measured ping); clients take the host's
    std::string name;     // shown in the host's log and in the lobby
    std::string build_id; // don_build_id()
    uint64_t data_hash = 0; // hash_game_files()
    std::string module_id;  // the loaded game module's id (don_game_module_id()), empty when linked with the game
    int hash_every = 120;
    int warn_after_ms = 3000;  // print who is late
    int abort_after_ms = 30000; // end the session
    int lobby_timeout_ms = 10000; // section 6.0: a client silent for this long in the lobby is removed
};

struct NetSession
{
    int local_player = 0;
    int players = 0;
    int layout = 0;
    int delay = 0;
    uint64_t session_epoch = 0;
};

// Section 6.0: the lobby seen from outside. `player` is the compacted index (the host is 0).
struct LobbyPeer
{
    int player = 0;
    std::string name;
    int ping_ms = 0;
};

enum class HandshakeState { Connecting, Lobby, Started, Failed };

class Netplay
{
public:
    Netplay(NetConfig config, std::unique_ptr<Transport> transport);
    ~Netplay();

    Netplay(const Netplay &) = delete;
    Netplay &operator=(const Netplay &) = delete;

    // ---- section 6.0: the non-blocking handshake the start screen drives ----
    // Host: initialise and listen (the transport is already bound by the caller). Client: resolve and send the
    // first HELLO. False with error set when it cannot start.
    bool start(std::string &error);
    // Non-blocking; call every frame of the start screen. out is filled when it returns Started; error when Failed.
    HandshakeState poll(NetSession &out, std::string &error);
    // Host: compact the indices, measure the delay, send START. Does nothing with fewer than 2 connected.
    void request_start();
    std::vector<LobbyPeer> lobby() const; // the current list (host and client alike)
    int connected_players() const;        // host: itself + clients in the lobby; client: the last LOBBY count
    std::string host_name() const;        // client: from WELCOME; host: its own name
    // Client: the player index WELCOME assigned this PC (provisional, section 6.0; the final compacted one
    // comes in START). The host is always 0; a client returns -1 before its WELCOME.
    int welcome_player() const;
    // Host: the lobby notices since the last call, each one line ("Refused <name>: ..."). A refused joiner
    // (another build, other game data, full, started) is named here so the start screen's lobby can show it.
    std::vector<std::string> take_notices();
    void cancel();                        // BYE to every peer, back to nothing

    // ---- blocking wrapper over the handshake (the command line and the tests use it unchanged) ----
    // Host: waits for players-1 clients (HELLO checked against build_id and data_hash: REJECT otherwise), then
    // sends START. Client: sends HELLO every 500 ms until WELCOME or REJECT, then waits for START. Both give up
    // when should_stop() or after abort_after_ms without progress.
    bool connect(NetSession &out, std::string &error, const std::function<bool()> &should_stop);
    // Per frame f (1, 2, 3, ...): submit the local pad, send and receive, and block until the confirmed set for f
    // exists; out[0..players-1] get it. False: the session ended (error says why).
    bool step(uint32_t f, const NetPad &local, NetPad *out, std::string &error,
              const std::function<bool()> &should_stop);
    // Every hash_every frames, after step(f): a client sends HASH and keeps resending it (with its INPUTs) until the
    // host's FRAMES have moved hash_every frames past that frame; a host records its own hash for f and compares each
    // client's value when it arrives (which may be a later frame), sending DESYNC on a mismatch. The host never blocks
    // the game waiting for a hash: one that does not arrive within abort_after_ms is logged and dropped (lossy
    // transports are covered by the client's resends). False: desync or a stop request.
    bool report_hash(uint32_t f, uint64_t hash, std::string &error,
                     const std::function<bool()> &should_stop = {});
    void close(uint8_t bye_reason); // sends BYE to the peers
    std::string status() const;     // one line for the log

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace don_net
