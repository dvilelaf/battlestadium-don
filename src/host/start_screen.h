// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen (T-517, docs/knowledge/22-netplay.md section 7.2): a small SDL window (640x448) of the port
// itself, before the game window opens, so the players of the AppImage and of the Windows package never type
// options. Keyboard or gamepad, English texts.
//
// The state machine lives in start_screen_model.h/.cpp (no SDL, tested on its own); this file is the view: it
// draws the model's state with SDL_RenderDebugText, turns SDL events into model inputs, and hands the caller
// the outcome. The network sits behind StartScreenNet.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/netplay.h"
#include "start_screen_model.h"

namespace don_host
{

struct StartScreenRequest
{
    StartScreenModel::Remembered remembered;
    // Pixels of every distinct screen are written here as BMP when not empty (the host fills it from
    // DON_START_SCREEN_SHOT=<dir>, section 7.2; --shots cannot see this window).
    std::string shots_dir;
};

enum class StartScreenOutcomeKind
{
    PlayLocal, // start the game as today
    Quit,      // the player closed the window or chose Quit
    Net,       // the session is up: the caller plays with the layer's Netplay
};

struct StartScreenOutcome
{
    StartScreenOutcomeKind kind = StartScreenOutcomeKind::PlayLocal;
    bool host = false;    // Net: this PC is the host (player 0)
    int players = 2;      // Net: the session's player count (the connected count on the host)
    uint16_t port = 7877; // Net and host: the UDP port the session listens on
    // What the caller writes back to net.ini (the screen itself remembers nothing).
    std::string name;    // may be empty: the OS user name then stands in
    std::string address; // the last address that was attempted
    int last_players = 2; // the last player count chosen when hosting
    // 7.3 (T-522): the LocalSetup and Settings choices, applied by the host to the game it starts.
    int local_players = 1;   // Play: 1..4 (the host maps 3..4 to the multitap layout)
    bool fullscreen = false; // the game window opens fullscreen
    int renderer = 0;        // 0 Vulkan, 1 software
    bool vibration = true;   // the game's vibration on the gamepads
    int net_delay = 0;       // 0 = automatic, 1..10 frames (hosting)
};

// Runs the screen until the player plays, hosts, joins or quits. Passing a null net is allowed (the net items
// then do nothing but the screens still work); when the window cannot be opened the game starts as today and
// the reason goes to stderr. Never throws.
StartScreenOutcome don_run_start_screen(const StartScreenRequest &request, StartScreenNet *net);

// The real layer (section 7.2): the non-blocking Netplay (section 6.0) and UPnP (section 7.1), so the screen
// never waits for the network. The caller makes one, passes it to don_run_start_screen, and when the outcome
// is Net takes the running session with take_netplay(); the layer then owns nothing more. A layer that is left
// unused (PlayLocal, Quit, or the player backing out of the lobby) says BYE and removes its UPnP mapping.
class StartScreenNetLayer final : public StartScreenNet
{
public:
    // build_id and elf_path are what goes in HELLO (build_id.h and the game-file hash of game_hash.h): the
    // hash is computed only when the player actually hosts or joins (PlayLocal has no use for it). delay_ms is
    // --net-delay (0 = measure it from the lobby's pings).
    StartScreenNetLayer(std::string build_id, std::string elf_path, int delay_ms = 0);
    ~StartScreenNetLayer() override;

    StartScreenNetLayer(const StartScreenNetLayer &) = delete;
    StartScreenNetLayer &operator=(const StartScreenNetLayer &) = delete;

    void host_begin(uint16_t port, int players, const std::string &name, int net_delay) override;
    void join_begin(const std::string &address, const std::string &name) override;
    void host_start() override;
    void cancel() override;
    void poll(std::vector<NetEvent> &out) override;

    // The session the screen ended on (outcome.kind == Net): the caller keeps the Netplay for the whole game
    // and closes its UPnP mapping and memory-card folder at the end. Empty when there is no session.
    std::unique_ptr<don_net::Netplay> take_netplay();
    const don_net::NetSession &session() const { return m_session; }

private:
    void reset();                    // drop the handshake: BYE, the UPnP mapping, the events
    void upnp_wait();                // join the discovery thread, if one runs
    void post(const NetEvent &e);    // from any thread (the UPnP one posts the router's answer)
    NetEvent failure_event(const std::string &error) const; // Netplay's line -> the model's event
    void report_lobby(std::vector<NetEvent> &out);    // the joins, the leaves, the pings

    std::string m_build_id;
    std::string m_elf_path; // the hash of the game files is computed when hosting/joining
    int m_delay_ms = 0;

    std::unique_ptr<don_net::Netplay> m_netplay;
    don_net::NetSession m_session;
    bool m_host = false;
    bool m_silent = false;    // cancelled or started: nothing more comes from the old handshake
    bool m_welcomed = false;  // the client: WELCOME was reported
    bool m_upnp_open = false; // a mapping was made and upnp_close has to remove it
    int m_local_player = -1;  // this PC's index in the lobby (the host: 0), -1 = unknown
    std::string m_name;       // the name sent in HELLO: it identifies this PC in the lobby list
    std::vector<don_net::LobbyPeer> m_lobby; // the last list reported

    // UPnP's discovery takes seconds (upnpDiscover's own 2 s): it runs on its own thread, so the lobby draws
    // and clients can already join while the router is asked.
    std::thread m_upnp_thread;
    std::mutex m_mutex;
    std::vector<NetEvent> m_events;
};

// Section 7.2, during play: the session ended (a player gone for 30 s, a desync, the host quitting). The text
// is one of StartScreenModel::net_end_player_left / net_end_desync, or the layer's error line. Shows the
// message box; the caller then stops the game and exits with code 4 (it is the caller's decision, so that the
// exit code of the program stays in one place).
void don_show_net_session_end(const std::string &reason);

} // namespace don_host
