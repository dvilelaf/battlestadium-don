// protocol.h: round trips, byte-exact layouts and rejection of malformed datagrams (D14, knowledge 22 sections 3
// and 6.0: protocol version 2, the LOBBY message, WELCOME with the host's name, START with the final count).
#include <cstring>

#include "check.h"
#include "protocol.h"

using namespace don_net;

static NetPad make_pad(int seed)
{
    NetPad p;
    p.buttons = static_cast<uint16_t>(0x1000 + seed);
    p.lx = static_cast<uint8_t>(seed);
    p.ly = static_cast<uint8_t>(seed + 1);
    p.rx = static_cast<uint8_t>(seed + 2);
    p.ry = static_cast<uint8_t>(seed + 3);
    p.connected = 1;
    p.has_pressure = 1;
    for (int i = 0; i < PS2_HOST_PAD_PRESSURE_COUNT; ++i)
        p.pressure[i] = static_cast<uint8_t>(seed * 3 + i);
    return p;
}

static bool roundtrip(const Message &m, Message &back)
{
    const auto bytes = encode(m);
    return !bytes.empty() && decode(bytes.data(), bytes.size(), back) && back.session == m.session &&
           back.body.index() == m.body.index();
}

static void test_roundtrips()
{
    Message back;
    {
        Hello h{"build-abc", 0x1122334455667788ull, "player one", "0123456789abcdef"};
        CHECK(roundtrip({0, h}, back));
        const Hello &g = std::get<Hello>(back.body);
        CHECK(g.build_id == h.build_id && g.data_hash == h.data_hash && g.name == h.name);
        CHECK(g.module_id == h.module_id);
        // A build linked with the game code sends an empty module id.
        CHECK(roundtrip({0, Hello{"b", 1, "n", ""}}, back));
        CHECK(std::get<Hello>(back.body).module_id.empty());
    }
    {
        Welcome w{2, 4, 1, 5, 120, 0x0102030405060708ull, "the host"};
        CHECK(roundtrip({0xBEEF, w}, back));
        const Welcome &g = std::get<Welcome>(back.body);
        CHECK(g.player == 2 && g.players == 4 && g.layout == 1 && g.delay == 5 && g.hash_every == 120 &&
              g.session_epoch == w.session_epoch && g.host_name == "the host");
        CHECK_EQ(back.session, 0xBEEF);
    }
    {
        // The provisional WELCOME of section 6.0: layout 0, the host's maximum, no delay yet.
        Welcome w{1, 4, 0, 0, 120, 7, ""};
        CHECK(roundtrip({9, w}, back));
        const Welcome &g = std::get<Welcome>(back.body);
        CHECK(g.player == 1 && g.players == 4 && g.layout == 0 && g.delay == 0 && g.host_name.empty());
    }
    {
        CHECK(roundtrip({7, Reject{3, "session is full"}}, back));
        CHECK(std::get<Reject>(back.body).reason == 3 && std::get<Reject>(back.body).text == "session is full");
    }
    CHECK(roundtrip({7, Start{1, 2, 0, 3, 1}}, back));
    CHECK(std::get<Start>(back.body).first_frame == 1 && std::get<Start>(back.body).players == 2 &&
          std::get<Start>(back.body).layout == 0 && std::get<Start>(back.body).delay == 3 &&
          std::get<Start>(back.body).your_player == 1);
    {
        Input in;
        in.player = 2;
        in.first_frame = 1000;
        for (int i = 0; i < 16; ++i)
            in.pads.push_back(make_pad(i));
        CHECK(roundtrip({9, in}, back));
        const Input &g = std::get<Input>(back.body);
        CHECK(g.player == 2 && g.first_frame == 1000 && g.pads == in.pads);
    }
    {
        Frames f;
        f.players = 4;
        f.first_frame = 70000;
        for (int i = 0; i < 12 * 4; ++i)
            f.pads.push_back(make_pad(i));
        CHECK(roundtrip({9, f}, back));
        const Frames &g = std::get<Frames>(back.body);
        CHECK(g.players == 4 && g.first_frame == 70000 && g.pads == f.pads);
    }
    CHECK(roundtrip({9, Hash{240, 0xFEDCBA9876543210ull}}, back));
    CHECK(std::get<Hash>(back.body).frame == 240 && std::get<Hash>(back.body).hash == 0xFEDCBA9876543210ull);
    CHECK(roundtrip({9, Desync{360, 3}}, back));
    CHECK(std::get<Desync>(back.body).frame == 360 && std::get<Desync>(back.body).player == 3);
    CHECK(roundtrip({9, Ping{5, 123456789012ull}}, back));
    CHECK(std::get<Ping>(back.body).id == 5 && std::get<Ping>(back.body).time_us == 123456789012ull);
    CHECK(roundtrip({9, Pong{6, 99ull}}, back));
    CHECK(std::get<Pong>(back.body).id == 6 && std::get<Pong>(back.body).time_us == 99);
    CHECK(roundtrip({9, Bye{2}}, back) && std::get<Bye>(back.body).reason == 2);
    CHECK(roundtrip({9, Waiting{44, 0x06}}, back));
    CHECK(std::get<Waiting>(back.body).frame == 44 && std::get<Waiting>(back.body).missing == 6);
    {
        Lobby l;
        l.players.push_back(LobbyEntry{0, 0, "host"});
        l.players.push_back(LobbyEntry{1, 42, "bob"});
        l.players.push_back(LobbyEntry{2, 65535, ""});
        CHECK(roundtrip({9, l}, back));
        const Lobby &g = std::get<Lobby>(back.body);
        CHECK(g.players.size() == 3 && g.players[0].player == 0 && g.players[0].ping_ms == 0 &&
              g.players[0].name == "host" && g.players[1].player == 1 && g.players[1].ping_ms == 42 &&
              g.players[1].name == "bob" && g.players[2].player == 2 && g.players[2].ping_ms == 65535);
    }
}

static void test_byte_exact()
{
    // Header (version 3) + START with the final count, layout, delay and compacted index: 8 + 8.
    auto b = encode({0x1234, Start{0x01020304, 3, 1, 5, 2}});
    const uint8_t start[] = {'D', 'O', 'N', 'N', 3, 4, 0x34, 0x12, 0x04, 0x03, 0x02, 0x01, 3, 1, 5, 2};
    CHECK_EQ(b.size(), sizeof(start));
    CHECK(b.size() == sizeof(start) && std::memcmp(b.data(), start, sizeof(start)) == 0);

    // INPUT: 1 frame.
    Input in;
    in.player = 2;
    in.first_frame = 0x0A0B0C0D;
    in.pads.push_back(make_pad(5));
    b = encode({1, in});
    CHECK_EQ(b.size(), 8 + 6 + 20);
    CHECK_EQ(b[5], 5);
    CHECK_EQ(b[6], 1);
    CHECK_EQ(b[7], 0);
    CHECK_EQ(b[8], 2);  // player
    CHECK_EQ(b[9], 1);  // count
    CHECK_EQ(b[10], 0x0D);
    CHECK_EQ(b[13], 0x0A);
    uint8_t pad[kNetPadBytes];
    write_pad(make_pad(5), pad);
    CHECK(std::memcmp(b.data() + 14, pad, kNetPadBytes) == 0);

    // FRAMES: 2 frames x 3 players, frame-major.
    Frames f;
    f.players = 3;
    f.first_frame = 9;
    for (int i = 0; i < 6; ++i)
        f.pads.push_back(make_pad(10 + i));
    b = encode({1, f});
    CHECK_EQ(b.size(), 8 + 6 + 6 * 20);
    CHECK_EQ(b[5], 6);
    CHECK_EQ(b[8], 3);
    CHECK_EQ(b[9], 2);
    CHECK_EQ(b[10], 9);
    for (int i = 0; i < 6; ++i)
    {
        write_pad(make_pad(10 + i), pad);
        CHECK(std::memcmp(b.data() + 14 + i * 20, pad, kNetPadBytes) == 0);
    }

    // HELLO layout: 8 + 48 + 8 + 16 + 64 (module id), zero-padded strings.
    b = encode({0, Hello{"ab", 1, "n"}});
    CHECK_EQ(b.size(), 144);
    CHECK_EQ(b[8], 'a');
    CHECK_EQ(b[10], 0);
    CHECK_EQ(b[8 + 48], 1);
    CHECK_EQ(b[8 + 56], 'n');

    // WELCOME: 8 + 14 + 16 (the host's name at the end).
    b = encode({0, Welcome{1, 2, 0, 3, 0x0102, 0, "host"}});
    CHECK_EQ(b.size(), 38);
    CHECK_EQ(b[12], 0x02);
    CHECK_EQ(b[13], 0x01);
    CHECK_EQ(b[22], 'h');
    CHECK_EQ(b[23], 'o');
    CHECK_EQ(b[26], 0);

    // LOBBY: 8 + 1 + count * 19; the host is player 0.
    Lobby l;
    l.players.push_back(LobbyEntry{0, 0, "host"});
    l.players.push_back(LobbyEntry{1, 42, "bob"});
    b = encode({1, l});
    CHECK_EQ(b.size(), 8 + 1 + 2 * 19);
    CHECK_EQ(b[5], 13);
    CHECK_EQ(b[8], 2);  // count
    CHECK_EQ(b[9], 0);  // player
    CHECK_EQ(b[10], 0); // ping_ms
    CHECK_EQ(b[11], 0);
    CHECK_EQ(b[12], 'h');
    CHECK_EQ(b[28], 1);  // second entry: player
    CHECK_EQ(b[29], 42); // ping_ms
    CHECK_EQ(b[31], 'b');
}

static void test_rejects()
{
    Message m;
    Frames f;
    f.players = 2;
    f.first_frame = 1;
    f.pads.assign(4, make_pad(1));
    Input in;
    in.player = 1;
    in.first_frame = 1;
    in.pads.assign(2, make_pad(2));
    Lobby l;
    l.players = {LobbyEntry{0, 0, "host"}, LobbyEntry{1, 3, "n"}};
    const Message samples[] = {{1, Hello{"b", 2, "n"}}, {1, Welcome{1, 2, 0, 3, 120, 5, "h"}},
                               {1, Reject{1, "x"}},       {1, Start{1, 2, 0, 3, 1}},
                               {1, in},                   {1, f},
                               {1, Hash{1, 2}},           {1, Desync{1, 1}},
                               {1, Ping{1, 2}},           {1, Pong{1, 2}},
                               {1, Bye{1}},               {1, Waiting{1, 1}},
                               {1, l}};
    for (const Message &s : samples)
    {
        const auto good = encode(s);
        CHECK(!good.empty() && decode(good.data(), good.size(), m));
        // every truncation, and one byte too many
        for (std::size_t n = 0; n < good.size(); ++n)
            CHECK(!decode(good.data(), n, m));
        auto longer = good;
        longer.push_back(0);
        CHECK(!decode(longer.data(), longer.size(), m));
        // wrong magic, an old version (2), a future one (4) and unknown types
        auto bad = good;
        bad[0] = 'X';
        CHECK(!decode(bad.data(), bad.size(), m));
        bad = good;
        bad[4] = 2;
        CHECK(!decode(bad.data(), bad.size(), m));
        bad = good;
        bad[4] = 4;
        CHECK(!decode(bad.data(), bad.size(), m));
        bad = good;
        bad[5] = 0;
        CHECK(!decode(bad.data(), bad.size(), m));
        bad = good;
        bad[5] = 14;
        CHECK(!decode(bad.data(), bad.size(), m));
    }
    CHECK(!decode(nullptr, 0, m));

    // Counts 0 and 17 (INPUT), 0 and 13 (FRAMES), 0 and 5 (LOBBY): crafted from a valid datagram; the size matches.
    auto in_bytes = encode({1, in});
    in_bytes[9] = 0;
    in_bytes.resize(14);
    CHECK(!decode(in_bytes.data(), in_bytes.size(), m));
    in_bytes[9] = 17;
    in_bytes.resize(14 + 17 * 20);
    CHECK(!decode(in_bytes.data(), in_bytes.size(), m));
    auto f_bytes = encode({1, f});
    f_bytes[9] = 0;
    f_bytes.resize(14);
    CHECK(!decode(f_bytes.data(), f_bytes.size(), m));
    f_bytes[9] = 13;
    f_bytes.resize(14 + 13 * 2 * 20);
    CHECK(!decode(f_bytes.data(), f_bytes.size(), m));
    // players out of range in FRAMES
    f_bytes = encode({1, f});
    f_bytes[8] = 5;
    CHECK(!decode(f_bytes.data(), f_bytes.size(), m));
    auto l_bytes = encode({1, l});
    l_bytes[8] = 0;
    l_bytes.resize(9);
    CHECK(!decode(l_bytes.data(), l_bytes.size(), m));
    l_bytes[8] = 5;
    l_bytes.resize(9 + 5 * 19);
    CHECK(!decode(l_bytes.data(), l_bytes.size(), m));
}

static void test_encode_limits()
{
    Input in;
    CHECK(encode({0, in}).empty()); // 0 pads
    in.pads.assign(17, make_pad(0));
    CHECK(encode({0, in}).empty());
    in.pads.assign(16, make_pad(0));
    CHECK(!encode({0, in}).empty());

    Frames f;
    f.players = 1;
    f.pads.assign(2, make_pad(0));
    CHECK(encode({0, f}).empty()); // players out of range
    f.players = 5;
    f.pads.assign(10, make_pad(0));
    CHECK(encode({0, f}).empty());
    f.players = 3;
    f.pads.assign(7, make_pad(0));
    CHECK(encode({0, f}).empty()); // not a multiple
    f.pads.assign(13 * 3, make_pad(0));
    CHECK(encode({0, f}).empty()); // 13 frames
    f.pads.clear();
    CHECK(encode({0, f}).empty());
    f.pads.assign(12 * 3, make_pad(0));
    CHECK(!encode({0, f}).empty());

    CHECK(encode({0, Welcome{0, 2, 0, 3, 1, 1, ""}}).empty());
    CHECK(encode({0, Welcome{1, 5, 0, 3, 1, 1, ""}}).empty());
    CHECK(encode({0, Welcome{1, 2, 0, 11, 1, 1, ""}}).empty());
    CHECK(!encode({0, Welcome{1, 2, 0, 0, 1, 1, ""}}).empty()); // delay 0: provisional (section 6.0)
    CHECK(encode({0, Welcome{1, 2, 2, 0, 1, 1, ""}}).empty());  // layout out of range

    // START: the count, the layout, the delay and the compacted index are checked.
    CHECK(!encode({0, Start{1, 2, 0, 3, 1}}).empty());
    CHECK(encode({0, Start{1, 1, 0, 3, 1}}).empty());  // players < 2
    CHECK(encode({0, Start{1, 5, 0, 3, 1}}).empty());  // players > 4
    CHECK(encode({0, Start{1, 2, 2, 3, 1}}).empty());  // layout > 1
    CHECK(encode({0, Start{1, 2, 0, 0, 1}}).empty());  // delay < 1
    CHECK(encode({0, Start{1, 2, 0, 11, 1}}).empty()); // delay > 10
    CHECK(encode({0, Start{1, 2, 0, 3, 0}}).empty());  // the host has no index in START
    CHECK(encode({0, Start{1, 2, 0, 3, 2}}).empty());  // index >= players
    CHECK(!encode({0, Start{1, 4, 1, 2, 3}}).empty());

    // LOBBY: 1..4 entries.
    Lobby l;
    CHECK(encode({0, l}).empty()); // 0 entries
    l.players.assign(5, LobbyEntry{1, 0, "x"});
    CHECK(encode({0, l}).empty());
    l.players.assign(4, LobbyEntry{1, 0, "x"});
    CHECK(!encode({0, l}).empty());
}

static void test_strings()
{
    const std::string longid(200, 'x');
    auto b = encode({0, Hello{longid, 1, std::string(100, 'n')}});
    CHECK_EQ(b.size(), 144);
    Message m;
    CHECK(decode(b.data(), b.size(), m));
    CHECK_EQ(std::get<Hello>(m.body).build_id.size(), 48);
    CHECK_EQ(std::get<Hello>(m.body).name.size(), 16);
    // The datagram after the build id is intact (hash field not overwritten).
    CHECK_EQ(std::get<Hello>(m.body).data_hash, 1);
    b = encode({0, Reject{1, std::string(300, 'r')}});
    CHECK_EQ(b.size(), 8 + 65);
    CHECK(decode(b.data(), b.size(), m));
    CHECK_EQ(std::get<Reject>(m.body).text.size(), 64);
    // A string field that is full (no zero) decodes at its full length; embedded zero stops it.
    b = encode({0, Hello{"abc", 0, ""}});
    b[8 + 5] = 'z'; // after the zero: ignored
    CHECK(decode(b.data(), b.size(), m));
    CHECK(std::get<Hello>(m.body).build_id == "abc");
    // The host's name and the lobby names are cut to 16 bytes too.
    b = encode({0, Welcome{1, 2, 0, 0, 120, 0, std::string(100, 'h')}});
    CHECK(decode(b.data(), b.size(), m));
    CHECK_EQ(std::get<Welcome>(m.body).host_name.size(), 16);
    Lobby l;
    l.players = {LobbyEntry{0, 0, std::string(100, 'z')}};
    b = encode({0, l});
    CHECK(decode(b.data(), b.size(), m));
    CHECK_EQ(std::get<Lobby>(m.body).players[0].name.size(), 16);
}

int main()
{
    test_roundtrips();
    test_byte_exact();
    test_rejects();
    test_encode_limits();
    test_strings();
    return check_result("test_net_protocol");
}
