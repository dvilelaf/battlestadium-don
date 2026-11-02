// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen's state machine (T-517, docs/knowledge/22-netplay.md section 7.2): every state and every
// case listed there, with no SDL and no socket of its own.
//
//   * the view (start_screen.cpp) draws view() and turns SDL events into key()/text()/tick();
//   * the network sits behind StartScreenNet (below), which the unit test replaces with a fake;
//   * start_screen.cpp builds the real layer, start_screen_model.cpp knows nothing about SDL.
//
// A screen of its own, before the game window: the players of the AppImage and of the Windows package never
// type options, they pick from this menu.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace don_host
{

enum class Screen
{
    Menu,        // Play / Host / Join / Settings / Quit
    LocalSetup,  // Play: 1 / 2 / 3 / 4 players (7.3)
    NameEdit,    // superseded by Settings (the name moved there); kept so the view's switch still compiles
    HostSetup,   // Players: 2 / 3 / 4
    HostLobby,   // the join line (or the UPnP failure), the player list, Start
    JoinSetup,   // Address: <IP or domain[:port]>
    Connecting,  // "Connecting to <address>… N s"
    ClientLobby, // "Connected to <host>. Waiting for the host to start." and the list
    Settings,    // name, fullscreen, renderer, vibration, net delay (7.3)
    Error,       // one line with the reason and Back
    Done,        // result() is set: the view leaves and the caller starts the game (or exits)
};

enum class Key
{
    Up,
    Down,
    Left,
    Right,
    Confirm,   // Enter / Cross
    Back,      // Esc / the gamepad's back button: one step back
    Backspace, // editing the name or the address
    Quit,      // the window was closed
};

// Why an operation failed. Each case of section 7.2 has its own message and the model builds it: the layer
// only says which one happened.
enum class NetError
{
    Other,
    PortInUse,       // host: another program (or another copy of the port) holds the UDP port
    NoIpv4Interface, // host: this PC has no network interface with an IPv4 address
    BadAddress,      // client: the address is empty or malformed (the model checks this one itself)
    Resolve,         // client: the name does not resolve
    NoAnswer,        // client: no answer in 10 s (the model's own timer, see tick())
    Reject,          // client: REJECT, with reason 1..4
    HostLeft,        // client: the host left the lobby or cancelled it
};

// Everything the layer tells the model. One event, one case of 7.2: the model never waits for the network.
struct NetEvent
{
    enum class Type
    {
        HostBound,       // ip = this PC's local address; the UDP port is bound
        HostBindFailed,  // error, text
        UpnpMapped,      // ip = the router's external address (or the host's domain later)
        UpnpFailed,      // text = why, ip = this PC's local address (the lobby shows the by-hand hint)
        PlayerJoined,    // player (1..3), name, ping_ms
        PlayerLeft,      // player (a client leaving the lobby: removed from the list)
        PlayerNotice,    // text: a joiner that was refused, e.g. another build (shows both ids)
        WelcomeReceived, // name = the host's name (may be empty), players
        Rejected,        // reason (1 build, 2 data, 3 full, 4 started), text
        ConnectFailed,   // error, text
        HostLeft,        // the host left the lobby
        SessionStarted,  // players = the session's player count (on the host: the connected count)
        SessionFailed,   // error, text
    };
    Type type = Type::HostBound;
    NetError error = NetError::Other;
    int player = 0;
    int players = 0;
    int reason = 0;
    int ping_ms = -1;
    std::string text;
    std::string name;
    std::string ip;
};

struct LobbyPlayer
{
    int player = 0;
    std::string name;
    int ping_ms = -1; // -1: not measured yet
    bool local = false;
};

// What the model asks the network to do. The real layer (netplay.h + upnp.h, start_screen.cpp) and the unit
// test's fake both implement it. No call may block: the lobby and the connecting screen poll.
class StartScreenNet
{
public:
    virtual ~StartScreenNet() = default;
    // Host: bind the UDP port, run UPnP, then accept clients until host_start() or cancel(). net_delay is
    // the Settings' choice (0 = automatic, 1..10): it overrides the layer's own default (7.3).
    virtual void host_begin(uint16_t port, int players, const std::string &name, int net_delay) = 0;
    // Client: resolve the address and send HELLO until WELCOME, REJECT or cancel().
    virtual void join_begin(const std::string &address, const std::string &name) = 0;
    // Host lobby: Start (the session's player count is the connected count).
    virtual void host_start() = 0;
    // BYE to everyone, the UPnP mapping removed; the lobby is left.
    virtual void cancel() = 0;
    // The events since the last call, oldest first. Never blocks.
    virtual void poll(std::vector<NetEvent> &out) = 0;
};

// One line of the screen. selected: drawn highlighted (the host lobby's Start is only highlighted when it can
// be pressed, and a dim line is a caption the player cannot pick).
struct ViewLine
{
    std::string text;
    bool selected = false;
    bool dim = false;
};

// Everything the view needs for one frame: no game state, no SDL.
struct StartScreenView
{
    Screen screen = Screen::Menu;
    std::string title;
    std::vector<ViewLine> lines;      // the items, the field being edited, the player list
    std::string note;                 // under the list: the seconds, the countdown, the error reason
    std::string footnote;             // the join line, the UPnP failure line, the Windows firewall hint
    std::vector<LobbyPlayer> players; // the lobby's list (the local player first)
    bool can_start = false;           // the host lobby: Start is enabled (2 or more connected)

    // 7.3: the LocalSetup and Settings screens, so the view can draw its controls. The values mirror the
    // model's own; the view only reads them (the model changes them through key()).
    int local_players = 1;            // LocalSetup: the chosen count, 1..4
    bool settings_fullscreen = false;
    int settings_renderer = 0;        // 0 Vulkan, 1 software
    bool settings_vibration = true;
    int settings_net_delay = 0;       // 0 = auto, 1..10
    int settings_item = 0;            // Settings: 0 name, 1 fullscreen, 2 renderer, 3 vibration, 4 net delay, 5 port
    bool settings_name_editing = false;
    bool settings_port_editing = false;
};

enum class StartScreenResult
{
    None,
    PlayLocal, // the game starts as today
    Quit,      // the window was closed or Quit was chosen
    Net,       // the session started: the caller plays with the layer's Netplay (host() says which side)
};

class StartScreenModel
{
public:
    // What is remembered between runs (net.ini) plus what the caller knows.
    struct Remembered
    {
        std::string name;         // net.ini: the last name (empty = the OS user name)
        std::string address;      // net.ini: the last address
        int port = 7877;          // net.ini (7.3): the host's UDP port, 1024..65535
        int players = 2;          // net.ini: the last player count when hosting (2..4)
        int local_players = 1;    // net.ini (7.3): the last local player count (1..4)
        bool fullscreen = false;  // net.ini (7.3): the game window opens fullscreen
        int renderer = 0;         // net.ini (7.3): 0 Vulkan, 1 software
        bool vibration = true;    // net.ini (7.3): the game's vibration on the gamepads
        int net_delay = 0;        // net.ini (7.3): 0 = automatic, 1..10 frames (hosting)
        std::string default_name; // the OS user name, used when the name is left empty
        std::string build_id;     // shown on the menu so two players can compare (may be empty)
        bool show_firewall_hint = false; // the Windows lobby's firewall hint
    };

    StartScreenModel(StartScreenNet *net, const Remembered &remembered);

    // Input, one call per SDL event.
    void key(Key k);
    // SDL_EVENT_TEXT_INPUT: the printable ASCII of s is appended to the field being edited.
    void text(const std::string &s);
    // Once per frame (and in the tests, once per simulated frame): the timers and the layer's events.
    void tick(int ms);

    // The state for the view.
    const StartScreenView &view() const;

    // The outcome; only meaningful when result() != None. The name, the address and the player count are
    // what the caller writes back to net.ini.
    StartScreenResult result() const { return m_result; }
    bool host() const { return m_host; }
    int players() const { return m_session_players; }
    // The player count of the Host setup screen: what net.ini keeps between runs (unlike players(), which is
    // the count the session ended up with).
    int last_players() const { return m_players; }
    const std::string &name() const { return m_name; }
    const std::string &address() const { return m_address; }
    uint16_t port() const { return m_port; }

    // 7.3 (T-522): what the host applies to the game it starts. local_players() is the LocalSetup's choice
    // (1..4); fullscreen/renderer/vibration/net_delay are the Settings' values (net_delay 0 = automatic).
    int local_players() const { return m_local_players; }
    bool fullscreen() const { return m_fullscreen; }
    int renderer() const { return m_renderer; }  // 0 Vulkan, 1 software
    bool vibration() const { return m_vibration; }
    int net_delay() const { return m_net_delay; }

    // Only the tests and the view use these two; the caller writes the file (net_config_file.h).
    const std::string &field() const { return m_field; }

    // Section 7.2: "an empty or malformed address" is the model's own check. why = the reason, in English.
    static bool valid_address(const std::string &address, std::string &why);

    // Section 7.2, during play (the game window, no screen of its own): the players' numbering is 1-based
    // ("Player 3" is the wire's player 2), and these are the two reasons the card names. The caller shows them
    // (don_show_net_session_end) and exits with code 4.
    static std::string net_end_player_left(int player, const std::string &name); // "Player 3 (Ana) left the game"
    static std::string net_end_desync(uint32_t frame);                           // "The game went out of sync at frame N"

    // The same thing during play, from Netplay's own error line ("player 3 left", "desync at frame 120",
    // "host stopped sending", "timed out waiting for frame 400"...). An empty result means "no box": the
    // session was stopped on request ("stopped": a signal, the window closing), not by the network.
    // names[player] (wire numbering, may be empty) names a departing player.
    static std::string net_end_reason(const std::string &netplay_error,
                                      const std::vector<std::string> &names = {});

private:
    void handle_key(Key k);
    void on_event(const NetEvent &e);
    void fail(NetError error, const std::string &text, Screen back_to);
    void host_start_session();
    void add_player(int player, const std::string &name, int ping_ms);
    void drop_player(int player);
    int connected_count() const;
    bool is_full() const;
    void parse_address_field();
    void rebuild() const;

    StartScreenNet *m_net = nullptr;
    Remembered m_r;

    Screen m_screen = Screen::Menu;
    Screen m_error_from = Screen::Menu; // the Error state goes back to the state it came from
    int m_item = 0;
    std::string m_name;    // the committed name (never empty after NameEdit: the OS name stands in)
    std::string m_field;   // the name or the address being edited
    std::string m_address; // the last address that was attempted (remembered in net.ini)
    int m_players = 2;     // the player count chosen in Host setup / read from net.ini
    uint16_t m_port = 7877;

    // 7.3: the LocalSetup and Settings values (what the caller writes back to net.ini and applies).
    int m_local_players = 1;
    bool m_fullscreen = false;
    int m_renderer = 0;
    bool m_vibration = true;
    int m_net_delay = 0;
    int m_settings_item = 0;           // Settings: the selected control
    bool m_settings_name_editing = false; // Settings: the name field has focus
    bool m_settings_port_editing = false; // Settings: the host port field has focus

    // The host's lobby.
    std::vector<LobbyPlayer> m_lobby; // the clients only; the local player is added when drawing
    std::string m_local_ip;
    std::string m_external_ip;
    std::string m_upnp_note;   // the failure line, or empty
    bool m_upnp_mapped = false; // the router opened the port (7.2's "players can join at" line)
    std::string m_notice;      // a refused joiner, or other lobby news (shown for a few seconds)
    int m_notice_ms = -1;      // how much longer m_notice stays on screen (-1: none)
    std::string m_error_text;  // the Error screen's line (built by fail())
    bool m_bound = false;
    bool m_starting = false;
    int m_countdown_ms = -1; // >= 0: the full lobby starts by itself in this many ms
    int m_connecting_ms = 0;
    int m_local_player = 0; // the client's own player index arrives with WELCOME; the host is 0, -1 = unknown
    std::string m_host_name; // the client's host name (WELCOME; may be empty)

    StartScreenResult m_result = StartScreenResult::None;
    bool m_host = false;
    int m_session_players = 2;

    mutable StartScreenView m_view;
    mutable bool m_view_dirty = true;
};

} // namespace don_host
