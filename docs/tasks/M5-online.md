# M5 · Online play for up to 4 players (stage 2)

Decision D14 ([../DECISIONS.md](../DECISIONS.md)); design contract: [../knowledge/22-netplay.md](../knowledge/22-netplay.md)
(read it first: names, files and message layouts are fixed there). Every card below is sized for one agent; the waves
say what can run at the same time.

| Wave | Tasks (in parallel) | Needs |
|---|---|---|
| 1 | T-500, T-501, T-502, T-503, T-504, T-505, T-506, T-507, T-515 | the contract (done) |
| 2 | T-508, then T-509; T-513, T-518 | wave 1 |
| 3 | T-517, T-510, T-511, T-512 | T-509 |
| owner | T-514 | T-510, T-511 |

Rules for every card: no commit; do not edit `patches/ps2recomp/series`, `docs/knowledge/22-netplay.md` or another
card's files; a change of the contract is a stop-and-report (AGENTS.md rule 3); no full regression suite (at most
the routes named in the card); never the owner's display, speakers or memory card; one heavy build at a time through
the container lock (`DON_JOBS=4`). Unit tests: `cmake -S tests/net -B build/net-tests -G Ninja && cmake --build
build/net-tests -j2 && DON_NET_TESTS_DIR=build/net-tests uv run --no-project --with pytest python -I -m pytest tests/net -q`
(each task uses its own build folder `build/net-tests-<task>` while others work, e.g. `-B build/net-tests-501`).

---

### T-500 · Determinism before the network

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1.5 h

**Read first:** knowledge 22 sections 1 and 9; journal 017 (the state line); `tests/regression/routes.json` (route
`multiplayer_match` and its script)

**Output:** a new journal entry `docs/journal/NNN-netplay-determinism.md` (next free number)

**Steps**

1. Binary: `build/player/don-linux/don` (record its `BUILT_FROM.txt` commit). Script: the `multiplayer_match` route's
   script (4 players), with `--players 4 --headless --sound on --mc-dir <a new empty folder>` and
   `DON_TRACE_STATE=60`.
2. Run it twice; compare every `[don] state` line. Then once with `--renderer software` against the default (Vulkan)
   run. Then once with `TZ=Asia/Tokyo` against `TZ=UTC`.
3. Report, per comparison: identical, or the first blank that differs.

**Done when** the journal entry has the four comparisons with the commands and the first and last state line of each run.

**Limits.** Measuring only, no code.

---

### T-501 · Protocol: messages and wire format

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1.5 h

**Read first:** knowledge 22 sections 2 and 3; `src/host/net/net_pad.h`

**Output:** `src/host/net/protocol.h`, `src/host/net/protocol.cpp`, `tests/net/test_net_protocol.cpp`

**Steps**

1. Declare exactly the types and functions of knowledge 22 section 3 in `protocol.h` (namespace `don_net`; include
   `net_pad.h`, `<variant>`, `<vector>`, `<string>`).
2. Implement `encode`/`decode` by hand (no `memcpy` of structs: write each field little-endian; strings as fixed,
   zero-padded fields). `encode` returns an empty vector when a count is out of range (Input 1..16, Frames 1..12 and
   players 2..4, pads.size() consistent) or the datagram would exceed `kMaxDatagram`.
3. Tests: a round trip of every message type with distinctive values; byte-exact checks of the header and of one
   `INPUT` and one `FRAMES`; `decode` rejects a wrong magic, a wrong version, an unknown type, every truncated
   length of a valid datagram (loop over sizes 0..n-1), a count of 0 and of 17; long strings are cut, not overflowed.

**Done when** the unit test command (with `-B build/net-tests-501`) shows `test_net_protocol` passing.

**Limits.** Only the three output files.

---

### T-502 · Lockstep engine

**Model:** Haiku · **Depends on:** nothing · **Time box:** 2 h

**Read first:** knowledge 22 sections 1, 2 and 4; `src/host/net/net_pad.h`

**Output:** `src/host/net/lockstep.h`, `src/host/net/lockstep.cpp`, `tests/net/test_net_lockstep.cpp`

**Steps**

1. Implement the class of knowledge 22 section 4 exactly (storage: a `std::map<uint32_t, Frame>` or a ring of 1024
   frames; `Frame` holds per player a `NetPad` and a "recorded" flag, and a "confirmed" flag).
2. Tests, each a small function:
   - frames 1..delay are confirmed and neutral for every player before anything is submitted;
   - `submit_local(10, p)` returns `10 + delay` and records p for the local player there;
   - host: `complete` only when every player has recorded; `missing` has the right bits; a second `record` for the
     same frame and player does not change it;
   - client: `confirm` from a `FRAMES` set, `confirmed_pad` returns it, `unacked_local` lists exactly the frames after
     `ack_local`, oldest first, at most `max`;
   - host: `confirmed_since(from, 12)` returns the last confirmed frames, at most 12;
   - `forget_before` drops old frames and ignores later `record` calls for them;
   - 4 players with delay 1 and delay 10.

**Done when** the unit test command (with `-B build/net-tests-502`) shows `test_net_lockstep` passing.

**Limits.** Only the three output files. No time, no threads, no I/O in this class.

---

### T-503 · Transports: UDP, simulated network, in-process hub

**Model:** Haiku · **Depends on:** nothing · **Time box:** 2 h

**Read first:** knowledge 22 sections 2 and 5; `src/host/net/transport.h` (the contract: implement it, do not change it)

**Output:** `src/host/net/udp_socket_posix.cpp`, `src/host/net/udp_socket_win32.cpp`, `src/host/net/sim_transport.cpp`,
`tests/net/test_net_transport.cpp`

**Steps**

1. `udp_socket_posix.cpp` (`#ifndef _WIN32` around everything): `open_udp` (socket, bind INADDR_ANY:port, `fcntl`
   O_NONBLOCK, `getsockname` for `local_port`), `receive` with `poll` up to `timeout_ms`, `send` with `sendto`;
   `resolve` with `getaddrinfo(AF_INET, SOCK_DGRAM)` after splitting "host:port" at the last `:` (port 1..65535).
2. `udp_socket_win32.cpp` (`#ifdef _WIN32` around everything): the same with Winsock (`WSAStartup` once through a
   function-static guard, `ioctlsocket(FIONBIO)`, `WSAPoll`, `closesocket`). It is compiled by the Windows build only
   (T-511 checks it); keep it to the same structure as the POSIX file.
3. `sim_transport.cpp`: `with_simulated_network` and `LoopbackHub` as knowledge 22 section 5 says.
4. Tests (POSIX side): two `open_udp(0)` sockets on 127.0.0.1 exchange 100 datagrams of different sizes; `receive`
   with a 50 ms timeout returns false within 40-200 ms when nothing comes; `resolve("127.0.0.1:7777")` and
   `resolve("localhost:7777")` give 127.0.0.1:7777, and `"x:0"`, `"x:70000"`, `"nope"` fail; a `LoopbackHub` with 4
   endpoints delivers to the right one; `with_simulated_network(…, 30, 0, 0.0, 1)` delays by 30 ms (±10) and with
   loss 0.5 over 1000 datagrams loses 400..600; the same seed gives the same losses.

**Done when** the unit test command (with `-B build/net-tests-503`) shows `test_net_transport` passing.

**Limits.** Only the four output files; no third-party library.

---

### T-504 · Host options for the net mode

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** knowledge 22 section 7 (the option list and its errors); `src/host/options.h`, `src/host/options.cpp`
(how options are parsed: `parse_uint`, the checks after the loop, `don_print_usage`); `tests/host/test_host_synthetic.py`

**Output:** `src/host/options.h`, `src/host/options.cpp`, `tests/host/test_host_synthetic.py` (new tests)

**Steps**

1. Fields in `DonHostOptions`: `int net_host_port = 0; std::string net_join; int net_players = 0; int net_delay = 0;
   std::string net_name; bool mute = false;` (0 = not given).
2. Parse them as knowledge 22 section 7 says, with the usage lines; add the combination errors (exit 2).
3. Do not use the fields anywhere else (T-509 does).
4. Tests in `test_host_synthetic.py`, like `test_exit_code_2_on_bad_argument`: each error case exits 2 with a message
   naming the option; a valid `--net-host 7777 --net-players 4 --net-delay 2` is accepted by the parser (the run
   itself may fail later: check only that the exit code is not 2 and the message is not an argument error).

**Done when** `bash tools/pipeline/build_host.sh` builds and `uv run --no-project --with pytest --with pillow --with numpy
python -I -m pytest tests/host -q` passes.

**Limits.** Only the output files.

---

### T-505 · Build id and game data hash

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** knowledge 22 sections 2 and 3 (`HELLO`); `src/host/CMakeLists.txt`

**Output:** `src/host/build_id.h`, `src/host/build_id.cpp`, the lines that define `DON_BUILD_ID` in
`src/host/CMakeLists.txt`, `src/host/net/game_hash.h`, `src/host/net/game_hash.cpp`, `tests/net/test_net_game_hash.cpp`

**Steps**

1. `const char *don_build_id();` returns `DON_BUILD_ID`, at most 47 characters: `<12-hex commit>[-dirty]-<ffmpeg|noffmpeg>`.
   In `src/host/CMakeLists.txt`: `execute_process(git -C ${CMAKE_SOURCE_DIR} rev-parse --short=12 HEAD)` and `git
   diff --quiet HEAD` at configure time (unknown → `unknown`), `DON_ENABLE_FFMPEG` for the suffix, and
   `set_source_files_properties(build_id.cpp PROPERTIES COMPILE_DEFINITIONS DON_BUILD_ID="…")` so only that file
   rebuilds when it changes.
2. `uint64_t hash_game_files(const std::filesystem::path &elf, std::string &error);` FNV-1a 64 over: the ELF's bytes,
   then for `DATA.AFS`, `BGM.AFS`, `SE.AFS`, `MOVIE.AFS` in that order, in the ELF's folder (case as on disk; a missing
   file is an error): its size as 8 little-endian bytes, then its bytes. Read in 1 MiB blocks.
3. Test with a temporary folder of small fake files (never the game's): equal folders give equal hashes, one changed
   byte or one missing file changes it or fails.

**Done when** the unit test command (with `-B build/net-tests-505`) shows `test_net_game_hash` passing and
`DON_JOBS=4 bash tools/pipeline/build.sh linux-dev` builds.

**Limits.** Only the output files.

---

### T-506 · Session clock (runtime patch 0074)

**Model:** Sonnet · **Depends on:** nothing · **Time box:** 2 h

**Read first:** knowledge 22 section 8 and 9; the runtime files `Kernel/Stubs/CD.cpp` (`sceCdReadClock`),
`Kernel/Syscalls/System.cpp` and `Syscalls/Helpers/Runtime.h` (the OSD time zone), `Kernel/Stubs/MemoryCard.cpp`
(`sceMcGetDir` dates, `writeMcDateTime`)

**Output:** `patches/ps2recomp/0074-session-clock.patch` (not added to the series), `tests/runtime/session_clock_test.cpp`
and its line in `tests/runtime/CMakeLists.txt` (in your scratch build only; give the line in the report)

**Steps**

1. In a scratch clone of `third_party/PS2Recomp` with the series applied (never the shared tree): `PS2Runtime::
   setSessionClock(uint64_t epoch_seconds)` and the use of it in the three places of knowledge 22 section 8, plus
   `DON_SESSION_EPOCH` read once at runtime start.
2. Test: with the clock set, `sceCdReadClock` gives epoch + ticks/60 in BCD; the time-zone offset is 0; `GetDir`
   dates equal the session time; unset, the old behaviour.
3. With the game (your own build folder against your scratch tree): `DON_SESSION_EPOCH=1700000000 DON_TRACE_STATE=60`
   on the `multiplayer_match` route's script, `TZ=UTC` and `TZ=Asia/Tokyo`: identical state lines.

**Done when** the test fails without the patch and passes with it, and the two runs' state lines are identical.

**Limits.** Scratch tree and build only.

---

### T-507 · Launcher: host and join

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** knowledge 22 section 7; `tools/launcher/launch.py` (`split_args`, `build_command`, `main`);
`tests/test_launcher.py`

**Output:** `tools/launcher/launch.py`, `tests/test_launcher.py`

**Steps**

1. `launch.sh <variant> --host [--port P] [--players N] [--delay D]` adds `--net-host P` (default 7777),
   `--net-players N` (default 2), `--net-delay D` if given; `launch.sh <variant> --join HOST[:PORT]` adds
   `--net-join HOST:PORT` (default port 7777). These options come before `--`; what follows `--` passes as today.
2. With `--host`, before starting, print the local addresses others on the same network can join: every non-loopback
   IPv4 of this PC (from a UDP socket "connected" to 192.0.2.1), with the port. The port itself prints the public address
   after UPnP (T-515).
3. Tests: the argv for `--host`, `--host --port 9000 --players 4 --delay 2`, `--join 10.20.30.5`, `--join h:9000`;
   `--host` with `--join` is an error.

**Done when** `uv run --no-project --with pytest python -I -m pytest tests/test_launcher.py -q` passes.

**Limits.** Only the output files.

---

### T-508 · Netplay coordinator

**Model:** Sonnet · **Depends on:** T-501, T-502, T-503 · **Time box:** 3 h

**Read first:** knowledge 22 sections 1, 3, 4, 5 and 6; the three tasks' headers

**Output:** `src/host/net/netplay.h`, `src/host/net/netplay.cpp`, `tests/net/test_net_netplay.cpp`

**Steps**

1. Implement `Netplay` exactly as knowledge 22 section 6 says, on top of `protocol`, `Lockstep` and `Transport`.
2. Tests with `LoopbackHub` and one thread per peer: 2 and 4 peers connect and step 600 frames with scripted local
   pads (player p presses a pattern derived from p and f); every peer's `out` sets are identical frame by frame and
   equal to the pads submitted `delay` frames earlier; the same over `with_simulated_network` (40 ms ± 10, 5 % loss);
   a client with another `build_id` gets `REJECT`; equal hashes pass, a different hash gives `DESYNC` on every peer; a
   client that stops sending makes the host's `step` fail after `abort_after_ms` (set it to 1000 in the test).

**Done when** the unit test command (with `-B build/net-tests-508`) passes, all of it.

**Limits.** Only the output files.

---

### T-509 · The net mode in the host

**Model:** Sonnet · **Depends on:** T-504, T-505, T-506, T-508 · **Time box:** 3 h

**Read first:** knowledge 22 sections 6, 7 and 9; `src/host/host.cpp` (the vblank hook, `publish_devices`, the video
probe pattern in `src/host/video_skip.h` and `src/game/don_overrides.cpp`)

**Output:** `src/host/host.cpp`, `src/host/CMakeLists.txt` (link `ws2_32` on Windows), `src/game/don_overrides.cpp`
(expose the RAM hash), a small `src/host/net_hash.h` (the function pointer), the series line for 0074 proposed in the
report

**Steps**

1. Everything in knowledge 22 section 7, in order. The shared runtime tree gets 0074 only when the orchestrator adds it
   to the series: until then build against a scratch tree with the series + 0074 (`-DDON_RUNTIME_DIR`).
2. Check by hand with two headless instances on this PC (`--headless --sound on --mute`, an input script for the host
   that goes to a 2-player fight): host `--net-host 7777 --net-players 2`, client `--net-join 127.0.0.1:7777`; both
   print the same hash lines; stopping the client ends the host with a clear message.

**Done when** the two instances reach a fight and run 5 minutes with no desync, logs in the report.

**Limits.** The output files only; no full regression suite (`-k fight_practice` once to show the non-net path is
unchanged).

---

### T-510 · Loopback tests of the whole game, 2 and 4 players

**Model:** Haiku · **Depends on:** T-509 · **Time box:** 1.5 h

**Output:** `tests/netplay/test_netplay_loopback.py` (pytest, marked `slow`), input scripts in `tools/run/scripts/`

**Steps**

1. A pytest that starts N headless instances of the given binary (`DON_BINARY`, default `build/linux-dev/don`) on
   127.0.0.1 with free ports, each with its own input script (only the host's script navigates menus; the others
   press attack buttons in the fight), runs them 5 minutes, and checks: all exit 0, no `DESYNC`, the same hash lines.
2. Cases: 2 players; 4 players; 4 players with `DON_NET_SIM_LATENCY_MS=40 DON_NET_SIM_JITTER_MS=10 DON_NET_SIM_LOSS=0.05`.

**Done when** the three cases pass, output in the report.

---

### T-511 · Windows netplay

**Model:** Haiku · **Depends on:** T-509 · **Time box:** 2 h

**Steps**

1. Build the Windows package (`tools/pipeline/package_windows.py`, from the committed HEAD that includes T-509).
2. Under Wine, headless: a Windows client joins a Linux host on 127.0.0.1 (2 players, 5 minutes): no desync, the same
   hash lines.

**Done when** both runs' logs show the same hash lines and no desync.

---

### T-512 · Documentation: playing online

**Model:** Haiku · **Depends on:** T-509 · **Time box:** 45 min

**Output:** a section "Playing online" in `docs/RUNNING.md`; the README's feature list

**Steps.** For a player: what everyone needs (the same build: the launcher prints its commit; the same game variant),
how to host and join with the launcher (joiners type the host's public IP or domain and nothing else; they install and
configure nothing), how the host becomes reachable (UPnP, printed at start; otherwise forward UDP 7777 on the router once,
with the steps for a typical router); the Windows firewall prompt on the host; the input delay
option; what ends a session (desync, a player gone 30 s); that nothing is saved online; the troubleshooting lines
(`[don-net]` messages).

**Done when** the section exists and every command in it was run once (headless) to check it.

---

### T-513 · Determinism with the session clock, across PCs' settings

**Model:** Haiku · **Depends on:** T-506 · **Time box:** 1 h

**Steps.** With 0074: the `multiplayer_match` script with `DON_SESSION_EPOCH=1700000000`, comparing state lines for
`TZ=UTC` against `TZ=Asia/Tokyo`, Vulkan against software, an empty card folder created now against one created an
hour ago (touch its folders' times). All identical. Append to the T-500 journal entry.

---

### T-514 · The owner's test (owner)

LAN with two PCs, then over the internet (joining by the host's public IP or domain), 2 then 4 players: a full match; feel of the input delay at
`--delay 2`, `3`, `4`.

---

---

### T-515 · UPnP: the host opens its own port

**Model:** DeepSeek · **Depends on:** T-503 · **Time box:** 1.5 h

**Read first:** knowledge 22 section 7.1

**Output:** `tools/setup/build_miniupnpc.sh` (pinned tarball with its SHA-256, built static in the container for Linux,
and with clang-cl in the Windows container like `build_ffmpeg_windows.sh`, into `third_party/miniupnpc/{linux,windows}`),
`src/host/net/upnp.h`, `src/host/net/upnp.cpp` (the only file that includes miniupnpc), the link lines in
`src/host/CMakeLists.txt` (guarded: without the prefix the port builds and `upnp_open` reports "not built with UPnP"),
`tests/net/test_net_upnp.cpp` (the parts that need no router: the "not available" path, the log text)

**Steps.** As knowledge 22 section 7.1. Check on this LAN once: `upnp_open(7777)` reports either the mapping and the
external address, or why not; `upnp_close` removes it (list the router's mappings before and after with miniupnpc's
`upnpc -l` built by the script). Never leave a mapping behind.

**Done when** the unit test passes and the LAN check's output is in the report.

---

### T-517 · The start screen

**Model:** DeepSeek · **Depends on:** T-508, T-509, T-515 (their interfaces are in knowledge 22; write against them, wire
up when they exist) · **Time box:** 3 h

**Read first:** knowledge 22 section 7.2 (every state and every case listed there is part of the task); sections 6, 7, 7.1

**Output:** `src/host/start_screen.h`, `src/host/start_screen.cpp` (SDL3 window + renderer + `SDL_RenderDebugText`;
the state machine of 7.2 kept apart from drawing, in `src/host/start_screen_model.h/.cpp`, so it can be tested without
a display), the call in `src/host/host.cpp` before the game window opens (only when no net, headless or script option
was given), `src/host/net_config_file.h/.cpp` (net.ini), `tests/host/start_screen_model_test.cpp` (or in tests/net if it
needs no SDL) driving the model through every case of 7.2 with a fake netplay/UPnP layer

**Steps**

1. The model: states, transitions, and the message for each case of 7.2, with the keys (arrows/D-pad, Enter/Cross or
   the layout's confirm, Esc/back) as inputs; no SDL in it.
2. The view: draws the model's state each frame (title, items, the selected one highlighted, the lists, the error);
   reads SDL events (keys, gamepad, text input) into model inputs.
3. The host's lobby and the client's connecting/lobby screens poll `Netplay` (non-blocking steps of `connect`; add
   a non-blocking variant to `Netplay` only if knowledge 22 section 6 cannot express it: if so, stop and report the
   exact signature you need before changing it).
4. During play, the message boxes of 7.2 and exit code 4.
5. Check: the model test passes every case; then by hand on this PC, two instances (one hosts, one joins on
   127.0.0.1) through the screens to a 2-player fight, and each error case you can provoke locally (wrong address,
   port in use, other build id via an environment override for tests, cancel in each state). Screenshots of each
   screen (`--shots` cannot see this window: use SDL to save the renderer's pixels when `DON_START_SCREEN_SHOT=<dir>`).

**Done when** the model test passes and the screenshots of every screen and error are in the report.

---

### T-518 · A clear message on a processor without AVX2

**Model:** DeepSeek · **Depends on:** nothing · **Time box:** 45 min

**Output:** `src/host/cpu_check.cpp` (compiled with `-march=x86-64`, not v3: set it per file in `src/host/CMakeLists.txt`),
its call first in `main` (`src/host/main.cpp`)

**Steps.** `__builtin_cpu_supports("avx2")`, `"fma"`, `"bmi2"` (and the MSVC/clang-cl equivalent with `__cpuid` on
Windows); if any is missing, `SDL_ShowSimpleMessageBox` with knowledge 22 section 7.2's text (and the same on stderr),
exit 5. Check: the binary's `main` disassembly before the call has no AVX instruction (`objdump -d`); the old test PC (an
Ivy Bridge Xeon) shows the message instead of "illegal instruction": copy only to `/tmp` there, run headless, delete.

**Done when** the old test PC prints the message and exits 5, and this PC starts as before.

## Discarded (D14)

A lobby server with join codes, hole punching or relays, and any VPN: players type the host's IP or domain and play.
