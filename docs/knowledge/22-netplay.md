# 22 · Netplay: the design contract (D14)

Online play for up to 4 players, stage 2. Decision: [../DECISIONS.md](../DECISIONS.md) D14. Task cards:
[../tasks/M5-online.md](../tasks/M5-online.md). This page is the contract every task implements: names, files,
message layouts and behaviour are fixed here so that the tasks can run in parallel. A task that needs to change the
contract stops and says so (AGENTS.md, rule 3); only the orchestrator edits this page.

Facts it relies on (measured, with references): [13-runtime-host.md](13-runtime-host.md) (the host loop),
journal 017 (determinism after patch 0038), [17-windows-toolchain.md](17-windows-toolchain.md) (Linux and Wine give
identical state lines), and the code survey of 2026-10-15 summarised in section 9.

## 1. Model

- Every player runs the same build of the port with the same game data. Only pad input crosses the network.
- Lockstep with input delay D (default 3 frames): the pad a player holds at frame f is applied at frame f+D on every
  copy. A copy enters frame f only when it has the **confirmed set** for f: one `NetPad` per player.
- Star: the host is the hub. Clients send `INPUT` to the host; the host builds each frame's set (its own pad plus the
  clients') and sends `FRAMES` to every client. The host uses its own sets the same way, so all copies apply the
  same sets.
- Frames 1..D: every player's pad is `neutral_pad()` (connected, nothing pressed).
- A frame is the host's vertical blank counter `frame` of `gs.set_on_vblank` (1-based, equal to the scheduler's vsync
  tick: section 9).
- Every `hash_every` frames (120) each copy computes the guest RAM hash at the same point (the vblank hook, before the
  pads are published) and clients send it to the host; a mismatch stops the session for everyone (`DESYNC`).
- No joining or reconnecting once started; a client silent for 30 s ends the session.

## 2. Files

| File | What | Task |
|---|---|---|
| `src/host/net/net_pad.h` | `NetPad`, wire layout, conversions | done (orchestrator) |
| `src/host/net/transport.h` | `Transport`, `PeerAddr`, `open_udp`, `resolve`, `with_simulated_network`, `LoopbackHub` | done (contract) |
| `src/host/net/protocol.h/.cpp` | message structs, `encode`, `decode` | T-501 |
| `src/host/net/lockstep.h/.cpp` | frame sets, delay, redundancy bookkeeping (no I/O) | T-502 |
| `src/host/net/udp_socket_posix.cpp`, `udp_socket_win32.cpp` | `open_udp`, `resolve` | T-503 |
| `src/host/net/sim_transport.cpp` | `with_simulated_network`, `LoopbackHub` | T-503 |
| `src/host/net/game_hash.h/.cpp` | hash of the game files | T-505 |
| `src/host/build_id.h/.cpp` | `don_build_id()` | T-505 |
| `src/host/net/netplay.h/.cpp` | `Netplay`: handshake, per-frame step, hashes | T-508 |
| `src/host/options.*`, `src/host/host.cpp` | options, the net mode in the host | T-504, T-509 |
| runtime patch `0074-session-clock` | the guest's clock from the session | T-506 |
| `tools/launcher/launch.py` | `--host` / `--join` | T-507 |
| `tests/net/test_net_*.cpp` | unit tests, one program each (`tests/net/CMakeLists.txt` globs them) | each task |

`src/host/net/*.cpp` must not include SDL or runtime headers other than `runtime/ps2_host_pad.h`: the unit tests build
them alone. Operating-system calls only in `udp_socket_posix.cpp` / `udp_socket_win32.cpp` (AGENTS.md, portability).

Build and run the unit tests (host tools, no container):

```bash
cmake -S tests/net -B build/net-tests -G Ninja && cmake --build build/net-tests -j2
DON_NET_TESTS_DIR=build/net-tests uv run --no-project --with pytest python -I -m pytest tests/net -q
```

## 3. Wire format (protocol version 1)

All integers little-endian. Every datagram starts with an 8-byte header:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `'D','O','N','N'` |
| 4 | 1 | protocol version = 1 |
| 5 | 1 | type (below) |
| 6 | 2 | session id (u16; 0 in `HELLO`; the host's random id afterwards; datagrams with another id are ignored) |

A datagram is at most `kMaxDatagram` (1200) bytes. `decode` rejects (returns false) a wrong magic, version, a size
that does not match the type's layout, or a count out of range; it never reads past `size`.

| Type | Name | Direction | Body after the header |
|---|---|---|---|
| 1 | `HELLO` | client → host | `build_id` char[48] (zero-padded ASCII), `data_hash` u64, `name` char[16] |
| 2 | `WELCOME` | host → client | `player` u8 (1..3), `players` u8 (2..4), `layout` u8 (0 two ports, 1 multitap port 0), `delay` u8 (1..10), `hash_every` u16, `session_epoch` u64 (Unix seconds) |
| 3 | `REJECT` | host → client | `reason` u8 (1 build, 2 data, 3 full, 4 started), `text` char[64] |
| 4 | `START` | host → all | `first_frame` u32 (= 1) |
| 5 | `INPUT` | client → host | `player` u8, `count` u8 (1..16), `first_frame` u32, then `count` × `NetPad` (20 bytes) for frames `first_frame` .. `first_frame+count-1` |
| 6 | `FRAMES` | host → client | `players` u8 (2..4), `count` u8 (1..12), `first_frame` u32, then `count` × `players` × `NetPad`, frame-major |
| 7 | `HASH` | client → host | `frame` u32, `hash` u64 |
| 8 | `DESYNC` | host → all | `frame` u32, `player` u8 (the first that differs) |
| 9 | `PING` | any | `id` u32, `time_us` u64 (the sender's clock) |
| 10 | `PONG` | any | `id` u32, `time_us` u64 (echoed) |
| 11 | `BYE` | any | `reason` u8 (0 quit, 1 timeout, 2 desync, 3 error) |
| 12 | `WAITING` | host → client | `frame` u32, `missing` u8 (bit p set: player p's input for `frame` is missing) |

The host is always player 0. `players` is the total in the session.

`protocol.h` (T-501) declares, in namespace `don_net`:

```cpp
enum class MsgType : uint8_t { Hello = 1, Welcome, Reject, Start, Input, Frames, Hash, Desync, Ping, Pong, Bye, Waiting };
struct Hello   { std::string build_id; uint64_t data_hash = 0; std::string name; };
struct Welcome { uint8_t player = 0, players = 0, layout = 0, delay = 0; uint16_t hash_every = 0; uint64_t session_epoch = 0; };
struct Reject  { uint8_t reason = 0; std::string text; };
struct Start   { uint32_t first_frame = 1; };
struct Input   { uint8_t player = 0; uint32_t first_frame = 0; std::vector<NetPad> pads; };          // pads.size() = count
struct Frames  { uint8_t players = 0; uint32_t first_frame = 0; std::vector<NetPad> pads; };          // count * players, frame-major
struct Hash    { uint32_t frame = 0; uint64_t hash = 0; };
struct Desync  { uint32_t frame = 0; uint8_t player = 0; };
struct Ping    { uint32_t id = 0; uint64_t time_us = 0; };
struct Pong    { uint32_t id = 0; uint64_t time_us = 0; };
struct Bye     { uint8_t reason = 0; };
struct Waiting { uint32_t frame = 0; uint8_t missing = 0; };
using Body = std::variant<Hello, Welcome, Reject, Start, Input, Frames, Hash, Desync, Ping, Pong, Bye, Waiting>;
struct Message { uint16_t session = 0; Body body; };

std::vector<uint8_t> encode(const Message &m);                        // empty if a field is out of range
bool decode(const uint8_t *data, std::size_t size, Message &out);
```

Strings longer than their field are cut; shorter ones are zero-padded; decoding stops a string at the first zero.

## 4. Lockstep engine (T-502)

Pure bookkeeping, no time, no I/O. In `lockstep.h`:

```cpp
class Lockstep {
public:
    Lockstep(int players, int local_player, int delay);              // players 2..4, delay 1..10
    // The local pad sampled at frame f; stored for frame f + delay. Returns that target frame.
    uint32_t submit_local(uint32_t f, const NetPad &pad);
    // Host: a player's pad for a frame (from INPUT or from submit_local). A second value for the same frame is ignored.
    void record(int player, uint32_t frame, const NetPad &pad);
    // Host: the frame's set is complete (every player recorded, or frame <= delay).
    bool complete(uint32_t frame) const;
    // Host: confirm a complete frame (copies the recorded pads into the confirmed set). Client: from FRAMES.
    void confirm(uint32_t frame, const NetPad *pads);                   // pads[players]
    bool confirmed(uint32_t frame) const;                                 // frames <= delay are always confirmed (neutral)
    NetPad confirmed_pad(uint32_t frame, int player) const;
    // Players whose pad for frame is still missing (bit p), on the host.
    uint8_t missing(uint32_t frame) const;
    // Client: the local pads to (re)send: frames (acked, last_submitted], at most max of them, oldest first.
    std::vector<std::pair<uint32_t, NetPad>> unacked_local(std::size_t max) const;
    void ack_local(uint32_t through_frame);                              // the host confirmed up to this frame
    // Host: confirmed frames (from, last_confirmed], at most max, for FRAMES (redundancy).
    std::vector<uint32_t> confirmed_since(uint32_t from, std::size_t max) const;
    uint32_t last_confirmed() const;
    void forget_before(uint32_t frame);                                  // drop old frames (memory bound)
};
```

Rules: frames are 1-based; `submit_local` is called with strictly increasing f; anything older than `forget_before`
is ignored; a frame <= delay has every player's `neutral_pad()`. Memory: callers forget frames older than 600.

## 5. Transport (T-503)

`transport.h` is the contract. `open_udp` binds IPv4 `INADDR_ANY:port`, non-blocking with `receive` waiting through
`poll`/`select` up to `timeout_ms`; `SO_REUSEADDR` off. Windows: `WSAStartup` once (a function-static guard), link
`ws2_32`. `resolve` uses `getaddrinfo` (AF_INET). `with_simulated_network` keeps a queue of datagrams with their
release times (`std::chrono::steady_clock`), releases due ones on every `send`/`receive` call, and uses
`std::mt19937_64(seed)` for loss and jitter. `LoopbackHub` endpoints share a mutex-protected map port → queue; a
`receive` with a timeout waits on a condition variable.

## 6. Netplay coordinator (T-508)

`netplay.h`:

```cpp
struct NetConfig {
    bool host = false;
    uint16_t port = 7877;            // host: listen port; client: 0 (any)
    std::string join;                // client: "host:port"
    int players = 2;                 // host: total players expected, 2..4
    int delay = 0;                   // host decides (0 = automatic from the measured ping, section 6.1); clients take the host's
    std::string name;                // shown in the host's log
    std::string build_id;            // don_build_id()
    uint64_t data_hash = 0;          // hash_game_files()
    int hash_every = 120;
    int warn_after_ms = 3000;        // print who is late
    int abort_after_ms = 30000;      // end the session
};
struct NetSession { int local_player = 0; int players = 0; int layout = 0; int delay = 0; uint64_t session_epoch = 0; };

class Netplay {
public:
    Netplay(NetConfig config, std::unique_ptr<Transport> transport);
    // Blocking. Host: waits for players-1 clients (HELLO checked against build_id and data_hash: REJECT otherwise),
    // answers WELCOME (players 1.., the layout: 0 if players == 2 else 1, delay, hash_every, session_epoch =
    // the host's time(nullptr)), then sends START to all. Client: sends HELLO every 500 ms until WELCOME or
    // REJECT, then waits for START. Both give up when should_stop() or after abort_after_ms without progress.
    bool connect(NetSession &out, std::string &error, const std::function<bool()> &should_stop);
    // Per frame f (1, 2, 3, ... on the game thread): submit the local pad, send and receive, and block until the
    // confirmed set for f exists; out[0..players-1] get it. False: the session ended (error says why).
    bool step(uint32_t f, const NetPad &local, NetPad *out, std::string &error, const std::function<bool()> &should_stop);
    // Every hash_every frames, after step(f). Never blocks (2026-10-15, journal 040): the host keeps its own hash per
    // checked frame and compares each client's when it arrives, sending DESYNC on a mismatch; a client resends its
    // pending hashes with every INPUT until the host's FRAMES are hash_every frames past them; a hash that never comes
    // is logged, not fatal. False: a desync was detected (here or from a DESYNC received).
    bool report_hash(uint32_t f, uint64_t hash, std::string &error);
    void close(uint8_t bye_reason);                                      // sends BYE to the peers
    std::string status() const;                                          // one line for the log
};
```

### 6.0 Protocol version 2: the lobby (2026-10-15, for the start screen, T-520)

The start screen (section 7.2) needs a lobby the blocking `connect` cannot show. Changes to section 3 (the protocol
version becomes 2; a version-1 peer is rejected by the header check):

| Type | Name | Change |
|---|---|---|
| 2 | `WELCOME` | sent at join; adds `host_name` char[16] at the end. Its `players`, `layout` and `delay` are provisional (the host's chosen maximum, 0, 0): the final ones come in `START` |
| 3 | `REJECT` | for reason 1 (build) `text` is `"host <host build id> / you <client build id>"` (cut to 64) |
| 4 | `START` | unicast to each client: `first_frame` u32, `players` u8 (the connected count, 2..4), `layout` u8, `delay` u8 (section 6.1, measured now), `your_player` u8 (1..players-1: indices are compacted at start, so a player who left the lobby leaves no gap) |
| 13 | `LOBBY` | host → each client, on every change and every second: `count` u8 (1..4), then `count` × (`player` u8, `ping_ms` u16, `name` char[16]); the host is player 0 with ping 0 |

`Netplay` gains a non-blocking handshake; `connect` stays as a blocking wrapper over it (the command line and the tests
use it unchanged):

```cpp
enum class HandshakeState { Connecting, Lobby, Started, Failed };
struct LobbyPeer { int player = 0; std::string name; int ping_ms = 0; };
bool start(std::string &error);                         // host: bind and listen; client: resolve and send the first HELLO
HandshakeState poll(NetSession &out, std::string &error); // non-blocking, call every frame of the start screen; out is
                                                        // filled when it returns Started; error when Failed
void request_start();                                   // host: compact the indices, measure the delay, send START
std::vector<LobbyPeer> lobby() const;                   // the current list (host and client alike)
int connected_players() const;                          // host: itself + clients in the lobby
std::string host_name() const;                          // client: from WELCOME
void cancel();                                          // BYE to every peer, back to nothing
```

Host lobby rules: a `HELLO` with a full lobby or after `request_start` gets `REJECT` (3 or 4); a client's `BYE` or 10 s
of silence in the lobby removes it (and `LOBBY` is resent); `request_start` with fewer than 2 connected does nothing.

### 6.1 Automatic input delay

With `delay == 0` the host measures each client during `connect`, after its `HELLO` and before `START`: 5 `PING`s
100 ms apart, the median round trip of each client. `delay = clamp(ceil(max_rtt_ms / 16.7) + 1, 1, 10)`, where
`max_rtt_ms` is the largest median (a client's input reaches the other clients through the host: about one round
trip), and the +1 absorbs jitter. Dolphin's netplay guide gives the same scale (about 1 frame per 15 ms of ping). The
chosen delay and every client's round trip are logged (`[don-net] delay 2 frames (round trips: p1 3 ms, p2 21 ms)`).
A later version may add Dolphin's "host input authority" mode (the host plays with no delay); not in this one.

Behaviour inside `step`: a client sends `INPUT` with all its unacked local pads (at most 16) on every call and every
16 ms while it waits; the host sends `FRAMES` with the last 12 confirmed frames to every client whenever it confirms a
frame and every 16 ms while it waits, and `WAITING` while a frame is incomplete. Both answer `PING` with `PONG` and
send a `PING` every second (the round trip is logged by `status()`). Log lines start with `[don-net]`.

## 7. Host integration (T-509)

Options (T-504, in `DonHostOptions`): `--net-host PORT` (1..65535), `--net-join HOST:PORT`, `--net-players N` (2..4,
host only, default 2), `--net-delay N` (1..10, host only; not given = automatic from the measured ping), `--net-name NAME` (at most 15 characters),
`--mute` (the sound processor runs as with sound on, no audio device is opened). Errors (exit 2): both `--net-host`
and `--net-join`; `--net-players`/`--net-delay` without `--net-host`; `--sound off` or `--players` with either net
option.

In net mode the host (`host.cpp`):

1. Before the game thread starts: `Netplay::connect`; on failure print the error and exit with code 3.
2. Forces sound on (the sound processor always emulated: it changes the guest state); `--mute` only skips the device.
3. Uses `NetSession::layout` for the pad layout (multitap when more than 2 players).
4. Uses a fresh empty memory card folder `<temp>/don-net-<pid>/` (removed at exit), whatever `--mc-dir` says.
5. Calls the runtime's session clock (patch 0074) with `session_epoch`.
6. In the vblank hook, instead of the per-player loop: the local pad = player 0's device pad merged with the input
   script's player 0 (so scripts drive the local player in tests) → `Netplay::step` → publish every player's
   confirmed pad at `player_to_port_slot(p, layout)`. The automatic video skip is off (players skip with Start).
7. Every `hash_every` frames: the RAM hash (the game override exposes `hashGuestRam` through a function pointer the
   host reads, like the video probe) → `report_hash`.
8. On a `false` from `step`/`report_hash`: print the error and request the stop.
9. Diagnostics: `DON_NET_SIM_LATENCY_MS`, `DON_NET_SIM_JITTER_MS`, `DON_NET_SIM_LOSS`, `DON_NET_SIM_SEED` wrap the
   UDP transport with `with_simulated_network`.

### 7.1 Reaching the host: IP or domain, and UPnP (T-515)

Joiners type the host's public IP address or domain (`--net-join HOST[:PORT]`, port 7877 by default) and nothing
else: they only send outbound UDP, which every home router lets through, and install or configure nothing. One session
per host. Only the host's UDP port must be reachable. `src/host/net/upnp.h`:

```cpp
struct UpnpResult { bool mapped = false; std::string external_ip; std::string local_ip; std::string message; };
UpnpResult upnp_open(uint16_t port);    // discover the router (2 s), map UDP port -> this PC's port, read the external IP
void upnp_close();                      // remove the mapping made by upnp_open (at exit, also after a desync or error)
```

miniupnpc (BSD-3-Clause), pinned and built static by `tools/setup/build_miniupnpc.sh` for Linux and Windows; only
`upnp.cpp` includes it. With `--net-host` the host calls `upnp_open` before `Netplay::connect` and logs either
`[don-net] players can join at <external_ip>:<port> (or your domain)` or `[don-net] the router did not open the port
(<why>): forward UDP <port> to <local_ip> once on the router`; it calls `upnp_close` at exit. A failed UPnP never stops
the host (on a LAN or with a forwarded port it is not needed). The mapping's description is `Battle Stadium DON`, its
lease 0 (until removed); a mapping of the same port to another PC is left alone and reported.

### 7.2 The start screen (T-517): how players reach all this

Players never type options: the AppImage and the Windows package start on a small screen of the port itself (an SDL
window with `SDL_RenderDebugText`, 640x448, before the game window opens), driven by keyboard or gamepad. Options given
on the command line (`--net-host`, `--net-join`, `--headless`, an input script) skip it, for tests. Texts in English.

States and transitions (Esc or the gamepad's back button always goes one step back; closing the window quits):

| State | Shows | Actions |
|---|---|---|
| Menu | Play on this PC / Host an online game / Join an online game / Name: <name> / Quit | Play: start the game as today. Name: edit (1-15 printable ASCII; empty = the OS user name) |
| Host setup | Players: 2 / 3 / 4 (last value) | Confirm: bind the port, run UPnP, go to Lobby (host) |
| Lobby (host) | "Players can join at <external_ip or domain>:<port>" or the UPnP failure line with the local IP; the Windows firewall hint; the list of players (name, ping) | Start (enabled with 2 or more connected: the session's player count is the connected count); full = start automatically after 3 s |
| Join setup | Address: <IP or domain[:port]> (the last one) | Confirm: Connecting |
| Connecting | "Connecting to <address>…" with seconds | Answer: Lobby (client); errors below |
| Lobby (client) | "Connected to <host name>. Waiting for the host to start." and the list | the host's START begins the game |
| Error | One line with the reason and "Back" | back to the state it came from |

Cases that must each give their own clear message (and never a hang or a crash):

- Host: the port is in use (another copy running, or another program); no network interface with IPv4; UPnP failed
  (why, and the forward-by-hand hint with the local IP); the router maps the port to another PC; a client with another
  build (show both ids); a client with other game data (another variant); a client joining a full or started session
  (it gets `REJECT`); a client leaving the lobby (removed from the list); two players with the same name (the second
  shown as "Name (2)"); the host cancelling the lobby (`BYE` to everyone).
- Client: an empty or malformed address; a name that does not resolve; no answer in 10 s ("Cannot reach <address>: is
  the host's port open?"); `REJECT` for each reason in words ("The host runs another version: <id>", "The host uses
  other game data (another language variant)", "The game is full", "The game has already started"); the host leaving
  the lobby; cancelling while connecting (sends `BYE`).
- During play (the game window, no screen of its own): a player gone for 30 s, a desync, the host quitting: the session
  ends for everyone with `SDL_ShowSimpleMessageBox` naming the reason ("Player 3 (Ana) left the game", "The game went
  out of sync at frame N"), then the program exits with code 4.
- Text input: SDL text input events for the address and the name (keyboard); with only a gamepad, the address field
  offers the last address and Back.

Remembered between runs in the per-user config folder (`~/.config/don/net.ini`, `%APPDATA%\don\net.ini`): name,
last address, last player count. Nothing about a session is remembered.

The CPU check (T-518) runs before this screen: a processor without AVX2 (the build needs x86-64-v3) gets a message box
("This game needs a processor with AVX2 (Intel 2013 or later, AMD Ryzen)") instead of a crash with an illegal
instruction; that check lives in a file compiled for baseline x86-64 and runs first in `main`.

### 7.3 The start screen's look: the owner's LAS Recomp style (2026-10-15, T-523)

Two earlier views were rejected by the owner (SDL debug text; then boxed ImGui buttons in a big centred column with
loud colours, cramped, settings as long click sequences with no Back): "no cumple con ninguna de las directrices".
The reference is the owner's own project **LAS Recomp** (`docs/start_screen.jpg`,
`runtime/ui/launcher.mm`, `runtime/ui/option_rows.{h,mm}`), whose start screen the owner likes. Its code is the owner's
and is reused here, adapted to SDL_Renderer and this model, with its origin noted in the files.

- **Window:** 16:9, first size 1280x720 (or the largest of 1280x720 / 1600x900 / 1920x1080 that fits 75 % of the
  display), resizable, F11 / Alt+Enter fullscreen. Everything scales with the window height (720 = 1.0).
- **Background:** full window, a calm dark gradient (LAS Recomp's fallback: teal-navy `#122228` → `#28463F`, darker
  corners `#0C1418` / `#080C10`) with a soft vignette; no loud colours, no shapes.
- **Fonts:** one friendly rounded sans for everything, bundled in the binary (Nunito, SIL OFL, its licence kept with
  it): title 92, menu 52, small 30 (at 1080 px height; scaled with the window).
- **Main screen**, LAS Recomp's layout: the title "Battle Stadium D.O.N" top-left (text_fx: a soft shadow and a top-to-bottom
  gradient); lower-left, text-only menu items (no boxes), generous line spacing: **PLAY** (the local game at once: the
  game asks how many play; the port takes the multitap layout when more than two controllers are connected, the keyboard
  counting as one; `--players` still wins), **HOST ONLINE** (straight to the lobby, open to 4; START GAME with 2 or more),
  **JOIN ONLINE**, **SETTINGS**, **QUIT**. No player counts on this screen (the owner's decision). The selected
  or hovered item: gold `#FFD640` text with an orange-to-gold gradient over a feathered dark band with a warm glow
  (menu_item). Bottom-left: a status line (a green check + "Game files ready." or what is missing) and "Player: <name>"
  and the build id in small muted text; bottom-right: "Online: ready" or nothing.
- **Settings:** LAS Recomp's two panes (option_rows): on the left, rows with an UPPERCASE label and the value on the right
  (brighter when not the default): NAME (text field), FULLSCREEN (toggle), RENDERER (Vulkan / Software), VIBRATION
  (toggle), ONLINE INPUT DELAY (Auto, 1-10 frames), then **BACK**; on the right, the selected row's description. Choices:
  left / right step them; a mouse click opens a small dropdown list next to the value. Esc / B also goes back.
- **Host lobby, Join, Connecting, client lobby:** the same language: a title, short text rows, the players as rows
  (name left, ping right), the address line with a COPY action, the UPnP / firewall lines in small muted text with an
  icon, and the actions as menu items (START GAME, CANCEL / BACK).
- **Errors and notices:** a centred dark rounded panel over the dimmed screen, the message, and OK.
- Mouse, keyboard and gamepad (LAS Recomp reads the gamepad straight from SDL: confirm / back / d-pad / stick).

The model (7.2's states, LocalSetup, Settings) does not change.

## 8. Session clock (T-506, runtime patch 0074)

`PS2Runtime::setSessionClock(uint64_t epoch_seconds)`. Once set, every host time the guest can see comes from
`epoch_seconds + vsync_tick / 60` (UTC), never from the PC: `sceCdReadClock` (CD.cpp), the time-zone offset in
`GetOsdConfigParam` (0 minutes), and the memory card's dates (`sceMcGetDir`'s "." / ".." and the file times). Unset,
nothing changes. A debug variable `DON_SESSION_EPOCH=<seconds>` sets it without netplay (for the determinism checks).

## 9. Facts from the code survey (2026-10-15)

- The vblank hook (`host.cpp` `gs.set_on_vblank`, called from `HostGs::on_vsync` on the game thread) runs the input
  script's `step(frame)`, merges the devices' pads and publishes them with `pads.publish(port, slot, state)`; the
  guest reads them afterwards (`scePadRead` → `PadTable::read`). Blocking there is allowed (no scheduler lock is held),
  but it must watch for a stop request or `stopGameThread`'s join hangs.
- The guest advances by emulated cycles; host time only decides how long the host sleeps (patch 0038).
- Host time visible to the guest: `sceCdReadClock` (memory-card script code), the OSD time zone at boot, the memory
  card's `GetDir` dates and file times. `rand` is recompiled (guest code), COP0 Count is constant.
- The state line: `hashGuestRam` in `src/game/don_overrides.cpp` (FNV-1a 64 over the 32 MiB of EE RAM, about 3 ms).
- What changes the guest state and so must match: the build, `--sound`, FFmpeg or not, the disc variant, the pad
  layout, the memory card's content and dates, `DON_FP_MODE`, `DON_IOP_BATCH_EE_CYCLES`.
- The GS is never read back by the guest in normal play (the renderer choice cannot change the state; not measured).
- Windows: `ws2_32.lib` and `winsock2.h` are in the xwin sysroot; `don_host` does not link Winsock yet.
