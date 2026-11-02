// SPDX-License-Identifier: GPL-3.0-or-later
// The start screen's state machine (T-517): see start_screen_model.h. No SDL, no socket, no thread: the only
// outside world is StartScreenNet, which the unit test replaces with a fake.
#include "start_screen_model.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace don_host
{
namespace
{

constexpr int kConnectingTimeoutMs = 10000; // 7.2: "no answer in 10 s"
constexpr int kFullLobbyStartMs = 3000;     // 7.2: "full = start automatically after 3 s"
constexpr int kNoticeMs = 5000;             // 7.2: a lobby notice (a refused joiner) is shown this long
constexpr std::size_t kMaxNameLen = 15;     // 7.2: "1-15 printable ASCII"
constexpr std::size_t kMaxAddressLen = 63;

// The port the start screen hosts on: --net-host overrides it, nothing here remembers it (7.2 remembers the
// name, the last address and the last player count, nothing else).
constexpr uint16_t kDefaultPort = 7877;

bool printable_ascii(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 32 && u <= 126;
}

// The host port as typed: digits only and 1024..65535. False (and out untouched) otherwise.
bool valid_port_field(const std::string &f, uint16_t &out)
{
    if (f.empty() || f.size() > 5 || f.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const long n = std::strtol(f.c_str(), nullptr, 10);
    if (n < 1024 || n > 65535)
        return false;
    out = static_cast<uint16_t>(n);
    return true;
}

std::string trim_field(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

} // namespace

// The name that goes on the wire: what the player typed, else the OS user name, else "Player".
static std::string effective_name_of(const StartScreenModel::Remembered &r, const std::string &name)
{
    if (!name.empty())
        return name;
    return r.default_name.empty() ? std::string("Player") : r.default_name;
}

bool StartScreenModel::valid_address(const std::string &address, std::string &why)
{
    why.clear();
    if (address.empty())
    {
        why = "there is no address";
        return false;
    }
    if (address.size() > 253)
    {
        why = "it is too long";
        return false;
    }
    std::string host = address;
    const std::size_t colon = address.rfind(':');
    if (colon != std::string::npos)
    {
        host = address.substr(0, colon);
        const std::string port = address.substr(colon + 1);
        if (port.empty())
        {
            why = "there is nothing after the ':'";
            return false;
        }
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
        {
            why = "'" + port + "' is not a port number";
            return false;
        }
        const long n = std::strtol(port.c_str(), nullptr, 10);
        if (n < 1 || n > 65535)
        {
            why = "the port must be between 1 and 65535";
            return false;
        }
    }
    if (host.empty())
    {
        why = "there is no host name before the ':'";
        return false;
    }
    for (char c : host)
    {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
        if (!ok)
        {
            why = "'" + std::string(1, c) + "' cannot be in an address";
            return false;
        }
    }
    if (host.front() == '.' || host.front() == '-' || host.back() == '.' || host.back() == '-')
    {
        why = "it starts or ends with '.' or '-'";
        return false;
    }
    if (host.find("..") != std::string::npos || host.find("--") != std::string::npos)
    {
        why = "it has two dots or two dashes in a row";
        return false;
    }
    return true;
}

std::string StartScreenModel::net_end_player_left(int player, const std::string &name)
{
    std::string s = "Player " + std::to_string(player);
    if (!name.empty())
        s += " (" + name + ")";
    return s + " left the game.";
}

std::string StartScreenModel::net_end_desync(uint32_t frame)
{
    return "The game went out of sync at frame " + std::to_string(frame) + ".";
}

namespace
{
// The first number after `marker` ("player 3 left" -> 3), 0 when the line has none.
int number_after(const std::string &s, const std::string &marker)
{
    const std::size_t at = s.find(marker);
    if (at == std::string::npos)
        return 0;
    std::size_t i = at + marker.size();
    int n = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
    {
        any = true;
        n = n * 10 + (s[i] - '0');
        ++i;
    }
    return any ? n : 0;
}
} // namespace

std::string StartScreenModel::net_end_reason(const std::string &netplay_error, const std::vector<std::string> &names)
{
    const std::string line = trim_field(netplay_error);
    // "stopped": the session was stopped on request (a signal, the window closing, the game thread ending):
    // the player asked to leave and no box is shown.
    if (line.empty() || line == "stopped")
        return std::string();
    if (line.rfind("desync", 0) == 0)
    {
        // The host's line carries the frame ("desync at frame 120"); the client's now carries it too (the
        // DESYNC message's frame, which Netplay keeps). Without a frame, the reason is still named.
        const int frame = number_after(line, "frame ");
        if (frame > 0)
            return net_end_desync(static_cast<uint32_t>(frame));
        return "The game went out of sync.";
    }
    const bool silent = line.find("stopped sending") != std::string::npos;
    const bool left = line.find("left") != std::string::npos;
    // Netplay's line uses the wire index ("player 1 left" is the first client); the box's number is 1-based
    // with the host as "Player 1", so it is the wire index + 1, and the name is names[wire].
    const int wire = number_after(line, "player ");
    if (wire > 0 && (silent || left))
    {
        const std::string name =
            static_cast<std::size_t>(wire) < names.size() ? names[static_cast<std::size_t>(wire)]
                                                          : std::string();
        return net_end_player_left(wire + 1, name);
    }
    if (line.find("host") != std::string::npos && (silent || left))
        return "The host left the game.";
    // Anything else (a frame timeout, "not connected"): the layer's own line, so the reason is still named.
    return line.back() == '.' ? line : line + ".";
}

StartScreenModel::StartScreenModel(StartScreenNet *net, const Remembered &remembered)
    : m_net(net), m_r(remembered), m_port(kDefaultPort)
{
    m_name = trim_field(m_r.name);
    m_address = trim_field(m_r.address);
    m_port = (m_r.port >= 1024 && m_r.port <= 65535) ? static_cast<uint16_t>(m_r.port) : kDefaultPort;
    m_players = (m_r.players >= 2 && m_r.players <= 4) ? m_r.players : 2;
    m_local_players = (m_r.local_players >= 1 && m_r.local_players <= 4) ? m_r.local_players : 1;
    m_fullscreen = m_r.fullscreen;
    m_renderer = (m_r.renderer == 0 || m_r.renderer == 1) ? m_r.renderer : 0;
    m_vibration = m_r.vibration;
    m_net_delay = (m_r.net_delay >= 0 && m_r.net_delay <= 10) ? m_r.net_delay : 0;
}

// The name the model puts in the wire's HELLO comes from effective_name_of (above).
void StartScreenModel::key(Key k)
{
    m_view_dirty = true;
    if (k == Key::Quit)
    {
        m_result = StartScreenResult::Quit;
        m_screen = Screen::Done;
        return;
    }
    handle_key(k);
}

void StartScreenModel::handle_key(Key k)
{
    switch (m_screen)
    {
    case Screen::Menu:
    {
        constexpr int kItems = 5;
        if (k == Key::Up)
            m_item = (m_item + kItems - 1) % kItems;
        else if (k == Key::Down)
            m_item = (m_item + 1) % kItems;
        else if (k == Key::Confirm)
        {
            switch (m_item)
            {
            case 0: // Play: the local game at once; the game asks how many play, the view picks the pad layout
                m_result = StartScreenResult::PlayLocal;
                m_screen = Screen::Done;
                break;
            case 1: // Host online: straight to the lobby, open to 4; the host starts with whoever is there
                m_players = 4;
                m_screen = Screen::HostSetup;
                handle_key(Key::Confirm);
                break;
            case 2:
                m_field = m_address; // 7.2: the address field starts on the last one
                m_screen = Screen::JoinSetup;
                break;
            case 3: // Settings (7.3): the name, fullscreen, renderer, vibration, the net delay and the host port
                m_settings_item = 0;
                m_settings_name_editing = false;
                m_settings_port_editing = false;
                m_screen = Screen::Settings;
                break;
            default:
                m_result = StartScreenResult::Quit;
                m_screen = Screen::Done;
                break;
            }
        }
        else if (k == Key::Back) // there is no step back from the menu: like closing the window
        {
            m_result = StartScreenResult::Quit;
            m_screen = Screen::Done;
        }
        break;
    }
    case Screen::LocalSetup:
    {
        if (k == Key::Left)
            m_local_players = (m_local_players <= 1) ? 4 : m_local_players - 1;
        else if (k == Key::Right)
            m_local_players = (m_local_players >= 4) ? 1 : m_local_players + 1;
        else if (k == Key::Confirm)
        {
            m_result = StartScreenResult::PlayLocal;
            m_screen = Screen::Done;
        }
        else if (k == Key::Back)
            m_screen = Screen::Menu;
        break;
    }
    case Screen::Settings:
    {
        constexpr int kItems = 6; // name, fullscreen, renderer, vibration, net delay, host port
        if (m_settings_name_editing || m_settings_port_editing)
        {
            if (k == Key::Backspace)
            {
                if (!m_field.empty())
                    m_field.pop_back();
            }
            else if (k == Key::Confirm)
            {
                if (m_settings_name_editing)
                {
                    // Empty = the OS user name (7.2); otherwise what was typed.
                    m_name = (m_field == effective_name_of(m_r, m_name)) ? m_name : trim_field(m_field);
                }
                else if (m_settings_port_editing)
                {
                    uint16_t port = 0;
                    if (valid_port_field(m_field, port))
                        m_port = port; // invalid: the old value stays
                }
                m_settings_name_editing = false;
                m_settings_port_editing = false;
            }
            else if (k == Key::Back)
            {
                m_settings_name_editing = false;  // stop editing, stay in Settings
                m_settings_port_editing = false;
            }
            else if (k == Key::Left) // a gamepad-only player clears the field
                m_field.clear();
            break;
        }
        if (k == Key::Up)
            m_settings_item = (m_settings_item + kItems - 1) % kItems;
        else if (k == Key::Down)
            m_settings_item = (m_settings_item + 1) % kItems;
        else if (k == Key::Left || k == Key::Right)
        {
            const int dir = (k == Key::Right) ? 1 : -1;
            switch (m_settings_item)
            {
            case 0: // name: Confirm enters the field; Left/Right does nothing here
            case 5: // host port: typed, Confirm enters the field
                break;
            case 1:
                m_fullscreen = !m_fullscreen;
                break;
            case 2:
                m_renderer = (m_renderer == 0) ? 1 : 0;
                break;
            case 3:
                m_vibration = !m_vibration;
                break;
            case 4:
            {
                int d = m_net_delay + dir;
                if (d < 0)
                    d = 10;
                if (d > 10)
                    d = 0;
                m_net_delay = d;
                break;
            }
            }
        }
        else if (k == Key::Confirm)
        {
            if (m_settings_item == 0)
            {
                m_field = effective_name_of(m_r, m_name);
                m_settings_name_editing = true;
            }
            else if (m_settings_item == 5)
            {
                m_field = std::to_string(m_port);
                m_settings_port_editing = true;
            }
        }
        else if (k == Key::Back)
            m_screen = Screen::Menu; // 7.3: Back saves (the values are already in the members)
        break;
    }
    case Screen::NameEdit:
    {
        if (k == Key::Backspace)
        {
            if (!m_field.empty())
                m_field.pop_back();
        }
        else if (k == Key::Confirm)
        {
            // Empty = the OS user name (7.2); otherwise what was typed.
            m_name = (m_field == effective_name_of(m_r, m_name)) ? m_name : trim_field(m_field);
            m_screen = Screen::Menu;
        }
        else if (k == Key::Back)
            m_screen = Screen::Menu;
        else if (k == Key::Left || k == Key::Right) // a gamepad-only player clears the field with Left
        {
            if (k == Key::Left)
                m_field.clear();
        }
        break;
    }
    case Screen::HostSetup:
    {
        if (k == Key::Left)
            m_players = (m_players <= 2) ? 4 : m_players - 1;
        else if (k == Key::Right)
            m_players = (m_players >= 4) ? 2 : m_players + 1;
        else if (k == Key::Confirm)
        {
            m_lobby.clear();
            m_bound = false;
            m_starting = false;
            m_countdown_ms = -1;
            m_local_ip.clear();
            m_external_ip.clear();
            m_upnp_note.clear();
            m_upnp_mapped = false;
            m_notice.clear();
            m_screen = Screen::HostLobby;
            if (m_net)
                m_net->host_begin(m_port, m_players, effective_name_of(m_r, m_name), m_net_delay);
        }
        else if (k == Key::Back)
            m_screen = Screen::Menu;
        break;
    }
    case Screen::HostLobby:
    {
        if (k == Key::Confirm)
        {
            if (connected_count() >= 2 && !m_starting)
                host_start_session();
        }
        else if (k == Key::Back) // 7.2: cancelling the lobby sends BYE to everyone
        {
            if (m_net)
                m_net->cancel();
            m_lobby.clear();
            m_countdown_ms = -1;
            m_starting = false;
            m_screen = Screen::Menu;
        }
        break;
    }
    case Screen::JoinSetup:
    {
        if (k == Key::Backspace)
        {
            if (!m_field.empty())
                m_field.pop_back();
        }
        else if (k == Key::Left) // a gamepad-only player clears the field; Enter offers the last address
            m_field.clear();
        else if (k == Key::Confirm)
        {
            parse_address_field();
        }
        else if (k == Key::Back)
            m_screen = Screen::Menu;
        break;
    }
    case Screen::Connecting:
    {
        if (k == Key::Back || k == Key::Confirm) // 7.2: cancelling while connecting sends BYE
        {
            if (m_net)
                m_net->cancel();
            m_screen = Screen::JoinSetup;
        }
        break;
    }
    case Screen::ClientLobby:
    {
        if (k == Key::Back || k == Key::Confirm)
        {
            if (m_net)
                m_net->cancel();
            m_screen = Screen::JoinSetup;
        }
        break;
    }
    case Screen::Error:
    {
        if (k == Key::Confirm || k == Key::Back)
            m_screen = m_error_from; // 7.2: back to the state it came from
        break;
    }
    case Screen::Done:
        break;
    }
}

void StartScreenModel::parse_address_field()
{
    if (m_field.empty())
    {
        if (m_address.empty())
        {
            // 7.2: with only a gamepad the field offers the last address; with none there is nothing to offer.
            fail(NetError::BadAddress,
                 "Type the host's address: an IP address or a domain name, like 203.0.113.5 or "
                 "host.example.com (an optional :port follows).",
                 Screen::JoinSetup);
            return;
        }
        m_field = m_address;
    }
    std::string why;
    if (!valid_address(m_field, why))
    {
        fail(NetError::BadAddress, "That is not a valid address: " + why + ".", Screen::JoinSetup);
        return;
    }
    m_address = trim_field(m_field);
    m_connecting_ms = 0;
    m_host_name.clear();
    m_lobby.clear();
    m_notice.clear();
    m_screen = Screen::Connecting;
    if (m_net)
        m_net->join_begin(m_address, effective_name_of(m_r, m_name));
}

void StartScreenModel::host_start_session()
{
    m_starting = true;
    m_countdown_ms = -1;
    if (m_net)
        m_net->host_start();
}

void StartScreenModel::fail(NetError error, const std::string &text, Screen back_to)
{
    m_error_from = back_to;
    m_screen = Screen::Error;
    m_starting = false;
    m_countdown_ms = -1;
    m_error_text.clear();
    switch (error)
    {
    case NetError::PortInUse:
        m_error_text = "Cannot use UDP port " + std::to_string(m_port) + ": it is already in use" +
                       (text.empty() ? "" : " (" + text + ")") +
                       ". Close the other copy of the game, or another program using the port, and try again.";
        break;
    case NetError::NoIpv4Interface:
        m_error_text = "This PC has no network interface with an IPv4 address. Connect it to the network "
                       "(Wi-Fi or cable) and try again.";
        break;
    case NetError::BadAddress:
        m_error_text = text;
        break;
    case NetError::Resolve:
        m_error_text = "Cannot resolve " + m_address + ": check the address, or the host's domain name.";
        break;
    case NetError::NoAnswer:
        m_error_text = "Cannot reach " + m_address + ": is the host's port open?";
        break;
    case NetError::Reject:
        m_error_text = text;
        break;
    case NetError::HostLeft:
        m_error_text = "The host left the lobby.";
        break;
    case NetError::Other:
    default:
        m_error_text = text.empty() ? "The network stopped for an unknown reason." : text;
        break;
    }
}

void StartScreenModel::add_player(int player, const std::string &name, int ping_ms)
{
    std::string n = name.empty() ? ("Player " + std::to_string(player)) : trim_field(name);
    if (n.empty())
        n = "Player " + std::to_string(player);
    // 7.2: two players with the same name: the second is shown as "Name (2)".
    auto taken = [this](const std::string &c) {
        return std::any_of(m_lobby.begin(), m_lobby.end(), [&c](const LobbyPlayer &p) { return p.name == c; });
    };
    if (taken(n))
    {
        int k = 2;
        std::string c;
        do
        {
            c = n + " (" + std::to_string(k++) + ")";
        } while (taken(c));
        n = c;
    }
    for (LobbyPlayer &p : m_lobby)
    {
        if (p.player == player)
        {
            p.name = n;
            p.ping_ms = ping_ms;
            return;
        }
    }
    LobbyPlayer p;
    p.player = player;
    p.name = n;
    p.ping_ms = ping_ms;
    m_lobby.push_back(p);
}

void StartScreenModel::drop_player(int player)
{
    m_lobby.erase(std::remove_if(m_lobby.begin(), m_lobby.end(),
                                 [player](const LobbyPlayer &p) { return p.player == player; }),
                  m_lobby.end());
}

int StartScreenModel::connected_count() const { return 1 + static_cast<int>(m_lobby.size()); }

bool StartScreenModel::is_full() const { return connected_count() >= m_players; }

void StartScreenModel::text(const std::string &s)
{
    m_view_dirty = true;
    const bool name_field = (m_screen == Screen::NameEdit) ||
                            (m_screen == Screen::Settings && m_settings_name_editing);
    const bool port_field = (m_screen == Screen::Settings && m_settings_port_editing);
    if (!name_field && !port_field && m_screen != Screen::JoinSetup)
        return;
    const std::size_t max = name_field ? kMaxNameLen : (port_field ? 5 : kMaxAddressLen);
    for (char c : s)
    {
        if (m_field.size() >= max)
            break;
        if (port_field ? (c >= '0' && c <= '9') : printable_ascii(c))
            m_field.push_back(c);
    }
}

void StartScreenModel::tick(int ms)
{
    m_view_dirty = true;
    if (m_net)
    {
        std::vector<NetEvent> events;
        m_net->poll(events);
        for (const NetEvent &e : events)
            on_event(e);
    }
    if (m_screen == Screen::Connecting)
    {
        m_connecting_ms += ms;
        if (m_connecting_ms >= kConnectingTimeoutMs)
        {
            if (m_net)
                m_net->cancel();
            fail(NetError::NoAnswer, std::string(), Screen::JoinSetup);
        }
    }
    if (m_screen == Screen::HostLobby && m_countdown_ms >= 0)
    {
        m_countdown_ms -= ms;
        if (m_countdown_ms <= 0 && !m_starting)
        {
            host_start_session();
        }
    }
    if (!m_notice.empty() && m_notice_ms >= 0)
    {
        m_notice_ms -= ms;
        if (m_notice_ms < 0)
            m_notice.clear(); // 7.2: the notice stays for a few seconds, then the lobby returns to its own line
    }
}

void StartScreenModel::on_event(const NetEvent &e)
{
    m_view_dirty = true;
    switch (e.type)
    {
    case NetEvent::Type::HostBound:
        m_bound = true;
        if (!e.ip.empty())
            m_local_ip = e.ip;
        break;
    case NetEvent::Type::HostBindFailed:
        fail(e.error == NetError::Other ? NetError::PortInUse : e.error, e.text, Screen::Menu);
        break;
    case NetEvent::Type::UpnpMapped:
        m_external_ip = e.ip;
        m_upnp_mapped = true;
        if (!e.name.empty())
            m_local_ip = e.name; // the router's own view of this PC
        m_upnp_note.clear();
        break;
    case NetEvent::Type::UpnpFailed:
        m_upnp_mapped = false;
        if (!e.ip.empty())
            m_local_ip = e.ip;
        // 7.2: the router did not open the port (why), or it maps the port to another PC: either way the host
        // keeps going and the lobby shows the forward-by-hand hint with the local address.
        m_upnp_note = "The router did not open the port (" +
                      (e.text.empty() ? std::string("no reason given") : e.text) + "): forward UDP " +
                      std::to_string(m_port) + " to " +
                      (m_local_ip.empty() ? std::string("this PC") : m_local_ip) +
                      " once on the router.";
        break;
    case NetEvent::Type::PlayerJoined:
        add_player(e.player, e.name, e.ping_ms);
        if (is_full() && connected_count() >= 2)
            m_countdown_ms = kFullLobbyStartMs;
        break;
    case NetEvent::Type::PlayerLeft:
        drop_player(e.player);
        if (!is_full())
            m_countdown_ms = -1;
        break;
    case NetEvent::Type::PlayerNotice:
        m_notice = e.text;
        m_notice_ms = kNoticeMs;
        break;
    case NetEvent::Type::WelcomeReceived:
        m_host_name = e.name;
        // -1: the layer cannot say which index this PC has in the list it sends (it then sends every entry),
        // so the local player is one of them (rebuild()) instead of a line of its own.
        m_local_player = (e.player >= 0 && e.player < 4) ? e.player : -1;
        m_notice.clear();
        m_screen = Screen::ClientLobby;
        break;
    case NetEvent::Type::Rejected:
        switch (e.reason)
        {
        case 1:
            fail(NetError::Reject,
                 "The host runs another version: " + (e.text.empty() ? std::string("no id given") : e.text) + ".",
                 Screen::JoinSetup);
            break;
        case 2:
            fail(NetError::Reject, "The host uses other game data (another language variant).", Screen::JoinSetup);
            break;
        case 3:
            fail(NetError::Reject, "The game is full.", Screen::JoinSetup);
            break;
        case 4:
            fail(NetError::Reject, "The game has already started.", Screen::JoinSetup);
            break;
        default:
            fail(NetError::Reject, "The host refused the connection: " + e.text + ".", Screen::JoinSetup);
            break;
        }
        break;
    case NetEvent::Type::ConnectFailed:
        if (e.error == NetError::BadAddress)
            fail(NetError::BadAddress, e.text, Screen::JoinSetup);
        else
            fail(e.error, e.text, Screen::JoinSetup);
        break;
    case NetEvent::Type::HostLeft:
        fail(NetError::HostLeft, std::string(), Screen::JoinSetup);
        break;
    case NetEvent::Type::SessionStarted:
        m_host = (m_screen == Screen::HostLobby);
        m_session_players = (e.players >= 2) ? e.players : (m_host ? connected_count() : 2);
        m_result = StartScreenResult::Net;
        m_screen = Screen::Done;
        break;
    case NetEvent::Type::SessionFailed:
        // The Error screen goes back to the state it came from: the host setup when hosting, the address
        // otherwise.
        fail(e.error, e.text, (m_screen == Screen::HostLobby) ? Screen::Menu : Screen::JoinSetup);
        break;
    }
}

const StartScreenView &StartScreenModel::view() const
{
    if (m_view_dirty)
    {
        rebuild();
        m_view_dirty = false;
    }
    return m_view;
}

namespace
{
// The players' numbering on the screen is 1-based: the wire's player 0 (the host) is "Player 1".
std::string player_label(int player, const std::string &name, int ping_ms, bool local)
{
    std::string s = std::to_string(player + 1) + " " + name;
    if (local)
        return s + " (you)";
    s += "   ";
    s += (ping_ms >= 0) ? (std::to_string(ping_ms) + " ms") : "(ping not measured)";
    return s;
}
} // namespace

void StartScreenModel::rebuild() const
{
    StartScreenView &v = m_view;
    v = StartScreenView{};
    v.screen = m_screen;
    v.can_start = false;
    v.players.clear();

    switch (m_screen)
    {
    case Screen::Menu:
        v.title = "Battle Stadium D.O.N";
        v.lines = {{"Play", m_item == 0, false},
                   {"Host an online game", m_item == 1, false},
                   {"Join an online game", m_item == 2, false},
                   {"Settings", m_item == 3, false},
                   {"Quit", m_item == 4, false}};
        v.footnote = "Player: " + effective_name_of(m_r, m_name);
        if (!m_r.build_id.empty())
            v.footnote += std::string("\n") + m_r.build_id;
        break;
    case Screen::LocalSetup:
        v.title = "Play";
        v.local_players = m_local_players;
        v.lines = {{"Players: " + std::to_string(m_local_players), true, false}};
        v.note = "1 or 2 players: the keyboard and one gamepad. 3 or 4: through a multitap. "
                 "Enter: start. Esc: back.";
        break;
    case Screen::NameEdit:
        v.title = "Your name";
        v.lines = {{m_field + "_", true, false}};
        v.note = "1 to 15 characters. Empty uses the name of your PC user (\"" +
                 (m_r.default_name.empty() ? std::string("Player") : m_r.default_name) +
                 "\"). Enter: OK. Esc: back.";
        break;
    case Screen::HostSetup:
        v.title = "Host an online game";
        v.lines = {{"Players: " + std::to_string(m_players), true, false}};
        v.note = "Left/Right: 2, 3 or 4 players. Enter: open the lobby. Esc: back.";
        break;
    case Screen::HostLobby:
    {
        v.title = "Host an online game - lobby";
        LobbyPlayer local;
        local.player = 0;
        local.name = effective_name_of(m_r, m_name);
        local.ping_ms = -1;
        local.local = true;
        v.players.push_back(local);
        for (const LobbyPlayer &p : m_lobby)
            v.players.push_back(p);
        for (const LobbyPlayer &p : v.players)
            v.lines.push_back({player_label(p.player, p.name, p.ping_ms, p.local), false, true});
        v.can_start = connected_count() >= 2 && !m_starting;
        v.lines.push_back({"Start (" + std::to_string(connected_count()) + " of " +
                               std::to_string(m_players) + " players)",
                           v.can_start, !v.can_start});
        std::string footnote;
        if (!m_upnp_note.empty())
            footnote = m_upnp_note;
        else if (m_upnp_mapped && !m_external_ip.empty())
            footnote = "Players can join at " + m_external_ip + ":" + std::to_string(m_port) +
                       " (or your domain name).";
        else if (m_upnp_mapped)
            footnote = "The router opened UDP " + std::to_string(m_port) +
                       " but did not say the public address" +
                       (m_local_ip.empty() ? std::string() : std::string("; this PC is ") + m_local_ip) + ".";
        else if (m_bound && !m_local_ip.empty())
            footnote = "Players can join on your network at " + m_local_ip + ":" +
                       std::to_string(m_port) + "; the router did not answer, so put the public address "
                       "(or a domain) in yourself.";
        else
            footnote = "Opening UDP port " + std::to_string(m_port) + "...";
        if (m_r.show_firewall_hint)
            footnote += std::string(footnote.empty() ? "" : "\n") +
                        "Windows: allow the game in the firewall prompt (private networks), or nobody can "
                        "join.";
        v.footnote = footnote;
        if (m_countdown_ms >= 0)
            v.note = "The lobby is full: starting in " + std::to_string((m_countdown_ms + 999) / 1000) + " s.";
        else if (m_starting)
            v.note = "Starting the game...";
        else
            v.note = "Enter: start with the players that are here (2 or more). Esc: close the lobby.";
        if (!m_notice.empty())
            v.note = m_notice + "\n" + v.note;
        break;
    }
    case Screen::JoinSetup:
        v.title = "Join an online game";
        v.lines = {{"Address: " + m_field + "_", true, false}};
        v.note = "Type the host's address (an IP address or a domain name, optional :port). Enter: connect. "
                 "Esc: back.";
        break;
    case Screen::Connecting:
        v.title = "Join an online game";
        v.lines = {{"Connecting to " + m_address + "...", false, false}};
        v.note = std::to_string(m_connecting_ms / 1000) + " s (Esc: cancel)";
        break;
    case Screen::ClientLobby:
    {
        v.title = "Join an online game - lobby";
        if (m_local_player >= 0)
        {
            LobbyPlayer local;
            local.player = m_local_player; // the index WELCOME gave this PC
            local.name = effective_name_of(m_r, m_name);
            local.ping_ms = -1;
            local.local = true;
            v.players.push_back(local);
        }
        for (const LobbyPlayer &p : m_lobby)
            v.players.push_back(p);
        for (const LobbyPlayer &p : v.players)
            v.lines.push_back({player_label(p.player, p.name, p.ping_ms, p.local), false, true});
        v.note = "Connected to " + (m_host_name.empty() ? std::string("the host") : m_host_name) +
                 ". Waiting for the host to start.";
        if (!m_notice.empty())
            v.note = m_notice + "\n" + v.note;
        break;
    }
    case Screen::Settings:
    {
        v.title = "Settings";
        v.settings_fullscreen = m_fullscreen;
        v.settings_renderer = m_renderer;
        v.settings_vibration = m_vibration;
        v.settings_net_delay = m_net_delay;
        v.settings_item = m_settings_item;
        v.settings_name_editing = m_settings_name_editing;
        v.settings_port_editing = m_settings_port_editing;
        const std::string shown_name =
            m_settings_name_editing ? m_field + "_" : effective_name_of(m_r, m_name);
        const std::string delay_text = (m_net_delay == 0) ? "Auto" : std::to_string(m_net_delay) + " frame" +
                                                                   (m_net_delay == 1 ? "" : "s");
        const std::string port_text = m_settings_port_editing ? m_field + "_" : std::to_string(m_port);
        v.lines = {{"Name: " + shown_name, m_settings_item == 0, false},
                   {"Fullscreen: " + std::string(m_fullscreen ? "On" : "Off"), m_settings_item == 1, false},
                   {"Renderer: " + std::string(m_renderer == 0 ? "Vulkan" : "Software"), m_settings_item == 2, false},
                   {"Vibration: " + std::string(m_vibration ? "On" : "Off"), m_settings_item == 3, false},
                   {"Online input delay: " + delay_text, m_settings_item == 4, false},
                   {"Host port: " + port_text, m_settings_item == 5, false}};
        v.note = "Up/Down: pick a setting. Left/Right: change it. Enter: edit. Esc: back (saves).";
        break;
    }
    case Screen::Error:
        v.title = "Could not continue";
        v.lines = {{"Back", true, false}};
        v.note = m_error_text;
        break;
    case Screen::Done:
        break;
    }
}

} // namespace don_host
