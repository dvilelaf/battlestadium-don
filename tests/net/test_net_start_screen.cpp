// The start screen's state machine (T-517, docs/knowledge/22-netplay.md section 7.2): every state and every
// case listed there, driven with a fake netplay/UPnP layer so none of it needs a network, a window or the game.
//
// The model lives in src/host/ and this project's CMake globs only src/host/net/*.cpp into don_net, so the test
// compiles the two sources directly (they include no SDL and no runtime header exactly so that it can).
#include "../../src/host/net_config_file.cpp"
#include "../../src/host/start_screen_model.cpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "check.h"

using namespace don_host;

namespace
{

// The fake layer: it records what the model asked for and hands back the events the test queued.
class FakeNet final : public StartScreenNet
{
public:
    void host_begin(uint16_t port, int players, const std::string &name, int net_delay) override
    {
        host_begins++;
        host_port = port;
        host_players = players;
        host_name = name;
        host_delay = net_delay;
        log.push_back("host_begin");
    }
    void join_begin(const std::string &address, const std::string &name) override
    {
        join_begins++;
        join_address = address;
        join_name = name;
        log.push_back("join_begin");
    }
    void host_start() override
    {
        starts++;
        log.push_back("host_start");
    }
    void cancel() override
    {
        cancels++;
        log.push_back("cancel");
    }
    void poll(std::vector<NetEvent> &out) override
    {
        out.insert(out.end(), queue.begin(), queue.end());
        queue.clear();
    }

    void push(const NetEvent &e) { queue.push_back(e); }

    int host_begins = 0, join_begins = 0, starts = 0, cancels = 0;
    uint16_t host_port = 0;
    int host_players = 0;
    int host_delay = 0;
    std::string host_name;
    std::string join_address, join_name;
    std::vector<std::string> log;
    std::vector<NetEvent> queue;
};

NetEvent event(NetEvent::Type t)
{
    NetEvent e;
    e.type = t;
    return e;
}

NetEvent joined(int player, const std::string &name, int ping_ms = -1)
{
    NetEvent e = event(NetEvent::Type::PlayerJoined);
    e.player = player;
    e.name = name;
    e.ping_ms = ping_ms;
    return e;
}

struct Fixture
{
    Fixture()
    {
        r.default_name = "dave";
        r.build_id = "0123456789ab-dirty-noffmpeg";
        r.players = 2;
        model = std::make_unique<StartScreenModel>(&net, r);
    }

    const StartScreenView &v() { return model->view(); }
    void key(Key k) { model->key(k); }
    void tick(int ms) { model->tick(ms); }
    void settle() { model->tick(0); } // drains the fake's queue without advancing a timer
    // Runs a whole frame the way the view does: the layer's events first, then the model's timers.
    void frame(int ms)
    {
        model->tick(ms);
    }

    FakeNet net;
    StartScreenModel::Remembered r;
    std::unique_ptr<StartScreenModel> model;
};

bool has(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Picks a menu item by its line: the menu remembers the cursor, so tests never count Down presses.
void menu_item(Fixture &f, int index)
{
    for (int guard = 0; guard < 8 && !f.v().lines[static_cast<std::size_t>(index)].selected; ++guard)
        f.key(Key::Down);
    CHECK(f.v().lines[static_cast<std::size_t>(index)].selected);
}

// Walks the menu to the host's lobby: Host -> Enter (the lobby opens at once, 4 players).
void go_to_host_lobby(Fixture &f)
{
    menu_item(f, 1);
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::HostLobby);
}

// Walks the menu to the join setup with the given address typed in.
void go_to_join_setup(Fixture &f, const std::string &address)
{
    menu_item(f, 2);
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::JoinSetup);
    f.model->text(address);
}

// Walks the menu to the Settings screen.
void go_to_settings(Fixture &f)
{
    menu_item(f, 3);
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::Settings);
}

// Walks the menu to the name field of the Settings screen and starts editing it.
void go_to_name_edit(Fixture &f)
{
    go_to_settings(f);
    CHECK_EQ(f.v().settings_item, 0); // the name is the first control
    f.key(Key::Confirm);              // enter the name field
    CHECK(f.v().settings_name_editing);
}

// ---------------------------------------------------------------------------------------------------------
// The menu

void test_menu()
{
    Fixture f;
    const StartScreenView &v = f.v();
    CHECK(v.screen == Screen::Menu);
    CHECK(v.title == "Battle Stadium D.O.N");
    CHECK_EQ(v.lines.size(), 5);
    CHECK(v.lines[3].text == "Settings"); // 7.3: the name moved into Settings
    CHECK(has(v.footnote, "Player: dave")); // empty name = the OS user name
    CHECK(has(v.footnote, "0123456789ab-dirty-noffmpeg")); // two players compare builds here
    CHECK(v.lines[0].selected);

    f.key(Key::Down);
    CHECK(f.v().lines[1].selected);
    f.key(Key::Up);
    f.key(Key::Up); // wraps to the last item
    CHECK(f.v().lines[4].selected);
    f.key(Key::Confirm);
    CHECK(f.model->result() == StartScreenResult::Quit);
    CHECK(f.v().screen == Screen::Done);
}

void test_menu_play_local()
{
    Fixture f;
    f.key(Key::Confirm); // Play -> the local game at once
    CHECK(f.model->result() == StartScreenResult::PlayLocal);
    CHECK(f.v().screen == Screen::Done);
}

void test_window_close_quits_from_every_screen()
{
    const Screen screens[] = {Screen::Menu,      Screen::Settings,    Screen::HostLobby, Screen::JoinSetup,
                              Screen::Connecting, Screen::ClientLobby, Screen::Error};
    for (Screen s : screens)
    {
        Fixture f;
        switch (s)
        {
        case Screen::Menu:
            break;
        case Screen::Settings:
            go_to_settings(f);
            break;
        case Screen::HostLobby:
            go_to_host_lobby(f);
            break;
        case Screen::JoinSetup:
            go_to_join_setup(f, "");
            break;
        case Screen::Connecting:
            go_to_join_setup(f, "127.0.0.1");
            f.key(Key::Confirm);
            break;
        case Screen::ClientLobby:
        {
            go_to_join_setup(f, "127.0.0.1");
            f.key(Key::Confirm);
            f.net.push(event(NetEvent::Type::WelcomeReceived));
            f.settle();
            break;
        }
        case Screen::Error:
            go_to_join_setup(f, "not a host!");
            f.key(Key::Confirm);
            break;
        default:
            break;
        }
        CHECK(f.v().screen == s);
        f.key(Key::Quit); // closing the window quits from anywhere
        CHECK(f.model->result() == StartScreenResult::Quit);
        CHECK(f.v().screen == Screen::Done);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The name

void test_name_edit()
{
    Fixture f;
    go_to_name_edit(f);
    CHECK(has(f.v().lines[0].text, "Name: dave_")); // the field starts on what the menu shows

    f.key(Key::Left); // clears the field (a gamepad-only player)
    CHECK(f.model->field().empty());
    f.model->text("Ana\n\t1");           // the non-printable characters are dropped
    CHECK(f.model->field() == "Ana1");
    f.key(Key::Backspace);
    CHECK(f.model->field() == "Ana");
    f.model->text("0123456789abcdefgh"); // 15 characters at most
    CHECK_EQ(static_cast<int>(f.model->field().size()), 15);
    f.key(Key::Confirm); // commit the name; Settings stays open
    CHECK(f.v().screen == Screen::Settings);
    CHECK(f.model->name() == f.model->field());
    CHECK(f.model->name().size() == 15);
    CHECK(has(f.v().lines[0].text, "Name: "));
    f.key(Key::Back); // back saves, to the menu
    CHECK(f.v().screen == Screen::Menu);
    CHECK(has(f.v().footnote, "Player: "));
}

void test_name_edit_empty_is_the_os_user_name()
{
    Fixture f;
    go_to_name_edit(f);
    f.key(Key::Left);
    f.key(Key::Confirm); // empty: the OS user name stands in
    CHECK(f.model->name().empty());
    CHECK(has(f.v().lines[0].text, "Name: dave"));

    // The name that goes on the wire is the OS one, never empty.
    f.key(Key::Back); // Settings -> menu
    menu_item(f, 1);  // Host
    f.key(Key::Confirm);
    CHECK(f.net.host_name == "dave");
}

void test_name_edit_escape_discards()
{
    Fixture f;
    go_to_name_edit(f);
    f.key(Key::Left);
    f.model->text("Zoe");
    f.key(Key::Back); // stop editing the name (nothing is remembered), stay in Settings
    CHECK(f.v().screen == Screen::Settings);
    CHECK(!f.v().settings_name_editing);
    CHECK(f.model->name().empty());
    CHECK(has(f.v().lines[0].text, "Name: dave"));
    f.key(Key::Back); // Settings -> menu
    CHECK(f.v().screen == Screen::Menu);
}

// ---------------------------------------------------------------------------------------------------------
// Settings (7.3)

void test_settings_values_and_back_saves()
{
    Fixture f;
    f.r.fullscreen = true;
    f.r.renderer = 1;
    f.r.vibration = false;
    f.r.net_delay = 3;
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_settings(f);
    CHECK(f.v().screen == Screen::Settings);
    CHECK(f.model->fullscreen());
    CHECK_EQ(f.model->renderer(), 1);
    CHECK(!f.model->vibration());
    CHECK_EQ(f.model->net_delay(), 3);
    CHECK_EQ(f.v().settings_item, 0);

    // Fullscreen (item 1): toggle.
    f.key(Key::Down);
    CHECK_EQ(f.v().settings_item, 1);
    f.key(Key::Right);
    CHECK(!f.model->fullscreen());
    f.key(Key::Left);
    CHECK(f.model->fullscreen());

    // Renderer (item 2): cycle software -> vulkan -> software.
    f.key(Key::Down);
    CHECK_EQ(f.v().settings_item, 2);
    f.key(Key::Right);
    CHECK_EQ(f.model->renderer(), 0);
    f.key(Key::Right);
    CHECK_EQ(f.model->renderer(), 1);

    // Vibration (item 3): toggle.
    f.key(Key::Down);
    CHECK_EQ(f.v().settings_item, 3);
    f.key(Key::Right);
    CHECK(f.model->vibration());

    // Net delay (item 4): cycle, wrapping at both ends.
    f.key(Key::Down);
    CHECK_EQ(f.v().settings_item, 4);
    CHECK_EQ(f.model->net_delay(), 3);
    f.key(Key::Left);
    CHECK_EQ(f.model->net_delay(), 2);
    f.key(Key::Left);
    CHECK_EQ(f.model->net_delay(), 1);
    f.key(Key::Left);
    CHECK_EQ(f.model->net_delay(), 0); // auto
    f.key(Key::Left);
    CHECK_EQ(f.model->net_delay(), 10);
    f.key(Key::Right);
    CHECK_EQ(f.model->net_delay(), 0);

    // Back saves: the values are committed in the model (the caller writes net.ini).
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::Menu);
    CHECK(f.model->fullscreen());
    CHECK_EQ(f.model->renderer(), 1);
    CHECK(f.model->vibration());
    CHECK_EQ(f.model->net_delay(), 0);
}

void test_settings_net_delay_reaches_the_host()
{
    Fixture f;
    f.r.net_delay = 5;
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_settings(f);
    f.key(Key::Back); // save and leave
    menu_item(f, 1);  // Host
    f.key(Key::Confirm);
    CHECK_EQ(f.net.host_delay, 5);
}

void test_settings_host_port()
{
    Fixture f;
    go_to_settings(f);
    CHECK_EQ(f.model->port(), 7877); // the default

    // Move to the HOST PORT row (item 5) and edit it.
    for (int i = 0; i < 5; ++i)
        f.key(Key::Down);
    CHECK_EQ(f.v().settings_item, 5);
    f.key(Key::Confirm); // enter the field
    CHECK(f.v().settings_port_editing);
    f.key(Key::Left); // clear the field (a gamepad-only player)
    CHECK(f.model->field().empty());
    f.model->text("9000");
    f.key(Key::Confirm); // commit
    CHECK_EQ(f.model->port(), 9000);

    // An invalid port keeps the old value.
    f.key(Key::Confirm); // enter again
    f.key(Key::Left);    // clear
    f.model->text("80"); // below 1024
    f.key(Key::Confirm);
    CHECK_EQ(f.model->port(), 9000);

    // Back saves; hosting uses the edited port.
    f.key(Key::Back);
    menu_item(f, 1);
    f.key(Key::Confirm);
    CHECK_EQ(f.net.host_port, 9000);
}

// ---------------------------------------------------------------------------------------------------------
// Host begin

void test_host_begin_four_players()
{
    Fixture f;
    f.r.players = 3; // net.ini's old value is ignored: HOST ONLINE always opens for 4
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    menu_item(f, 1);
    f.key(Key::Confirm); // Host -> the lobby at once (bind + UPnP)
    CHECK(f.net.host_begins == 1);
    CHECK_EQ(f.net.host_port, 7877);
    CHECK_EQ(f.net.host_players, 4); // up to 4 players (3 others)
    CHECK(f.net.host_name == "dave");
    CHECK(f.v().screen == Screen::HostLobby);
    CHECK(has(f.v().footnote, "Opening UDP port 7877"));
    CHECK(!f.v().can_start); // 1 player: not enough
}

// ---------------------------------------------------------------------------------------------------------
// The host's lobby

void test_host_lobby_bound_and_upnp_mapped()
{
    Fixture f;
    go_to_host_lobby(f);
    NetEvent bound = event(NetEvent::Type::HostBound);
    bound.ip = "192.168.50.20";
    f.net.push(bound);
    f.settle();
    CHECK(has(f.v().footnote, "192.168.50.20:7877"));

    NetEvent mapped = event(NetEvent::Type::UpnpMapped);
    mapped.ip = "203.0.113.9";
    f.net.push(mapped);
    f.settle();
    CHECK(has(f.v().footnote, "Players can join at 203.0.113.9:7877"));
}

void test_host_lobby_upnp_failed_shows_the_forward_hint()
{
    Fixture f;
    go_to_host_lobby(f);
    NetEvent failed = event(NetEvent::Type::UpnpFailed);
    failed.text = "no UPnP device found";
    failed.ip = "192.168.50.20";
    f.net.push(failed);
    f.settle();
    CHECK(has(f.v().footnote, "The router did not open the port (no UPnP device found)"));
    CHECK(has(f.v().footnote, "forward UDP 7877 to 192.168.50.20 once on the router"));

    // The router maps the port to another PC: the same line, with the router's reason in the brackets.
    Fixture g;
    go_to_host_lobby(g);
    NetEvent other = event(NetEvent::Type::UpnpFailed);
    other.text = "the router maps 7877 to 192.168.50.31";
    other.ip = "192.168.50.20";
    g.net.push(other);
    g.settle();
    CHECK(has(g.v().footnote, "the router maps 7877 to 192.168.50.31"));
    CHECK(has(g.v().footnote, "forward UDP 7877 to 192.168.50.20"));
}

void test_host_lobby_windows_firewall_hint()
{
    Fixture f;
    f.r.show_firewall_hint = true;
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_host_lobby(f);
    NetEvent bound = event(NetEvent::Type::HostBound);
    bound.ip = "10.20.30.4";
    f.net.push(bound);
    f.settle();
    CHECK(has(f.v().footnote, "Windows: allow the game in the firewall prompt"));
    CHECK(has(f.v().footnote, "10.20.30.4:7877"));
}

void test_host_lobby_players_start_and_auto_start()
{
    Fixture f;
    f.r.players = 2; // ignored: the lobby always opens for 4
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_host_lobby(f);
    f.net.push(joined(1, "Ana", 21));
    f.settle();
    const StartScreenView &v = f.v();
    CHECK_EQ(static_cast<int>(v.players.size()), 2); // the host and Ana
    CHECK(v.players[0].local);
    CHECK(!v.players[1].local);
    CHECK(v.players[1].ping_ms == 21);
    CHECK(has(v.lines[1].text, "Ana"));
    CHECK(has(v.lines[1].text, "21 ms"));
    CHECK(v.can_start); // 2 connected: Start is enabled
    CHECK(has(v.lines[2].text, "Start (2 of 4 players)"));
    CHECK(!has(v.note, "starting in")); // 2 players: not full, no auto-start

    // Full (4 connected) starts by itself after 3 s.
    f.net.push(joined(2, "Bob", 12));
    f.net.push(joined(3, "Cid", 8));
    f.settle();
    CHECK(has(f.v().note, "starting in 3 s"));
    f.frame(2000);
    CHECK(f.net.starts == 0); // not yet
    CHECK(has(f.v().note, "starting in 1 s"));
    f.frame(1000);
    CHECK(f.net.starts == 1); // started by itself
    CHECK(has(f.v().note, "Starting the game..."));

    NetEvent started = event(NetEvent::Type::SessionStarted);
    started.players = 4;
    f.net.push(started);
    f.settle();
    CHECK(f.model->result() == StartScreenResult::Net);
    CHECK(f.model->host());
    CHECK_EQ(f.model->players(), 4);
}

void test_host_lobby_start_with_two_of_four()
{
    Fixture f;
    f.r.players = 4;
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_host_lobby(f);
    f.net.push(joined(1, "Ana"));
    f.settle();
    CHECK(f.v().can_start);
    CHECK(has(f.v().note, "start with the players that are here"));
    f.key(Key::Confirm); // Start with 2 of 4: the session's count is the connected count
    CHECK(f.net.starts == 1);
    NetEvent started = event(NetEvent::Type::SessionStarted);
    started.players = 2;
    f.net.push(started);
    f.settle();
    CHECK(f.model->result() == StartScreenResult::Net);
    CHECK_EQ(f.model->players(), 2);
}

void test_host_lobby_same_names_and_leaving()
{
    Fixture f;
    f.r.players = 4;
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    go_to_host_lobby(f);
    f.net.push(joined(1, "Ana", 10));
    f.net.push(joined(2, "Ana", 30));
    f.net.push(joined(3, "Ana", 40));
    f.settle();
    CHECK(has(f.v().lines[1].text, "Ana "));
    CHECK(has(f.v().lines[2].text, "Ana (2)"));
    CHECK(has(f.v().lines[3].text, "Ana (3)"));

    // A client leaving the lobby is removed from the list.
    NetEvent left = event(NetEvent::Type::PlayerLeft);
    left.player = 2;
    f.net.push(left);
    f.settle();
    CHECK_EQ(static_cast<int>(f.v().players.size()), 3);
    CHECK(has(f.v().lines[2].text, "Ana (3)"));
    CHECK(!has(f.v().lines[2].text, "Ana (2)"));
    CHECK(f.v().can_start); // 3 connected is still enough to start by hand
    CHECK(!has(f.v().note, "starting in")); // but the 4-player lobby is not full any more
}

void test_host_lobby_notice_for_a_refused_client()
{
    Fixture f;
    go_to_host_lobby(f);
    NetEvent notice = event(NetEvent::Type::PlayerNotice);
    notice.text = "Refused Ana: another build 1111aaaa-ffmpeg (this host runs 2222bbbb-noffmpeg)";
    f.net.push(notice);
    f.settle();
    CHECK(has(f.v().note, "Refused Ana: another build 1111aaaa-ffmpeg"));
    CHECK(has(f.v().note, "2222bbbb-noffmpeg"));
    CHECK_EQ(static_cast<int>(f.v().players.size()), 1); // refused: not in the list
}

void test_host_lobby_notice_fades_after_a_few_seconds()
{
    Fixture f;
    go_to_host_lobby(f);
    NetEvent notice = event(NetEvent::Type::PlayerNotice);
    notice.text = "Refused Bob: the game is full";
    f.net.push(notice);
    f.settle();
    CHECK(has(f.v().note, "Refused Bob: the game is full"));

    f.frame(4000); // under the 5 s the notice stays up
    CHECK(has(f.v().note, "Refused Bob"));
    f.frame(1001); // past 5 s it is gone, the lobby shows its own line again
    CHECK(!has(f.v().note, "Refused Bob"));
    CHECK(has(f.v().note, "start with the players that are here"));
}

void test_host_lobby_cancel_sends_bye()
{
    Fixture f;
    go_to_host_lobby(f);
    f.net.push(joined(1, "Ana"));
    f.settle();
    f.key(Key::Back); // one step back: the menu, and everyone gets BYE
    CHECK(f.net.cancels == 1);
    CHECK(f.v().screen == Screen::Menu);
}

void test_host_bind_failures()
{
    // The port is in use (another copy of the game, or another program).
    Fixture f;
    go_to_host_lobby(f);
    NetEvent bad = event(NetEvent::Type::HostBindFailed);
    bad.error = NetError::PortInUse;
    bad.text = "address already in use";
    f.net.push(bad);
    f.settle();
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "Cannot use UDP port 7877"));
    CHECK(has(f.v().note, "already in use"));
    CHECK(has(f.v().note, "address already in use"));
    CHECK(has(f.v().lines[0].text, "Back"));
    f.key(Key::Confirm); // back to the state it came from: the menu
    CHECK(f.v().screen == Screen::Menu);

    // This PC has no IPv4 interface.
    Fixture g;
    go_to_host_lobby(g);
    NetEvent no_ip = event(NetEvent::Type::HostBindFailed);
    no_ip.error = NetError::NoIpv4Interface;
    g.net.push(no_ip);
    g.settle();
    CHECK(g.v().screen == Screen::Error);
    CHECK(has(g.v().note, "no network interface with an IPv4 address"));
    g.key(Key::Back);
    CHECK(g.v().screen == Screen::Menu);
}

// ---------------------------------------------------------------------------------------------------------
// Joining

void test_valid_address()
{
    std::string why;
    CHECK(StartScreenModel::valid_address("127.0.0.1", why));
    CHECK(StartScreenModel::valid_address("example.com", why));
    CHECK(StartScreenModel::valid_address("example.com:7877", why));
    CHECK(StartScreenModel::valid_address("don-host.lan:1", why));
    CHECK(StartScreenModel::valid_address("203.0.113.5:65535", why));

    CHECK(!StartScreenModel::valid_address("", why));
    CHECK(!why.empty());
    CHECK(!StartScreenModel::valid_address("1.2.3.4:0", why));
    CHECK(!StartScreenModel::valid_address("1.2.3.4:70000", why));
    CHECK(!StartScreenModel::valid_address("1.2.3.4:abc", why));
    CHECK(!StartScreenModel::valid_address("1.2.3.4:", why));
    CHECK(!StartScreenModel::valid_address(":7877", why));
    CHECK(!StartScreenModel::valid_address("a..b", why));
    CHECK(!StartScreenModel::valid_address("a b", why));
    CHECK(!StartScreenModel::valid_address("http://example.com", why));
}

void test_join_setup_empty_and_malformed()
{
    Fixture f;
    go_to_join_setup(f, "");
    f.key(Key::Confirm); // nothing typed and nothing remembered
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "Type the host's address"));
    CHECK(f.net.join_begins == 0);
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::JoinSetup);

    f.model->text("127.0.0.1:99999");
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "That is not a valid address"));
    CHECK(has(f.v().note, "port must be between 1 and 65535"));
    CHECK(f.net.join_begins == 0);
}

void test_join_setup_ok_and_connecting()
{
    Fixture f;
    go_to_join_setup(f, "example.com:9000");
    f.key(Key::Confirm);
    CHECK(f.net.join_begins == 1);
    CHECK(f.net.join_address == "example.com:9000");
    CHECK(f.net.join_name == "dave");
    CHECK(f.v().screen == Screen::Connecting);
    CHECK(has(f.v().lines[0].text, "Connecting to example.com:9000"));
    CHECK(has(f.v().note, "0 s"));
    f.frame(4000);
    CHECK(has(f.v().note, "4 s")); // the seconds are shown while it waits
    CHECK(f.v().screen == Screen::Connecting);
}

void test_join_setup_offers_the_last_address()
{
    Fixture f;
    f.r.address = "198.51.100.7:7877"; // from net.ini: the field starts on it
    f.model = std::make_unique<StartScreenModel>(&f.net, f.r);
    menu_item(f, 2);
    f.key(Key::Confirm);
    CHECK(has(f.v().lines[0].text, "198.51.100.7:7877"));
    f.key(Key::Left); // only a gamepad: the field is cleared and Enter offers the last address again
    CHECK(f.model->field().empty());
    f.key(Key::Confirm);
    CHECK(f.net.join_address == "198.51.100.7:7877");
    CHECK(f.v().screen == Screen::Connecting);
}

void test_connecting_timeout_after_10_seconds()
{
    Fixture f;
    go_to_join_setup(f, "203.0.113.5");
    f.key(Key::Confirm);
    for (int i = 0; i < 9; ++i) // 9 s: still trying
        f.frame(1000);
    CHECK(f.v().screen == Screen::Connecting);
    CHECK(f.net.cancels == 0);
    f.frame(999); // just under 10 s
    CHECK(f.v().screen == Screen::Connecting);
    f.frame(1);
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "Cannot reach 203.0.113.5: is the host's port open?"));
    CHECK_EQ(f.net.cancels, 1); // the attempt is called off
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::JoinSetup);
}

void test_connecting_cancel_sends_bye()
{
    Fixture f;
    go_to_join_setup(f, "127.0.0.1");
    f.key(Key::Confirm);
    f.key(Key::Back);
    CHECK(f.net.cancels == 1);
    CHECK(f.v().screen == Screen::JoinSetup);
}

void test_connecting_name_does_not_resolve()
{
    Fixture f;
    go_to_join_setup(f, "no-such-host.example");
    f.key(Key::Confirm);
    NetEvent e = event(NetEvent::Type::ConnectFailed);
    e.error = NetError::Resolve;
    f.net.push(e);
    f.settle();
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "Cannot resolve no-such-host.example"));
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::JoinSetup);
}

void test_client_rejects()
{
    struct Case
    {
        int reason;
        const char *text;
    };
    const Case cases[] = {{1, "The host runs another version: 2222bbbb-noffmpeg."},
                          {2, "The host uses other game data (another language variant)."},
                          {3, "The game is full."},
                          {4, "The game has already started."}};
    for (const Case &c : cases)
    {
        Fixture f;
        go_to_join_setup(f, "127.0.0.1");
        f.key(Key::Confirm);
        NetEvent e = event(NetEvent::Type::Rejected);
        e.reason = c.reason;
        e.text = "2222bbbb-noffmpeg";
        f.net.push(e);
        f.settle();
        CHECK(f.v().screen == Screen::Error);
        CHECK(f.v().note == std::string(c.text));
    }
}

void test_client_lobby_and_host_leaving()
{
    Fixture f;
    go_to_join_setup(f, "127.0.0.1");
    f.key(Key::Confirm);
    NetEvent welcome = event(NetEvent::Type::WelcomeReceived);
    welcome.name = "Ana";
    welcome.players = 2;
    welcome.player = 1; // WELCOME's player: this PC is player 1 in the session
    f.net.push(welcome);
    NetEvent other = joined(1, "Bob", 12);
    other.player = 1;
    f.net.push(other);
    f.settle();
    CHECK(f.v().screen == Screen::ClientLobby);
    CHECK(has(f.v().note, "Connected to Ana. Waiting for the host to start."));
    CHECK_EQ(static_cast<int>(f.v().players.size()), 2);
    CHECK_EQ(f.v().players[0].player, 1); // the client's own entry carries its WELCOME index
    CHECK(has(f.v().lines[0].text, "2 dave (you)")); // shown 1-based: the wire's player 1 is "Player 2"

    // The host leaves the lobby: the client's error names it, and Back returns to the address.
    f.net.push(event(NetEvent::Type::HostLeft));
    f.settle();
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "The host left the lobby"));
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::JoinSetup);
}

void test_client_lobby_cancel()
{
    Fixture f;
    go_to_join_setup(f, "127.0.0.1");
    f.key(Key::Confirm);
    f.net.push(event(NetEvent::Type::WelcomeReceived));
    f.settle();
    CHECK(f.v().screen == Screen::ClientLobby);
    f.key(Key::Back); // leaving the lobby sends BYE
    CHECK_EQ(f.net.cancels, 1);
    CHECK(f.v().screen == Screen::JoinSetup);
}

void test_client_session_started()
{
    Fixture f;
    go_to_join_setup(f, "127.0.0.1");
    f.key(Key::Confirm);
    f.net.push(event(NetEvent::Type::WelcomeReceived));
    f.settle();
    NetEvent started = event(NetEvent::Type::SessionStarted);
    started.players = 3;
    f.net.push(started);
    f.settle();
    CHECK(f.model->result() == StartScreenResult::Net);
    CHECK(!f.model->host());
    CHECK_EQ(f.model->players(), 3);
    CHECK(f.v().screen == Screen::Done);
}

void test_host_session_failed()
{
    Fixture f;
    go_to_host_lobby(f);
    f.net.push(joined(1, "Ana"));
    f.settle();
    f.key(Key::Confirm);
    NetEvent failed = event(NetEvent::Type::SessionFailed);
    failed.text = "the socket closed";
    f.net.push(failed);
    f.settle();
    CHECK(f.v().screen == Screen::Error);
    CHECK(has(f.v().note, "the socket closed"));
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::Menu);
}

void test_session_end_messages()
{
    // 7.2, during play: the two messages the card names, and the numbering the players see (1-based).
    CHECK(StartScreenModel::net_end_player_left(3, "Ana") == "Player 3 (Ana) left the game.");
    CHECK(StartScreenModel::net_end_player_left(2, "") == "Player 2 left the game.");
    CHECK(StartScreenModel::net_end_desync(4242) == "The game went out of sync at frame 4242.");
}

void test_session_end_reason_from_the_layer()
{
    // Netplay's own error lines, as step()/report_hash() report them, and the name from the lobby's list.
    // The error line uses the wire index (the host is 0, the first client is 1); the box's number is 1-based
    // with the host as "Player 1", so wire 1 (Ana) is "Player 2", wire 2 (Bob) "Player 3" and wire 3 (Cid)
    // "Player 4".
    const std::vector<std::string> names = {"Host", "Ana", "Bob", "Cid"};
    CHECK(StartScreenModel::net_end_reason("player 1 left", names) == "Player 2 (Ana) left the game.");
    CHECK(StartScreenModel::net_end_reason("player 2 left", names) == "Player 3 (Bob) left the game.");
    CHECK(StartScreenModel::net_end_reason("player 3 stopped sending (31012 ms)", names) ==
          "Player 4 (Cid) left the game.");
    CHECK(StartScreenModel::net_end_reason("player 2 left") == "Player 3 left the game."); // no names: still numbered
    CHECK(StartScreenModel::net_end_reason("desync at frame 120") == "The game went out of sync at frame 120.");
    CHECK(StartScreenModel::net_end_reason("desync") == "The game went out of sync."); // no frame in the line
    CHECK(StartScreenModel::net_end_reason("host stopped sending") == "The host left the game.");
    CHECK(StartScreenModel::net_end_reason("the host left the lobby") == "The host left the game.");
    // Anything else is still named, with the layer's own words.
    CHECK(StartScreenModel::net_end_reason("timed out waiting for frame 400") ==
          "timed out waiting for frame 400.");
    // "stopped" is the player himself leaving (a signal, the window closing): no message box, exit code
    // untouched.
    CHECK(StartScreenModel::net_end_reason("stopped").empty());
    CHECK(StartScreenModel::net_end_reason("").empty());
}

void test_client_lobby_without_a_local_index()
{
    // The real layer cannot always say which entry is this PC (WELCOME's index is not exposed in the lobby):
    // it then sends every entry and WelcomeReceived.player = -1. The list is shown as it came, with no line
    // of its own and nobody marked "(you)".
    Fixture f;
    go_to_join_setup(f, "127.0.0.1");
    f.key(Key::Confirm);
    NetEvent welcome = event(NetEvent::Type::WelcomeReceived);
    welcome.name = "Ana";
    welcome.player = -1;
    f.net.push(welcome);
    f.net.push(joined(0, "Ana", 3));
    f.net.push(joined(1, "dave", 9));
    f.settle();
    CHECK(f.v().screen == Screen::ClientLobby);
    CHECK(has(f.v().note, "Connected to Ana. Waiting for the host to start."));
    CHECK_EQ(static_cast<int>(f.v().players.size()), 2);
    CHECK_EQ(f.v().players[0].player, 0);
    CHECK(!has(f.v().lines[0].text, "(you)"));
    CHECK(has(f.v().lines[0].text, "1 Ana"));
    CHECK(has(f.v().lines[1].text, "2 dave"));
}

void test_host_lobby_upnp_mapped_without_the_external_address()
{
    // The router opened the port but did not say the public address: the lobby says so and gives the local
    // address instead of leaving "Opening UDP port..." on the screen.
    Fixture f;
    go_to_host_lobby(f);
    NetEvent mapped = event(NetEvent::Type::UpnpMapped);
    mapped.ip.clear(); // no public address
    mapped.name = "192.168.50.20";
    f.net.push(mapped);
    f.settle();
    CHECK(has(f.v().footnote, "The router opened UDP 7877"));
    CHECK(has(f.v().footnote, "this PC is 192.168.50.20"));
    CHECK(!has(f.v().footnote, "Opening UDP port"));
}

// ---------------------------------------------------------------------------------------------------------
// The state machine as a whole, and the view's shape

void test_back_is_one_step_everywhere()
{
    Fixture f;
    menu_item(f, 1);
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::HostLobby); // Host -> the lobby at once
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::Menu);      // the lobby's Back returns to the menu

    menu_item(f, 2);
    f.key(Key::Confirm);
    CHECK(f.v().screen == Screen::JoinSetup);
    f.key(Key::Back);
    CHECK(f.v().screen == Screen::Menu);
}

void test_view_shape_every_screen()
{
    const Screen screens[] = {Screen::Menu,      Screen::Settings,    Screen::HostLobby, Screen::JoinSetup,
                              Screen::Connecting, Screen::ClientLobby, Screen::Error};
    for (Screen s : screens)
    {
        Fixture g;
        bool ok = true;
        switch (s)
        {
        case Screen::Menu:
            break;
        case Screen::Settings:
            go_to_settings(g);
            break;
        case Screen::HostLobby:
            go_to_host_lobby(g);
            break;
        case Screen::JoinSetup:
            go_to_join_setup(g, "");
            break;
        case Screen::Connecting:
            go_to_join_setup(g, "127.0.0.1");
            g.key(Key::Confirm);
            break;
        case Screen::ClientLobby:
            go_to_join_setup(g, "127.0.0.1");
            g.key(Key::Confirm);
            g.net.push(event(NetEvent::Type::WelcomeReceived));
            g.settle();
            break;
        case Screen::Error:
            go_to_join_setup(g, "");
            g.key(Key::Confirm);
            break;
        default:
            ok = false;
            break;
        }
        CHECK(ok);
        CHECK(g.v().screen == s);
        CHECK(!g.v().title.empty());               // every screen says what it is
        CHECK(!g.v().lines.empty());               // and has at least one line
        int selected = 0;
        for (const ViewLine &l : g.v().lines)
        {
            CHECK(!l.text.empty());
            if (l.selected)
                ++selected;
        }
        CHECK(selected <= 1);                      // at most one highlighted item
        if (s == Screen::Error)
            CHECK(!g.v().note.empty());            // the reason is always shown
    }
}

// ---------------------------------------------------------------------------------------------------------
// net.ini

std::filesystem::path temp_dir(const char *name)
{
    const auto base = std::filesystem::temp_directory_path() / "don-start-screen-test";
    const auto dir = base / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void test_net_ini_round_trip()
{
    const std::filesystem::path dir = temp_dir("round-trip");
    const std::filesystem::path file = dir / "net.ini";
    NetConfigFile in;
    in.name = "Ana";
    in.address = "203.0.113.5:7877";
    in.players = 3;
    in.local_players = 4;
    in.port = 9000;
    in.fullscreen = true;
    in.renderer = 1;
    in.vibration = false;
    in.net_delay = 7;
    std::string error;
    CHECK(don_net_config_save(file, in, error));
    CHECK(error.empty());
    CHECK(std::filesystem::exists(file));

    NetConfigFile out; // the defaults are 2 players, no name, no fullscreen, Vulkan, vibration on, auto delay
    CHECK(don_net_config_load(file, out, error));
    CHECK(error.empty());
    CHECK(out.name == "Ana");
    CHECK(out.address == "203.0.113.5:7877");
    CHECK_EQ(out.players, 3);
    CHECK_EQ(out.local_players, 4);
    CHECK_EQ(out.port, 9000);
    CHECK(out.fullscreen);
    CHECK_EQ(out.renderer, 1);
    CHECK(!out.vibration);
    CHECK_EQ(out.net_delay, 7);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_net_ini_settings_keys_default_when_missing()
{
    // An old file without the 7.3 keys still loads: the new values keep their defaults.
    const std::filesystem::path dir = temp_dir("old-file");
    const std::filesystem::path file = dir / "net.ini";
    std::string error;
    {
        std::ofstream old(file);
        old << "name=Ana\naddress=10.20.30.5:7877\nplayers=4\n";
    }
    NetConfigFile out;
    CHECK(don_net_config_load(file, out, error));
    CHECK(out.name == "Ana");
    CHECK_EQ(out.players, 4);
    CHECK_EQ(out.local_players, 1);
    CHECK(!out.fullscreen);
    CHECK_EQ(out.renderer, 0);
    CHECK(out.vibration);
    CHECK_EQ(out.net_delay, 0);

    // Bad values are ignored, the defaults stand.
    {
        std::ofstream bad(file);
        bad << "local_players=9\nfullscreen=2\nrenderer=3\nvibration=yes\nnet_delay=11\n";
        bad << "local_players=3\nfullscreen=1\nrenderer=1\nvibration=0\nnet_delay=10\n";
    }
    CHECK(don_net_config_load(file, out, error));
    CHECK_EQ(out.local_players, 3);
    CHECK(out.fullscreen);
    CHECK_EQ(out.renderer, 1);
    CHECK(!out.vibration);
    CHECK_EQ(out.net_delay, 10);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_net_ini_missing_file_and_bad_values()
{
    const std::filesystem::path dir = temp_dir("bad-values");
    const std::filesystem::path file = dir / "net.ini";
    std::string error;
    NetConfigFile out;
    out.players = 4;
    CHECK(don_net_config_load(file, out, error)); // a missing file is not an error
    CHECK(out.name.empty());
    CHECK(out.address.empty());
    CHECK_EQ(out.players, 4);

    {
        std::ofstream bad(file);
        bad << "# a comment\n";
        bad << "name=Ana\n";
        bad << "players=9\n";   // out of range: ignored
        bad << "players=3\n";   // the last good one wins
        bad << "nonsense\n";    // a line without '=' is ignored
        bad << "address = 10.20.30.5:7877 \n"; // spaces around the '=' are trimmed
    }
    CHECK(don_net_config_load(file, out, error));
    CHECK(out.name == "Ana");
    CHECK_EQ(out.players, 3);
    CHECK(out.address == "10.20.30.5:7877");

    // A folder where the file should be: that is an error, not a silent success.
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::filesystem::create_directories(file, ec);
    CHECK(!don_net_config_load(file, out, error));
    CHECK(!error.empty());
    std::filesystem::remove_all(dir, ec);
}

void test_net_ini_paths_and_user_name()
{
    const std::filesystem::path dir = don_net_config_dir();
    CHECK(!dir.empty());
    CHECK(don_net_config_path().filename() == "net.ini");
    CHECK(don_net_config_path().parent_path() == dir);
    CHECK(!don_os_user_name().empty()); // the tests always run as someone
}

} // namespace

int main()
{
    test_menu();
    test_menu_play_local();
    test_window_close_quits_from_every_screen();
    test_name_edit();
    test_name_edit_empty_is_the_os_user_name();
    test_name_edit_escape_discards();
    test_settings_values_and_back_saves();
    test_settings_net_delay_reaches_the_host();
    test_settings_host_port();
    test_host_begin_four_players();
    test_host_lobby_bound_and_upnp_mapped();
    test_host_lobby_upnp_failed_shows_the_forward_hint();
    test_host_lobby_windows_firewall_hint();
    test_host_lobby_players_start_and_auto_start();
    test_host_lobby_start_with_two_of_four();
    test_host_lobby_same_names_and_leaving();
    test_host_lobby_notice_for_a_refused_client();
    test_host_lobby_notice_fades_after_a_few_seconds();
    test_host_lobby_cancel_sends_bye();
    test_host_bind_failures();
    test_valid_address();
    test_join_setup_empty_and_malformed();
    test_join_setup_ok_and_connecting();
    test_join_setup_offers_the_last_address();
    test_connecting_timeout_after_10_seconds();
    test_connecting_cancel_sends_bye();
    test_connecting_name_does_not_resolve();
    test_client_rejects();
    test_client_lobby_and_host_leaving();
    test_client_lobby_cancel();
    test_client_session_started();
    test_host_session_failed();
    test_session_end_messages();
    test_session_end_reason_from_the_layer();
    test_client_lobby_without_a_local_index();
    test_host_lobby_upnp_mapped_without_the_external_address();
    test_back_is_one_step_everywhere();
    test_view_shape_every_screen();
    test_net_ini_round_trip();
    test_net_ini_settings_keys_default_when_missing();
    test_net_ini_missing_file_and_bad_values();
    test_net_ini_paths_and_user_name();
    return check_result("test_net_start_screen");
}
