#include "protocol.h"

#include <cstring>
#include <type_traits>

namespace don_net
{
namespace
{

constexpr uint8_t kMagic[4] = {'D', 'O', 'N', 'N'};
constexpr std::size_t kBuildIdBytes = 48, kModuleIdBytes = 64, kNameBytes = 16, kTextBytes = 64;
// WELCOME (section 6.0): 14 body bytes before the host's name; LOBBY: player u8 + ping_ms u16 + name char[16].
constexpr std::size_t kWelcomeBytes = 14 + kNameBytes, kLobbyEntryBytes = 1 + 2 + kNameBytes;

struct Writer
{
    std::vector<uint8_t> out;
    void u8(uint32_t v) { out.push_back(static_cast<uint8_t>(v)); }
    void u16(uint32_t v) { u8(v & 0xFF); u8((v >> 8) & 0xFF); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
    void str(const std::string &s, std::size_t field)
    {
        std::size_t n = s.size() < field ? s.size() : field;
        out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        out.insert(out.end(), field - n, 0);
    }
    void pad(const NetPad &p)
    {
        const std::size_t at = out.size();
        out.resize(at + kNetPadBytes);
        write_pad(p, out.data() + at);
    }
};

struct Reader
{
    const uint8_t *p;
    std::size_t n, pos = 0;
    std::size_t left() const { return n - pos; }
    uint8_t u8() { return p[pos++]; }
    uint16_t u16() { uint16_t a = u8(); return static_cast<uint16_t>(a | (u8() << 8)); }
    uint32_t u32() { uint32_t a = u16(); return a | (static_cast<uint32_t>(u16()) << 16); }
    uint64_t u64() { uint64_t a = u32(); return a | (static_cast<uint64_t>(u32()) << 32); }
    std::string str(std::size_t field)
    {
        std::size_t len = 0;
        while (len < field && p[pos + len] != 0)
            ++len;
        std::string s(reinterpret_cast<const char *>(p + pos), len);
        pos += field;
        return s;
    }
    NetPad pad()
    {
        NetPad v = read_pad(p + pos);
        pos += kNetPadBytes;
        return v;
    }
};

MsgType type_of(const Body &b) { return static_cast<MsgType>(b.index() + 1); }

} // namespace

std::vector<uint8_t> encode(const Message &m)
{
    Writer w;
    for (uint8_t c : kMagic)
        w.u8(c);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(type_of(m.body)));
    w.u16(m.session);

    bool ok = true;
    std::visit(
        [&](const auto &b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, Hello>)
            {
                w.str(b.build_id, kBuildIdBytes);
                w.u64(b.data_hash);
                w.str(b.name, kNameBytes);
                w.str(b.module_id, kModuleIdBytes);
            }
            else if constexpr (std::is_same_v<T, Welcome>)
            {
                // delay 0: provisional, the host decides it at START (section 6.0).
                ok = b.player >= 1 && b.player <= 3 && b.players >= 2 && b.players <= 4 && b.layout <= 1 &&
                     b.delay <= 10;
                w.u8(b.player); w.u8(b.players); w.u8(b.layout); w.u8(b.delay);
                w.u16(b.hash_every);
                w.u64(b.session_epoch);
                w.str(b.host_name, kNameBytes);
            }
            else if constexpr (std::is_same_v<T, Reject>)
            {
                w.u8(b.reason);
                w.str(b.text, kTextBytes);
            }
            else if constexpr (std::is_same_v<T, Start>)
            {
                ok = b.players >= 2 && b.players <= kMaxPlayers && b.layout <= 1 && b.delay >= 1 && b.delay <= 10 &&
                     b.your_player >= 1 && b.your_player < b.players;
                if (ok)
                {
                    w.u32(b.first_frame);
                    w.u8(b.players); w.u8(b.layout); w.u8(b.delay); w.u8(b.your_player);
                }
            }
            else if constexpr (std::is_same_v<T, Input>)
            {
                ok = !b.pads.empty() && b.pads.size() <= kMaxInputFrames;
                if (ok)
                {
                    w.u8(b.player);
                    w.u8(static_cast<uint32_t>(b.pads.size()));
                    w.u32(b.first_frame);
                    for (const NetPad &p : b.pads)
                        w.pad(p);
                }
            }
            else if constexpr (std::is_same_v<T, Frames>)
            {
                ok = b.players >= 2 && b.players <= kMaxPlayers && !b.pads.empty() &&
                     b.pads.size() % b.players == 0 && b.pads.size() / b.players <= kMaxFramesPerMessage;
                if (ok)
                {
                    w.u8(b.players);
                    w.u8(static_cast<uint32_t>(b.pads.size() / b.players));
                    w.u32(b.first_frame);
                    for (const NetPad &p : b.pads)
                        w.pad(p);
                }
            }
            else if constexpr (std::is_same_v<T, Hash>)
            {
                w.u32(b.frame);
                w.u64(b.hash);
            }
            else if constexpr (std::is_same_v<T, Desync>)
            {
                w.u32(b.frame);
                w.u8(b.player);
            }
            else if constexpr (std::is_same_v<T, Ping> || std::is_same_v<T, Pong>)
            {
                w.u32(b.id);
                w.u64(b.time_us);
            }
            else if constexpr (std::is_same_v<T, Bye>)
                w.u8(b.reason);
            else if constexpr (std::is_same_v<T, Waiting>)
            {
                w.u32(b.frame);
                w.u8(b.missing);
            }
            else if constexpr (std::is_same_v<T, Lobby>)
            {
                ok = !b.players.empty() && b.players.size() <= kMaxPlayers;
                if (ok)
                {
                    w.u8(static_cast<uint32_t>(b.players.size()));
                    for (const LobbyEntry &e : b.players)
                    {
                        w.u8(e.player);
                        w.u16(e.ping_ms);
                        w.str(e.name, kNameBytes);
                    }
                }
            }
        },
        m.body);
    if (!ok || w.out.size() > kMaxDatagram)
        return {};
    return std::move(w.out);
}

bool decode(const uint8_t *data, std::size_t size, Message &out)
{
    if (data == nullptr || size < kHeaderBytes || size > kMaxDatagram || std::memcmp(data, kMagic, 4) != 0 ||
        data[4] != kProtocolVersion)
        return false;
    const uint8_t type = data[5];
    if (type < 1 || type > 13)
        return false;
    Reader r{data, size};
    r.pos = 6;
    Message m;
    m.session = r.u16();
    const std::size_t body = size - kHeaderBytes;

    switch (static_cast<MsgType>(type))
    {
    case MsgType::Hello:
    {
        if (body != kBuildIdBytes + 8 + kNameBytes + kModuleIdBytes)
            return false;
        Hello h;
        h.build_id = r.str(kBuildIdBytes);
        h.data_hash = r.u64();
        h.name = r.str(kNameBytes);
        h.module_id = r.str(kModuleIdBytes);
        m.body = std::move(h);
        break;
    }
    case MsgType::Welcome:
    {
        if (body != kWelcomeBytes)
            return false;
        Welcome v;
        v.player = r.u8(); v.players = r.u8(); v.layout = r.u8(); v.delay = r.u8();
        v.hash_every = r.u16();
        v.session_epoch = r.u64();
        v.host_name = r.str(kNameBytes);
        if (v.player < 1 || v.player > 3 || v.players < 2 || v.players > 4 || v.layout > 1 || v.delay > 10)
            return false;
        m.body = std::move(v);
        break;
    }
    case MsgType::Reject:
    {
        if (body != 1 + kTextBytes)
            return false;
        Reject v;
        v.reason = r.u8();
        v.text = r.str(kTextBytes);
        m.body = std::move(v);
        break;
    }
    case MsgType::Start:
    {
        if (body != 8)
            return false;
        Start v;
        v.first_frame = r.u32();
        v.players = r.u8(); v.layout = r.u8(); v.delay = r.u8(); v.your_player = r.u8();
        if (v.players < 2 || v.players > kMaxPlayers || v.layout > 1 || v.delay < 1 || v.delay > 10 ||
            v.your_player < 1 || v.your_player >= v.players)
            return false;
        m.body = v;
        break;
    }
    case MsgType::Input:
    {
        if (body < 6)
            return false;
        Input v;
        v.player = r.u8();
        const std::size_t count = r.u8();
        v.first_frame = r.u32();
        if (count < 1 || count > kMaxInputFrames || body != 6 + count * kNetPadBytes)
            return false;
        for (std::size_t i = 0; i < count; ++i)
            v.pads.push_back(r.pad());
        m.body = std::move(v);
        break;
    }
    case MsgType::Frames:
    {
        if (body < 6)
            return false;
        Frames v;
        v.players = r.u8();
        const std::size_t count = r.u8();
        v.first_frame = r.u32();
        if (v.players < 2 || v.players > kMaxPlayers || count < 1 || count > kMaxFramesPerMessage ||
            body != 6 + count * v.players * kNetPadBytes)
            return false;
        for (std::size_t i = 0; i < count * v.players; ++i)
            v.pads.push_back(r.pad());
        m.body = std::move(v);
        break;
    }
    case MsgType::Hash:
    {
        if (body != 12)
            return false;
        Hash v;
        v.frame = r.u32();
        v.hash = r.u64();
        m.body = v;
        break;
    }
    case MsgType::Desync:
    {
        if (body != 5)
            return false;
        Desync v;
        v.frame = r.u32();
        v.player = r.u8();
        m.body = v;
        break;
    }
    case MsgType::Ping:
    {
        if (body != 12)
            return false;
        Ping v;
        v.id = r.u32();
        v.time_us = r.u64();
        m.body = v;
        break;
    }
    case MsgType::Pong:
    {
        if (body != 12)
            return false;
        Pong v;
        v.id = r.u32();
        v.time_us = r.u64();
        m.body = v;
        break;
    }
    case MsgType::Bye:
    {
        if (body != 1)
            return false;
        Bye v;
        v.reason = r.u8();
        m.body = v;
        break;
    }
    case MsgType::Waiting:
    {
        if (body != 5)
            return false;
        Waiting v;
        v.frame = r.u32();
        v.missing = r.u8();
        m.body = v;
        break;
    }
    case MsgType::Lobby:
    {
        if (body < 1)
            return false;
        Lobby v;
        const std::size_t count = r.u8();
        if (count < 1 || count > kMaxPlayers || body != 1 + count * kLobbyEntryBytes)
            return false;
        for (std::size_t i = 0; i < count; ++i)
        {
            LobbyEntry e;
            e.player = r.u8();
            e.ping_ms = r.u16();
            e.name = r.str(kNameBytes);
            v.players.push_back(std::move(e));
        }
        m.body = std::move(v);
        break;
    }
    }
    out = std::move(m);
    return true;
}

} // namespace don_net
