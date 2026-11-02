# Running the port

For whoever wants to play or look at the game, not to build it (the build from a clean clone is in [BUILDING.md](BUILDING.md)). Everything is run from the repository root.
Nothing of the game is in this repository: you need your own disc images (see
[knowledge/01-game-and-discs.md](knowledge/01-game-and-discs.md)).

## The short way: the launcher

```bash
tools/launcher/launch.sh            # Linux (on Windows: tools\launcher\launch.bat)
```

A small window asks for the language: 日本語, English with Japanese voices, English with English voices. The
last choice is preselected and remembered; Cancel or closing the window starts nothing. Without a display it
asks in the terminal. The first time a variant is chosen it makes that variant's folder (a few seconds) in
`~/.local/share/don/variants/<variant>/` (`%APPDATA%\don\variants\` on Windows). The saves are not there: see
"Saving".

### The player folder (a binary that changes only when you say so)

`build/linux-dev/don` is the developers' binary: it is rebuilt all day from a working tree that has other people's unfinished
changes. To play, make a player folder from the **committed** `HEAD` only:

```bash
uv run --no-project python -I tools/pipeline/package_linux.py      # DON_JOBS=4 compile jobs by default
```

It builds in its own folders (`work/player-src`, a detached worktree of `HEAD`; `work/player-ps2recomp`, a PS2Recomp tree with
`patches/ps2recomp/series` applied; `build/player-build/`) and writes `build/player/don-linux/`: `don`, the GS, VU and SPU2
libraries, `resources/`, SDL3 and the other libraries from the build image, all with run path `$ORIGIN`, plus `BUILT_FROM.txt`
(the commit). It ends with an `ldd` check that nothing resolves into `build/linux-dev` or `third_party`. The first run is a full
build (the game alone is about 8 to 15 minutes at 4 jobs, then the GS library); the next ones rebuild only what the new commits
changed, and the folder is replaced in one step. To update, commit, then run the same command. The launcher uses the folder when it
exists and `DON_BIN` is not set, and prints `port: <path> (player folder, commit <hash>: <subject>)` before it starts; `DON_BIN`
still overrides, and without the folder it falls back to `build/linux-dev/don` and says so. `third_party/` and `generated/` are
shared, not versioned: the player is built against the ones in the main checkout.

The GS library keeps its shader and pipeline cache in the user folder (see "Where the port keeps your things"), so a
refresh keeps it; deleting it only brings back the first-time stutters of each scene.

Other ways to call it (`launch.sh` is `uv run --no-project python -I tools/launcher/launch.py`):

```bash
tools/launcher/launch.sh en-jpvoice          # skip the window: jp, en-jpvoice or en-envoice
tools/launcher/launch.sh --list              # each variant: available / needs preparing / missing (what)
tools/launcher/launch.sh en-envoice --print  # show the command instead of running it
tools/launcher/launch.sh jp -- --renderer software   # what follows -- goes to the port unchanged
```

Where it looks for things, in this order: its settings file (`launcher.conf` in the user folder:
`~/.local/share/don/launcher.conf`, or `%APPDATA%\don\launcher.conf`; `NAME=value` lines; an older
`~/.config/don/launcher.conf` is copied there once), the environment, `local.env` in the repository root
(read as text), then the checkout's usual places. Settings: `DON_BIN` (the port; default
`build/player/don-linux/don` if that player folder exists, else `build/linux-dev/don`), `DON_DISC_DIR` (the Japanese disc's extracted files; default `disc/`), `DON_ISO` (the
Japanese image; used to extract those files if the folder lacks them), `DON_EN_JPVOICE` and `DON_EN_ENVOICE`
(an English image, a folder with its extracted `DATA.AFS`, or a folder holding `disc.iso`; defaults
`en_patch/jpvoice` and `en_patch/envoice`). A variant whose files are not found is shown as "not available"
with what is missing. The launcher passes no folders to the port: the port keeps its things in the user folder
by itself (see "Where the port keeps your things"); `DON_MC_DIR` still overrides the memory cards' folder.

The launcher starts the port on the variant's folder and gives it no `--iso`, and removes `DON_ISO` from the
port's environment, so an English game never gets the Japanese image by accident. (To give an image anyway:
`-- --iso file.iso`.)

## The long way, by hand

The port takes the game's files from the folder of the executable it is given. One folder per variant
(Japanese, English with Japanese voices, English with English voices) is made by a tool, once:

```bash
set -a; . ./local.env; set +a        # DON_ISO and the other paths of this machine (local.env.example)

# the Japanese disc's files, once (they are the base of every variant); skip it if disc/ already has them.
# (tools/disc/extract.sh does not take the .AFS files; the port needs them, MOVIE.AFS included)
uv run --no-project python -I tools/disc/verify_disc.py "$DON_ISO"
7z x -y -odisc "$DON_ISO" SLPS_256.75 SYSTEM.CNF DIR.DIR MODULES DATA.AFS SE.AFS BGM.AFS MOVIE.AFS

# English, Japanese voices (use en_patch/envoice/disc.iso for English voices)
uv run --no-project python -I tools/disc/make_variant_dir.py disc en_patch/jpvoice/disc.iso work/variants/en-jpvoice
```

It prints which variant it recognised (by the hash of `DATA.AFS`) and the executable to run. Then:

```bash
build/linux-dev/don --iso en_patch/jpvoice/disc.iso work/variants/en-jpvoice/SLPS_256.75
```

`--iso` takes the image of the same variant. (In the runs measured so far the port reads every byte from the
folder and never opens the image, but give it the right one.) For the Japanese game the same way:
`make_variant_dir.py disc disc work/variants/jp` and `--iso "$DON_ISO"`.

Until the shared build `build/linux-dev` has today's fixes, the same commands work with `build/linux-3d/don`
and `DON_VU1=microvu` in front: `DON_VU1=microvu build/linux-3d/don --iso ... .../SLPS_256.75`.

The window needs a Vulkan GPU; without one add `--renderer software` (slow). The memory cards, the cache and
the log go to the user folder, as with every other way of starting the port (see "Where the port keeps your things").

### On Windows, from the packaged folder

**Make or refresh the package** (on the Linux machine, from the main checkout; one command):

```bash
uv run --no-project python -I tools/pipeline/package_windows.py      # or: bash tools/pipeline/package_windows.sh; DON_JOBS=4
```

Like the Linux player folder, it builds the **committed** `HEAD` only: a detached worktree (`work/player-src-win`), a PS2Recomp tree with
`patches/ps2recomp/series` applied (`work/player-ps2recomp-win`) and a PCSX2 tree with *all* of `patches/pcsx2/series`, including the
clang-cl fixes the shared `third_party/pcsx2` (the Linux builds') does not have (`work/player-pcsx2-win`, a local clone of it). It
cross-builds in the Windows container (the same lock as every container run; the first run is a full build of about 25 to 30 minutes at 4 jobs,
later ones rebuild what changed) and replaces `build/windows-package/don-windows/` in one step. The folder holds `BUILT_FROM.txt` with the
commit. To update: commit, then run the same command.

**On the Windows PC:**

1. Copy the whole folder `don-windows` anywhere (Documents, a USB stick). It is about 135 MB and holds no game files.
2. Install the Microsoft Visual C++ Redistributable 2015-2022 (x64) once.
3. Put **your own** copy of the game's files in a folder named `game` beside the program: that folder must hold
   `SLPS_256.75` (the contents of your own disc, extracted to a folder on your disk). The program reads the game
   from there; there is no setting to write.
4. Double-click `Battle Stadium DON.exe`. Windows very likely shows the blue "Windows protected your PC" box the
   first time (only because the program is not signed with a paid certificate): **More info**, then **Run anyway**.
   The saves, the cache and `don.log` (what the program does, any message or error) go to `%APPDATA%\don\`, not
   to the program's folder, so a new version unzipped anywhere keeps them (see "Where the port keeps your things").
   To pass options, run it from a command prompt in that folder instead: `"Battle Stadium DON.exe" --sound off`,
   `--players 4`.

The processor must have AVX2 (Intel 2013 or later, AMD Ryzen); on one without it the program shows a message and does
not start. This is not the launcher above: it has no variant picker, so for the English variants copy the variant
folder the launcher makes into `game`. Only tried under Wine so far, headless; not on a real Windows PC.

## Keys

The keyboard is player 1 (the comment at the top of `src/host/input.h` is the source):

| Key | Pad |
|---|---|
| **Enter** | **Start** |
| **L** | **Circle: confirm / OK** (this game confirms with Circle) |
| **K** | **Cross: cancel / back** |
| J / I | Square / Triangle |
| Arrow keys | D-pad |
| W A S D | left stick |
| Q / E | L1 / R1 |
| 1 / 3 | L2 / R2 |
| Backspace | Select |
| C / V | L3 / R3 |

### In a fight (Naruto, practice match; journal 026)

Keyboard keys from the table above (Square = **J**, Triangle = **I**, Circle = **L**, Cross = **K**, D-pad = arrow keys).
"Forward" is the direction the fighter faces. The source column says where each line comes from. The game's own
"Skill list" is in the pause menu (**Enter** in a fight, then Down twice to SKILL LIST, **L**); its footer says
"*for key configuration type A": the game has other button layouts, and the options screen (Audio mode, BGM, SE,
Auto save, Save, Load) has no button configuration; it is at the character select (below), and the table here is
layout A, the default.

| Function | Pad | Keyboard | Source |
|---|---|---|---|
| Move left / right | D-pad Left / Right | Left / Right | seen in a fight |
| Jump | Cross, or D-pad Up | K, or Up | seen in a fight: Cross (one tap, airborne 10 blanks later; Cross + Right jumps forward) and Up (two taps, two jumps) |
| Dash (run) | Forward tapped twice | Right, Right | seen in a fight (one run: Naruto runs after two 3-blank taps 8 blanks apart); not on the game's list |
| Crouch | hold Down | Down | seen in a fight (crouches ~10 blanks after the press, stands ~10 after the release) |
| **Guard** | hold L2 or R2 | hold 1 or 3 | the game's screen ("L2 R2 Guard", type A); seen in a fight: held, Naruto gets a blue then yellow aura and Sasuke's kick lands on it with no hit spark and no knock-down (no-input run: spark and knock-down), bars unchanged by eye |
| Basic combo | Square x3 | J x3 | the game's screen; seen: a punch about 15 blanks after the first tap, three taps 12 blanks apart gave a punch string ending in a kick |
| Upper | Up + Square | Up + J | the game's screen; seen: a different move starts 7 blanks after the tap (arm raised), not further identified |
| Hard punch | Forward + Square | Forward + J | the game's screen; seen: a lunging kick that hit the opponent (spark) about 30 blanks after the tap |
| Sweep | Down + Square | Down + J | the game's screen; seen: Naruto crouches after the tap, not identified further |
| Slam (in the air) / Dash punch (mid-dash) | Square | J | the game's screen |
| Giant windmill (holdable); in the air also | Triangle | I | the game's screen; seen in a fight (a spinning move with a large shuriken shape) |
| Shadow clone jutsu | hold Triangle | hold I | the game's screen; seen: holding 60 blanks shows a spark charge ~15 blanks later, then on release a dash with shurikens (not confirmed as clones) |
| Uzumaki rising dragon / barrage / summoning | Up / Forward / Down + Triangle | Up / Forward / Down + I | the game's screen |
| Rising assault / exploding dive (in the air) | Up / Down + Triangle | Up / Down + I | the game's screen |
| Rasengan = Naruto's ultra move (holdable) | Circle | L | the game's screen; seen in a fight with sound on: after a few hits given, a Circle tap gives a red cut-in with Naruto's face about 10 blanks later, then a blue swirl hit (twice, 900 blanks apart); with no gauge (early taps) nothing visible. What fills the gauge was not isolated |
| Pause menu (Resume, Com state Stand/Auto, Skill list, Reset, Reselect) | Start | Enter | seen in a fight |
| Grab / throw item, pick up item | R1 | E | the game's screen (type A: "R1 Grab/Throw item", Square also "Pick up/use item"); not tried: no item was on the stage; R1 held 140 blanks changed nothing |
| Normal / special / ultra move, jump | Square / Triangle / Circle / Cross | J / I / L / K | the game's key configuration screen (type A: "Normal move", "Special move", "Ultra move", "Jump") |
| Vibration | | | the key configuration panel: ON / OFF (Left/Right); no vibration on a PC |

#### Button layouts (key configuration)

At the character select (Practice, Multiplayer and the main mode were checked only in Multiplayer: the footer there
says "Triangle: key configure"), **Triangle** opens a "KEY CONFIG" panel for that pad's player (1P with P1's Triangle,
2P with P2's), **Down / Up** change the layout, **Left / Right** toggle vibration, **Circle** closes it. Each player
has their own. The layouts as the panel prints them:

| Layout | Square | Triangle | Circle | Cross | R1 | L2 / R2 | other |
|---|---|---|---|---|---|---|---|
| A (opens with it) | Normal move, Pick up/use item | Special move | Ultra move | Jump | Grab/Throw item | Guard | |
| B (one Down) | Normal move, use item | Special move | Ultra move | Jump | Guard (R1 R2) | | right stick (R): Pick up item, Grab/Throw item |
| C (two Downs) | Normal move, use item | Special move | Pick up item, Grab/Throw item | Jump | Ultra move | Guard (L2 R2) | |
| D (three Downs) | Normal move, use item | Jump | Ultra move | Special move | Pick up item, Grab/Throw item | Guard (L2 R2) | |

(Layout letters are by order of appearance; a fourth Down was not tried, so there may be more.) The skill list's
"*for key configuration type A" and this table agree with each other on Square, Triangle and Circle for A.

### Two players (journal 026)

The keyboard drives player 1 only (`src/host/input.h`: the keyboard is "player 0 only, fixed"). Player 2 needs a second
gamepad, or the input-script layer (`P2:` prefix, docs/INPUT_SCRIPTS.md), which is how it was tried. The game sees a
second pad: in the practice match it moves and attacks Sasuke (`P2: press LEFT` walked him toward Naruto) and with the
pad connected but idle he stays still. The main menu's **Multiplayer** (the item above Tickets Mode) opens an "ENTRY
EDITING" screen with four slots (1P, 2P, Com, Com), then the character select, the stage list and a four-fighter match
with a 180 timer that ends on the timer and returns to that entry screen. A pad is only "plugged in" for the game
if something presses it; pads 3 and 4 were connected by script and the entry screen still showed slots 3 and 4 as Com
(not checked further).

A gamepad also works, **by position**: the right face button (Xbox B) is Circle = confirm, the bottom one
(Xbox A) is Cross = cancel, left (X) is Square, top (Y) is Triangle; Back is Select, Start is Start, the
D-pad and the left stick move, shoulders and triggers are L1/R1 and L2/R2. (Written from the code; no real
gamepad has been tried yet.)

## What to expect at the start

1. A memory-card dialog ("There is no game data on the memory card ... Do you wanna continue the game?",
   YES / NO). The cursor starts on NO: press the **left arrow**, then **L** to answer YES.
2. The health warning and the publisher logos, then the opening movie. **Build with FFmpeg** (`-DDON_ENABLE_FFMPEG=ON`,
   after `tools/setup/build_ffmpeg.sh`): the game's own video player runs, the movie plays with its sound, and **Enter**
   (Start) skips it; when it ends or is skipped the title follows. `DON_MOVIE_SKIP_AFTER=<blanks>` makes the port
   press Start for you after that many vertical blanks (the regression routes use 240). **Build without FFmpeg**
   (the default): there is nothing to decode, so the screen is black for about 4 seconds (240 vertical blanks) and
   the port presses Start for the game's player, then the title follows (`DON_MOVIE_SKIP_AFTER=<blanks>` changes
   the wait). `DON_PLAY_MOVIES=1` turns the port's pressing off in either build (without FFmpeg the player then
   shows black pictures for the length of the movie, or until you press **Enter**).
3. The title ("PRESS START button"): **Enter**, within about 3 seconds of it appearing (see the next section).
   Then the main menu.

At the menu: **L** confirms, **K** goes back, up and down arrows move.

## Checking that the game still works

After a change, with the build in `build/linux-dev/don` and the English variant in `en_patch/jpvoice/disc/` (the long way above), one command runs four real routes headless (about 15 minutes, no sound, no window) and compares the landmark pictures:

    uv run --no-project --with pytest python -I -m pytest tests/regression -m slow -q

It checks that each route exits 0, that the frame loop never stops, and that the md5 of each landmark shot is the recorded one (`tests/regression/routes.json`); a failure says the route, the blank and where the shot was left (`work/regression/`). After a change that is meant to alter pictures, re-record on purpose with `tests/regression/record.py` (see `tests/README.md`). Routes: `tools/run/scripts/`.

## Where the port keeps your things

One folder per user, the same however the port is started (the launcher, the AppImage, the program run directly,
the Windows package) and with the same contents on both systems (decision D15):

| System | The user folder |
|---|---|
| Linux | `$XDG_DATA_HOME/don`, usually `~/.local/share/don` |
| Windows | `%APPDATA%\don` |

| Inside | What it is |
|---|---|
| `memcards/` | the memory cards, `mc0` and `mc1` |
| `gs-cache/` | the shader and pipeline cache (deleting it brings back the first-time stutters) |
| `net.ini` | the start screen's settings: name, last address, host port, renderer, fullscreen... |
| `don.log` | what the program did. Every start adds a block at the end, under `===== Battle Stadium D.O.N started <date> (pid, build) =====`; the messages still reach the terminal too when there is one. Past 10 MB it becomes `don.old.log`. A start that did not end normally is noted at the next start. Send this file with a report of a problem |
| `launcher.conf` | the launcher's settings (developers) |

Nothing is written anywhere else, except an online session's empty temporary memory card (decision D14).
`DON_USER_DIR` moves the whole folder (tests and developers); `--mc-dir` / `DON_MC_DIR` and `DON_GS_DATA_DIR` still
override the cards and the cache.

The first start of a version with this folder copies (never moves) what it finds in the old places when the new one
has none, and writes it in `don.log`: the cards of the AppImage (`~/.local/share/don-test/saves`) or of the Windows
package (`saves\` beside the program), and `~/.config/don/net.ini`. The old copies are left where they were.

## Saving

The game saves to its memory card from the main menu: OPTIONS, then MEMORY CARD, SAVE (YES at "Do you want to
save the game?"; it ends with "Save ... completed"). LOAD, next to it, reads the save back. A save is three files
in a folder, `BISLPS-25675game/`, inside the card folder `mc0`, in `memcards/` of the user folder (above). The
three language variants share that card.

## Known not to work yet

State of 2026-10-15 (videos, music in every mode, the freeze after a few fights and most loading stutters were
fixed on 2026-10-14 and 15: journals 011, 034, 037, 038).

- Videos play (with the FFmpeg build, the default of the Linux presets) and Start skips them (journal 034).
- On the first start after the computer boots (game files not yet in memory) there are still stutters during the
  opening movie and the first screens, while the game folder is read into memory in the background; later starts
  are smooth (journal 038; `DON_NO_WARMUP=1` turns that reading off).
- After Start on the title, on menu changes and on NOW LOADING the picture holds still for a moment (about 0.3-2 s):
  the game itself waits there for its sound processor (a music fade), as on the console; kept as it is (journal 038).
- Two players by default (a second gamepad); the keyboard is player 1 only. For three or four players start with `--players 4` (see "Three or four players" below)
  (the game's own four layouts work: see the fight table). Vibration: the game's own setting (Vibration ON/OFF in the key configuration, Triangle on the character select) drives the motors of a gamepad (the keyboard has none); `--rumble off` turns it off in the port (for a pad whose motors misbehave). Not tried on a real pad.
- Sound is on by default (`--sound off` turns it off; without the sound library the port says so in one
  `[don]` line and runs silent).
- Picking up and throwing items and throwing an opponent have not been seen working (not tried on purpose).

## Quit

**Esc**, or close the window.

## Three or four players

`--players 4` makes the port present a multitap in port 0, so the game reads four pads. The default (`--players 2`)
is the two-port layout, unchanged. It is an option, not automatic, because the game decides between two ports and a
multitap when it starts polling, and the topology is fixed when the program starts: a third gamepad connected later
could not be honoured without switching modes in the middle of a menu.

- Player 1 is the keyboard; gamepads fill players 2 to 4 in the
  order they were connected.
- On the Multiplayer entry screen a slot shows PLAYER only when its pad is plugged in, so press a button on each
  pad (the game plugs a pad in when it is pressed; the scripts do `P3: tap SELECT`), then edit the slots with Circle.
- Input scripts accept `P3:` and `P4:` (docs/INPUT_SCRIPTS.md). Written from the code and headless runs; not tried
  with real gamepads.

## Playing online

**Being finished: not yet tested between two PCs.** The start screen, the lobby and the net mode are written and are
exercised by loopback tests (two or four copies of the port on one PC), but no match has been played yet between two
machines, on a LAN or over the internet. Read the section as what the program is built to do; a report of anything
that differs is useful.

Online play is for players, not developers: up to four people, each on their own PC, playing the same fight. All the
copies run the same frames and apply each player's buttons at the same moment (lockstep); only pad input crosses the
network.

### What everyone needs

- **The same build of the port.** The build id is printed when the program starts and is in `BUILT_FROM.txt`; both
  PCs must use the same commit (with the launcher, `tools/launcher/launch.sh --list` and the `commit <hash>` line it
  prints say which one). Build the Linux player folder (`tools/pipeline/package_linux.py`) from the same commit on
  both PCs.
- **The same game variant**: both the Japanese game, or both English with the same voices. Different game files are
  rejected.
- **A processor with AVX2** (Intel 2013 or later, AMD Ryzen). One without it cannot run the port at all: it shows a
  message instead of starting ("This game needs a processor with AVX2 ...").
- A gamepad for every player but player 1: the keyboard is always player 1 (the host). See "Keys".

### Hosting

Start the game as usual (launcher, AppImage or the Windows package) and, on the small start screen, choose
**Host an online game**. The program listens on the **Host port** set in **Settings** (default 7877; forward a
different one yourself) and asks the router to open it (UPnP). The lobby then shows the players connected so
far (name, ping) and either

- **Players can join at `<address>:7877`** - the external address to give the others (an IP, or your domain); or
- **the router did not open the port (`<why>`): forward UDP 7877 to `<local IP>` once on the router** - the router
  refused; forward it by hand (below). A failed request never stops the host: on the same network, or with a port
  already forwarded, it is not needed.

Press **Start** once two or more players are in the lobby; with all four slots filled it starts by itself after
3 seconds. Esc goes back one step, and a host leaving the lobby sends everyone back. After Start the game proper
begins on every screen, and the session runs until something in "What ends a session" happens.

**If the router did not open the port**, forward **UDP 7877** to the local IP the lobby printed, once. On a typical
router: open the router's own page in a browser (usually `192.168.0.1` or `192.168.1.1`; the address, the user and
the password are on the router or in its manual), find the section called "Port forwarding" (or "Virtual server",
"Applications", "NAT"), add a rule: protocol **UDP**, external port **7877**, internal port **7877**, internal
(local) IP the one shown. Change nothing else. Give the others the external address the lobby shows; if it changes
when the router reconnects, a name (a dynamic-DNS service) is easier for them to type.

**Windows firewall:** the first time a Windows host starts, Windows asks whether to let the program through the
firewall ("Windows Defender Firewall has blocked some features of this app"): tick **Private networks** and choose
**Allow access**. Without it nobody can join.

### Joining

On the start screen choose **Join an online game** and type the host's address: its public IP (`203.0.113.7`) or its
domain (`example.dyndns.org`), with `:port` only if the host uses another port (default 7877). Nothing is installed or
configured on your side: the port only sends outgoing UDP, which every router allows. The screen shows "Connecting to
`<address>`..." for up to 10 seconds, then the lobby ("Connected to `<host name>`. Waiting for the host to start.");
the host presses Start.

The name and the last address are remembered for the next time (`net.ini` in the user folder). With only a
gamepad, the address field offers the last one to accept, or Back.

### What the messages mean

Host:

| Message | What to do |
|---|---|
| the port is in use | another copy of the program (or another program) already uses UDP 7877: close it, or host on another port (`--net-host <port>`) and tell the others to type `address:<port>` |
| no network interface with IPv4 | the PC has no usable network (cable or Wi-Fi off, or IPv6 only): connect it and try again |
| the router did not open the port (...) | UPnP is off or unsupported: forward UDP 7877 by hand, above |
| the router maps the port to another PC | a forwarding rule for 7877 already points at another machine: change or remove it |
| a client rejected for another version / other game data | that PC has another build or another game variant: make both the same |
| two players with the same name | the second is listed as "Name (2)"; one of them changes it on the start screen (Name) |

Client:

| Message | What to do |
|---|---|
| Cannot reach `<address>`: is the host's port open? | nothing answered in 10 s: check the address for typos, that the host is in its lobby, and the host's port forwarding |
| The host runs another version: `<id>` | your build differs from the host's: use the same commit |
| The host uses other game data (another language variant) | you and the host do not have the same game files: use the same variant |
| The game is full | the host's players (2, 3 or 4) are already in: try a new lobby or a free slot |
| The game has already started | the host pressed Start before you joined: a started session cannot be joined |
| the host left, or cancelled the lobby | the session is over: host again, or join a new one |
| a malformed address | type an IP or a domain, with `:port` only if it is not 7877 |

### Input delay

Because every copy applies the same buttons on the same frame, your button shows on screen a few frames later, the
same for everyone (that is what keeps the fight in step however far apart the players are). The host measures the
ping to each client at the start and picks the delay automatically (about one frame per 15 ms of the worst round
trip, from 1 to 10). The host can force it with `--net-delay <1..10>`: a higher value is smoother over a long
distance but responds later. The chosen delay and every player's ping are printed in the log (`[don-net] delay 2
frames (round trips: p1 3 ms, p2 21 ms)`).

### What ends a session

- a **desync**: the copies' game memory no longer matches ("The game went out of sync at frame N"); everyone is sent
  out at once, and it is not caused by playing badly;
- a **player gone for 30 seconds** (connection lost, program closed): "Player 3 (Ana) left the game";
- the **host quitting**, or closing the window.

Any of them ends the session for all four and closes the game (it does not return to the fight): host a new lobby to
play again. **Nothing is saved online**: a session uses a temporary memory card that is thrown away when it ends,
whatever `--mc-dir` says, so SAVE from the game's menu keeps nothing; saving is for play on your own PC.

### By hand (advanced)

The start screen is skipped as soon as a net option is given on the command line (that is how the tests drive it):

```
don --net-host 7877 --net-players 4 --net-delay 3 --net-name Ana     # host
don --net-join 203.0.113.7:7877                                      # client
```

- `--net-host PORT`: host, listen on that UDP port (1..65535; 7877 is the usual one).
- `--net-join HOST[:PORT]`: client, the host's IP or domain (port 7877 if not given).
- `--net-players N`: host only, 2..4 (how many the lobby waits for).
- `--net-delay N`: host only, 1..10; not given = automatic from the measured ping.
- `--net-name NAME`: at most 15 characters, shown in the others' lobby list.
- `--mute`: the sound processor is emulated but no audio device is opened (net play needs the sound processor).
- `--sound off` and `--players` cannot be combined with either net option: the port exits with an error.

The launcher passes them: `launch.sh en-jpvoice --host [--port P] [--players N] [--delay D]` and
`launch.sh en-jpvoice --join HOST[:PORT]`. With `--host` the launcher also prints the local addresses of this PC, for
a session on the same network. The `[don-net]` lines in the log (`don.log` in the user folder) are the diagnostics of all of
this.
