# Decisions

Record of decisions made, with their reason and what was discarded. If a decision changes, a
new entry is added that supersedes the previous one; the old one is not deleted.

The data that supports them is in [knowledge/](knowledge/README.md).

## D1 · Recompile, do not decompile (2026-10-11)

**Decision.** The port is done by static recompilation of the executable. Decompilation is
reserved for specific functions, when there is something to modify.

**Why.** DON has no symbols in either of its two versions. A full decompilation is many months
before seeing the game run; recompilation gets it going in weeks and allows replacing
functions one by one afterwards.

**Discarded.** Full decompilation in the style of the GTA ports (decompiled game code +
`librw`): it is the best final result, but there are no symbols to start from.

## D2 · PS2 base, not GameCube (2026-10-11)

**Decision.** The PS2 version (`SLPS-25675`) is ported.

**Why.**
- The English patch exists only for PS2 and does not touch the executable: one build serves
  Japanese and English.
- There is a common recompiler (PS2Recomp) already tried on DON, and two projects at full
  speed with the chosen architecture.
- The PS2 textures are the same or slightly better.

**Discarded.** GameCube: simpler hardware and better decompilation tools, but it has no
translation, there is no standard recompiler, and porting the translation would cost weeks.

**GameCube remains as a reference** (see [knowledge/06-gamecube-version.md](knowledge/06-gamecube-version.md)).

## D3 · Scope of stage 1 (2026-10-11)

**In.** PC with Windows and Linux (x86-64), gamepad, 60 fps, sound, videos, memory card,
4 players, Japanese and English discs.

**Out, for now.** macOS, browser, more than 4 players, own renderer on top of RenderWare,
real widescreen, mods, online play.

**Technical consequence.** Since macOS and browser are out, runtime code generation (JIT) is
allowed for the vector units.

## D4 · Graphics: the PCSX2 GS as a library (2026-10-11)

**Decision.** The graphics chip is handled by linking the PCSX2 renderer (v2.8.2, Vulkan
backend on both systems) behind an own bridge with a C API. The PS2Recomp GS is not used,
except as support for the `SIGNAL`/`FINISH`/`LABEL` signals.

**Why.** The two projects that declare full speed (Ridge Racer V, Killzone) do it this way.
Those that use the PS2Recomp GS are slow or do not get past boot, and none of the RenderWare
ones has solved it.

**Discarded.**
- PS2Recomp GS (software or OpenGL): insufficient performance, declared by the project itself.
- Own renderer from RenderWare: requires identifying RenderWare without symbols. Moves to a later stage.
- paraLLEl-GS: promising but experimental and compute-only Vulkan.
- Copying the Fukami bridge as is: it brings Ridge Racer V settings and does not build on Windows.
  It is used as an API model.

**Accepted cost.** Building PCSX2 and its dependencies on both systems; GPL-3.0 license.

## D5 · Vector unit VU1: interpreter first, microVU later (2026-10-11)

**Decision.** Until the image is correct, the PS2Recomp VU1 interpreter is used. To reach
60 fps, PCSX2 microVU is linked as a library. Before making the change, the share of frame
time spent in VU1 is measured (decision gate at milestone M2).

**Why.** The interpreter already exists and does not block boot. microVU is compatible with
all PS2 games and avoids dealing with the 141 microprograms one by one.

**Documented alternative.** Compile the microprograms ahead of time (as Fukami does): possible
because they are static in the executable. It would be chosen if macOS or browser is wanted in
the future.

## D6 · Sound: the original modules plus the PCSX2 SPU2 (2026-10-11)

**Decision.** The game's `.IRX` modules are run in the PS2Recomp IOP emulator, and the sound
chip is handled with the PCSX2 SPU2 core, connected through a hook in the emulator.

**Why.** PS2Recomp states that games with `SDRDRV`/`TSNDDRV` must run their real modules.
Rewriting the sound driver (what Fukami did) is game-specific work.

**Risk.** It is the least bounded part of the project. It is bounded by task T-101.

## D7 · Dependencies: pinned clone plus patches, not a fork (2026-10-11)

**Decision.** PS2Recomp and PCSX2 are cloned into `third_party/` (ignored by git) at a pinned
commit, and the patches in `patches/` are applied to them, in the order of the `series` file.

**Why.** It does not require publishing forks or submodules; the repo stays self-contained.

**Review when** the PS2Recomp patches exceed about 25: then a fork with its own branch pays off.

## D8 · Legal model: each user generates from their own disc (2026-10-11)

**Decision.** The repository contains neither the game, nor its data, nor the generated C++,
nor binaries linked with it. The user provides their ISO and builds on their machine.

**Why.** The generated C++ is a mechanical translation of the game code: a derivative work.

**Consequence.** There will be no downloads of a ready-to-play executable. What is delivered
is a one-command procedure.

**Repo license.** GPL-3.0, required by linking PS2Recomp and PCSX2.

## D9 · Window, input and audio output with SDL3 (2026-10-11)

**Decision.** The program creates its window with SDL3 and hands the native handle to the
PCSX2 GS. Gamepads and audio output also go through SDL3. The raylib window that comes with
PS2Recomp is not used.

**Why.** PCSX2 needs a native window handle to create its Vulkan device; SDL3 provides it the
same on Windows, X11 and Wayland, and it is what Fukami and Killzone use.

## D11 · The PCSX2 pieces are built in a container (2026-10-11)

**Decision.** On Linux, the PCSX2 GS, microVU and SPU2 are built inside a Docker container
based on Ubuntu 22.04, with the compiler and dependencies that PCSX2 needs. The result is
libraries with a C API that the executable loads.

**Why.** This machine only has GCC 11 and lacks the development packages; the container avoids
`sudo` and makes the build repeatable. Ubuntu 22.04 is used so that the libc matches the
system one.

**To watch.** If PCSX2 requires a newer C++ standard library than the one in Ubuntu 22.04, the
bridge libraries will statically link their own and expose only C.

**Compiler flags.** The PCSX2 pieces keep PCSX2's own compiler flags. The floating point rules of
`AGENTS.md` (`-ffp-contract=off`, no fast-math) apply to the game code and the runtime, not to
them. Toolchain found by T-103: clang 17 and lld 17, as in PCSX2's own CI.

## D10 · Language of the documentation (2026-10-11)

**Superseded by D12.**

**Decision.** Documentation in Spanish. Identifiers, code file names and commit messages in
English.

## D12 · Everything in the repository is in English (2026-10-11)

**Decision.** Documentation, file and directory names, code, comments, program messages and
commit messages are in English. This replaces D10.

**Why.** The owner's instruction. The repository will be published on GitHub.

## D13 · Windows: built from Linux and smoke-tested under Wine (2026-10-11)

**Decision.** Linux is always the priority platform. The Windows build is produced from Linux
(cross-compiled inside the build container) and its automated checks run under Wine, headless,
with the software renderer. The owner's real Windows PC is used only for the final interactive
validation.

**Constraint.** End users on Windows still have to build on their own machine, because no
binary linked with the game can be distributed (D8). So the toolchain chosen for the
cross-build must also work natively on Windows, with the same CMake presets.

**Why.** The owner has a Windows PC but prefers not to use it as the build machine. Wine 6.0.3
is installed on this machine. A virtual machine would need a Windows image and license and has
no usable GPU.

**Toolchain (decided on 2026-10-11 after T-400).** `clang-cl` and `lld-link` with the Microsoft
CRT and SDK fetched by `xwin`, for everything that is shipped. Measured in T-400
([knowledge/17-windows-toolchain.md](knowledge/17-windows-toolchain.md)): it builds the SPU2
library (its self-test passes under Wine), the GS library, the PS2Recomp runtime and a sample of
the generated code. `llvm-mingw` builds the runtime and the game code too, but not the PCSX2 GS
(PCSX2 does not support MinGW), so choosing it would mean two toolchains.

**Still open.**

- The GS library cannot be validated under the Wine installed here (6.0.3): it lacks Windows 10
  memory functions the library imports, and with stand-ins for them the GS could not create a
  render device. Either a newer Wine placed under `third_party/` (to be tried in a task of M4) or
  the first run on the owner's Windows PC.
- Under `clang-cl` the floating point flags of `AGENTS.md` do not arrive as written:
  `-ffp-contract=off` is ignored (it must be `/clang:-ffp-contract=off`) and PS2Recomp's own
  build files add `/fp:fast`. A patch and a change in the root `CMakeLists.txt` are needed before
  any Windows binary of the game is trusted (T-401).
- The image has clang 17 and the current Microsoft STL asks for clang 19 or newer; a define
  works around it for now.

**Discarded.** Building on the owner's Windows PC as the main route; a Windows virtual machine.

## D14 · Online play: lockstep input sync, host in a star (2026-10-15)

**Decision.** Stage 2 adds online play for up to 4 players. Every player runs the game on their own PC; only pad input
crosses the network. All copies advance a frame only when the host has confirmed the inputs of every player for that
frame (lockstep with an input delay of a few frames). The host is the hub of a star: clients send their input to the
host, the host sends the complete set of each frame to everyone. Raw UDP, no third-party networking library. One session per host:
players join with the host's public IP address or domain (`--net-join HOST[:PORT]`) and install or configure nothing
(they only send outbound UDP). Only the host's UDP port must be reachable: the host's port asks its router to open it
with UPnP (miniupnpc), and when the router does not allow that the host opens it once by hand. No lobby server, no VPN
(the owner's decision, 2026-10-15: "arrancar, conectar y jugar"). During a session the game sees a session clock from the host instead of the PC's clock, and an empty memory card
that is not kept. A state hash is compared every 120 frames; a mismatch stops the session.

**Why.** The port is deterministic (same input, same state, also Linux against Windows under Wine: journals 017 and
031, knowledge 17), and it already injects the pads of each frame at one point on the game thread, so the network only
replaces the source of that input. Rollback (GGPO-style) needs saving and loading the whole machine state several
times a second, which the port cannot do; streaming one host's picture to the others needs no code at all (Parsec)
and is the owner's fallback.

**Changes D3.** "Online play" leaves the "out, for now" list of D3 for stage 2. The design and the task cards are in
[knowledge/22-netplay.md](knowledge/22-netplay.md) and [tasks/M5-online.md](tasks/M5-online.md).

**Discarded.** Rollback (no savestates, and about 5 ms of headroom per frame where 2-8 re-simulated frames are needed);
a full mesh (every peer reachable); ENet or GameNetworkingSockets (a dependency to pin and cross-build for little gain
over a few hundred lines of UDP); joining or reconnecting during a session; a lobby server, join codes, and any VPN
(Tailscale, ZeroTier): players must not install or configure anything.

## D15 · One user folder, decided by the program, the same on every platform (2026-10-16)

**Decision.** Everything the port keeps for a player lives in one folder per user: `%APPDATA%\don` on Windows,
`$XDG_DATA_HOME/don` (else `~/.local/share/don`) on Linux. Inside it, the same names everywhere: `memcards/` (mc0, mc1),
`gs-cache/`, `net.ini`, `don.log` (and `don.old.log`), `launcher.conf`. The program decides it, not the wrappers: the
launcher, the AppImage's AppRun and the Windows package pass no locations. Nothing is written outside that folder (the
online session's empty temporary card, D14, is the one exception). `don.log` is always written, on both platforms and
however the program is started: every start appends a dated block, the messages also still reach the terminal or the
caller's pipe, and past 10 MB the file becomes `don.old.log`. `DON_USER_DIR` moves the whole folder (tests and
developers); `--mc-dir`, `DON_MC_DIR` and `DON_GS_DATA_DIR` stay as overrides. On the first start after this change the
program copies, never moves, the saves it finds in the old places (the AppImage's `don-test/saves`, the Windows
package's `saves\` beside the program) and Linux's `~/.config/don/net.ini`, and says so in the log.

**Why.** On 2026-10-16 the saves were in four places depending on how the game was started (the launcher, the AppImage,
the plain program, the Windows package), the shader cache in three, the log in three or nowhere. A Windows player who
unzipped a new version lost his saves, his shader cache (the slow first game again) and his log; the AppImage and the
launcher did not share the owner's save. The owner: "esas divergencias entre sistemas no son aceptables".

**Discarded.** Keeping the saves beside the program (a portable folder): every new version would start without them.
Following each platform's convention to the letter (Linux config in `~/.config`, cache in `~/.cache`): three folders
instead of one, and a different layout from Windows, for no gain to the player.

## D16 · Exceptions cross the game module through unwind tables (2026-10-25)

**Decision.** The runtime keeps throwing `EeDispatcherTransfer` and `std::runtime_error` through generated frames
(A1 of [tasks/M6-public-release.md](tasks/M6-public-release.md), option a). The game module is compiled with
`-funwind-tables` and exports `don_game_module` with default visibility; the runtime is not changed. The flag goes into
the module build recipe when it becomes a tracked file (T-610/T-614); until then T-607 has no code to deliver.

**Why.** Gate 1 measured both paths in a dlopen'ed freestanding module built with clang 17: (a) the host threw through
the module's frame and caught it (`a: caught 42`), (b) the flag after the call also worked
(`work/t603/gate1.md`, row "exception path chosen"). (a) changes no runtime code, so the linked build and its state
hashes cannot move; (b) would rewrite five throw sites and three catch sites in the scheduler.

**Discarded.** Option (b), the unwinding flag of patch 0054 in place of the throws: more code in the hottest part of
the runtime for no measured gain.
