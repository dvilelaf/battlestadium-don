// Netplay messages and their wire format (D14, docs/knowledge/22-netplay.md sections 3 and 6.0). No I/O here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "net_pad.h"
#include "transport.h"

namespace don_net
{

// Version 3: HELLO carries the module id. Version 2: the lobby of section 6.0 (LOBBY, WELCOME with the host's name and provisional counts, START
// unicast with the final count, the layout, the measured delay and the client's compacted index).
constexpr uint8_t kProtocolVersion = 3;

enum class MsgType : uint8_t { Hello = 1, Welcome, Reject, Start, Input, Frames, Hash, Desync, Ping, Pong, Bye, Waiting, Lobby };
// module_id: the loaded game module's id (manifest.json), empty for a build linked with the game code.
struct Hello   { std::string build_id; uint64_t data_hash = 0; std::string name; std::string module_id; };
// Sent at join: players/layout/delay are provisional (the host's maximum, 0, 0); the final ones come in START.
struct Welcome { uint8_t player = 0, players = 0, layout = 0, delay = 0; uint16_t hash_every = 0; uint64_t session_epoch = 0;
                 std::string host_name; };
struct Reject  { uint8_t reason = 0; std::string text; };
// Unicast to one client: the final count, the layout, the measured delay and that client's compacted index.
struct Start   { uint32_t first_frame = 1; uint8_t players = 0, layout = 0, delay = 0, your_player = 0; };
struct Input   { uint8_t player = 0; uint32_t first_frame = 0; std::vector<NetPad> pads; };          // pads.size() = count
struct Frames  { uint8_t players = 0; uint32_t first_frame = 0; std::vector<NetPad> pads; };          // count * players, frame-major
struct Hash    { uint32_t frame = 0; uint64_t hash = 0; };
struct Desync  { uint32_t frame = 0; uint8_t player = 0; };
struct Ping    { uint32_t id = 0; uint64_t time_us = 0; };
struct Pong    { uint32_t id = 0; uint64_t time_us = 0; };
struct Bye     { uint8_t reason = 0; };
struct Waiting { uint32_t frame = 0; uint8_t missing = 0; };
// The lobby list: the host is player 0 with ping 0, then the clients in index order.
struct LobbyEntry { uint8_t player = 0; uint16_t ping_ms = 0; std::string name; };
struct Lobby   { std::vector<LobbyEntry> players; };                                                  // players.size() = count
using Body = std::variant<Hello, Welcome, Reject, Start, Input, Frames, Hash, Desync, Ping, Pong, Bye, Waiting, Lobby>;
struct Message { uint16_t session = 0; Body body; };

// Reject reasons.
constexpr uint8_t kRejectBuild = 1, kRejectData = 2, kRejectFull = 3, kRejectStarted = 4, kRejectModule = 5;
// Bye reasons.
constexpr uint8_t kByeQuit = 0, kByeTimeout = 1, kByeDesync = 2, kByeError = 3;

constexpr std::size_t kHeaderBytes = 8;
constexpr std::size_t kMaxInputFrames = 16;
constexpr std::size_t kMaxFramesPerMessage = 12;

std::vector<uint8_t> encode(const Message &m);                        // empty if a field is out of range
bool decode(const uint8_t *data, std::size_t size, Message &out);

} // namespace don_net
