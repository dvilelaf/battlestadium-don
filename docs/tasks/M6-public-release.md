# M6 · Public release: the player brings the disc, their PC prepares the game

The owner's plan (Spanish, in OpenChamber) is the source; it was reviewed by a second Opus in two rounds
(`work/plan-review/REVIEW.md`). This file holds what an agent needs: the design reference in English (section A)
and one card per piece (section C). **Scope now: OS-agnostic design, implemented and tested on Linux only.**
Windows and packages come later and have no cards here.

**V1 freeze (owner, 2026-10-26, after an independent review).** The first public release on Linux needs only:
(1) the tracked module recipe (done: T-614, `tools/module/make_ninja.py`); (2) the real preparation, end to end, as
three cards: T-690 build half (generated tree → compiled, linked, manifested module, compiles in parallel),
T-691 front half (ISO → checked files → generated tree with the reference hash), T-692 `don --prepare <iso>` and
the auto-load of the prepared module; (3) T-649 wiring the setup screens to that flow; (4) a Linux package that
carries `don`, the PCSX2 pieces, `ps2_recomp`, clang/lld, the module headers, configs, licences and the GPL source
offer (new card, after T-692); (5) T-651 once on the owner's PC with a real GPU (also settles the five screenshot
md5s); (6) legal checks: personal patterns file, `public_check.py`, LLVM NOTICE. Everything else in section C
(T-615, T-636a's object cache, T-637, T-638a/b, T-650, T-652/653, T-661, T-663, T-673, the 5+5 perf series) waits
until after 1.0. Reviews by the Sonnet reviewer only for critical-path cards.

## Critical path
1. [t-614] Tracked module recipe, generator emits against the module ABI (1-3 h)
2. [t-691] ISO to checked game files to generated tree with the reference hash (2-3 h)
3. [t-690] Generated tree to compiled, linked, manifested module, in parallel (2-4 h)
4. [t-692] `don --prepare <iso>` and auto-load of the prepared module, end-to-end route in a clean user folder (3-5 h)
5. [t-693] English variants (the owner plays in English): allow-lists and references for `en-jpvoice`/`en-envoice`, prepare from a patched image or folder (1-3 h)
6. [t-649] Setup screens wired to the real flow, S1 to S3 to play (2-4 h)
7. [t-696, t-698, t-697b] Linux package: don, PCSX2 pieces, ps2_recomp, clang/lld, module headers, configs, licences, GPL source (2-4 h)
8. [t-701, t-702, t-704, t-705, t-707, t-708, t-709, t-712, t-713b] Done: fixes after the owner rejected the first package; a fast check of the real program (27 paths, ASan build, asserts error codes). It does NOT catch the 899dddd crash (journal 046): the gate for that crash is the ASan build of `tests/setup/test_setup_browser_crash.cpp` ; about 10 h spent (1-2 h)
9. [t-714, t-715, t-716, t-717, t-718, t-719, t-720, t-721, t-722] Package the owner can try: archive from main proven headless (t-722, running), the orchestrator plays it on the desktop (prepare, PLAY, a fight), then one round of fixes for what that play shows and a new package, installed to the dock; each desktop play so far brought one round of fixes and a new package (4-7 h)
10. Legal checks: personal patterns (done), public_check, LLVM notices (in t-696), no machine path in `.orchestration/config.json`, `<source-url>` in SOURCE.txt (0.5-1 h)
11. Owner's PC, like a player, real GPU (0.5-1.5 h)

Goal: publish with zero game-derived material. On first run the program finds or asks for the player's own disc
image, checks it, copies the needed files, runs `ps2_recomp` on the player's PC and compiles the generated C++ into
a **game module** (`libdon_game.so`) with a pinned clang/lld, then plays. The player always knows what to do, what
to bring and how long it takes; every failure says what happened, why and what to do.

---

## B · Rules for every card

**Token economy (owner's rule, 2026-10-22):**

1. **One new agent per card.** The prompt is short: the card ID, this file's path, the clock deadline. Never reuse
   an agent session for another card. A session is resumed only for one short follow-up right after its own report.
2. **Read only what the card lists**, with line ranges (`sed -n 'A,Bp'`, `grep -n`). Never `cat` a file of
   `generated/` (412 MB) or a whole log. Long output goes to a file, and only `tail -n 40` or a `grep` of it is read.
3. **Report in at most 25 lines** (AGENTS.md format). Details go to `work/<card>/NOTES.md`.
4. **Haiku** for mechanical work, runs, measurements and docs; **Sonnet** for code and reviews. Never Opus as a
   subagent (too expensive).
5. **No duplicated checks.** The orchestrator re-runs only the card's "Done when" command. The full regression
   suite runs once per integration batch, by the orchestrator, never by a card.
6. **Check the premise cheaply first.** If a card's first step shows a fact in the card is false, stop and report
   (AGENTS.md rule 3).

**Execution:**
- **Worktree** (AGENTS.md "Isolation"): `git worktree add work/wt/<card> -b agent/<card> main`, then symlink
  `third_party` and `generated` to the main checkout's. Chains share one worktree:
  - `work/wt/gate`: T-600…T-611, T-614, T-615, T-616;
  - `work/wt/recomp`: T-612, T-613, T-672;
  - `work/wt/setup-screen`: T-624…T-628, T-644, T-673.

  Commit on the branch, `git merge main` before reporting, and never touch `main`. `work/<card>/` notes stay in the
  main checkout.
- **Never the owner's user folder:** every game run sets `DON_USER_DIR=/tmp/<card>-user` (T-651 uses a fresh one too).
- Write `HH:MM | done | doing | on track?` to `work/<card>/PROGRESS.md` every 10-15 min. Stop at the deadline with a
  report.
- Never end a turn with something still running. Kill only by pid.
- Heavy builds and game runs under `flock /tmp/don-build.lock`, one at a time, with `DON_JOBS=4`. clang-17 exists only
  in the build image: `tools/container/run.sh <command>`.
- Sound only through `SDL_AUDIO_DRIVER=disk` with `SDL_AUDIO_DISK_OUTPUT_FILE` in `/tmp`. Headless unless the card
  says otherwise.
- **`generated/` is frozen until Gate 2** (T-611). Nobody regenerates it; recompiler work writes to `work/<card>/`.
- A card touches only the files it **Owns**. Unit tests of `src/setup/` live in `tests/setup/` (one program per
  `test_setup_*.cpp`, each card in its own build folder `build/setup-tests-<card>`):
  `cmake -S tests/setup -B build/setup-tests-<card> && cmake --build build/setup-tests-<card> -j4 && ctest --test-dir build/setup-tests-<card> --output-on-failure`.
- One regression route: `uv run --no-project --with pytest --with numpy python -I -m pytest tests/regression -m slow -q -k <route>`
  (`DON_BINARY` picks the binary; after T-610, `DON_GAME_MODULE` picks a module).
- Performance: `uv run --no-project python -I tools/run/perf_fight.py --binary <don> --json`. Read what its exit code
  means at `tools/run/perf_fight.py:222`. Never put it in the same command line as a build: it waits while any
  `build.sh` or `ninja` runs.
- **Baselines** (state hashes, `perf_fight` medians) come from T-671's `work/baseline/`. "Equal state hash" always
  means equal to that file.
- **PS2Recomp patches:** never run `tools/setup/fetch_ps2recomp.sh --reset` (it wipes other cards' work). To check
  that a new patch applies from clean:
  - make a scratch tree:
    `git -C third_party/PS2Recomp worktree add /tmp/p2r-<card> $(grep -oP '^PIN=\K\w+' tools/setup/fetch_ps2recomp.sh)`;
  - apply in order:
    `for p in $(cat patches/ps2recomp/series) <new>.patch; do git -C /tmp/p2r-<card> apply "$PWD/patches/ps2recomp/$p" || break; done`;
  - remove it: `git -C third_party/PS2Recomp worktree remove --force /tmp/p2r-<card>`.
- **Runtime files** are under `third_party/PS2Recomp/ps2xRuntime/{include,src}`; find one with
  `git -C third_party/PS2Recomp ls-files | grep <name>`.
- **Shared plumbing** (`CMakeLists.txt`, `src/host/CMakeLists.txt`, `src/host/options.*`, `tests/setup/CMakeLists.txt`,
  `tests/regression/harness.py`) is owned by T-670 and T-610 only. Other cards ask for a flag in "Notes" instead of
  editing those files.

---

## A · Design reference (English)

### A1. Architecture

- **The game module.** The code generated from the game is compiled *freestanding* against our header
  `src/game_module/don_guest_abi.h` into `libdon_game.so`. The module:
  - exports one symbol, `don_game_module`, which returns a `const DonGameModule*` with the ABI version, the module
    id, and the function table as a const `{uint32_t addr; fn}` array (no static constructors);
  - makes direct internal calls (no PLT/GOT between generated functions), has no TLS, and imports at most
    `memcpy`/`memset`/`memmove`;
  - is compiled with `-nostdinc -isystem <clang resource dir>/include -ffreestanding -fno-rtti
    -fno-threadsafe-statics -fvisibility=hidden -fno-stack-protector -ffp-contract=off -march=x86-64-v3
    -ffile-prefix-map=<root>=. -fno-crash-diagnostics`, with no `-g`. The level is `-O1`, the one the player build
    uses today (`CMakePresets.json:51`).
- **Exceptions.** The runtime throws `EeDispatcherTransfer` (`ee_scheduler.h:19-23`, thrown in `EeScheduler.cpp`
  around lines 730, 1028, 1331, 1350 and 1913, caught at 251, 283 and 381) and `std::runtime_error`
  (`ps2_memory.cpp`) through generated frames. Gate 1 picks one path:
  - (a) unwind tables in the module (`-funwind-tables`, no catch inside);
  - (b) replacing those throws with the "unwinding" flag of patch 0054 (`ps2_dispatch_inline.h:50-53`).
- **Hot state.** `dispatchGuestBranch` (61,396 call sites) and `eeCheckpointDue` (12,879) read private scheduler
  state (`ee_scheduler.h:570-580`). That state moves to a standard-layout `DonHotState` owned by the runtime
  (a PS2Recomp patch).
- **Overrides** (`src/game/don_overrides.cpp`) resolve the original functions by address, not by generated name, and
  `don` builds without `generated/`.
- **IOP native (`cri_adxi`)**: the first module ships without it, using the interpreter fallback. T-648 runs only if
  Gate 2's budget is exceeded.
- **Preparation pipeline on the player's PC** (no Python, no internet):
  1. extract the allow-listed files (263 MB; never `DUMMY.DAT`);
  2. run `ps2_recomp` with the frozen `config/recomp.toml`;
  3. compare the generated tree's hash with the reference **before** compiling;
  4. compile stable batches (by address range) with the pinned clang into a content-addressed object cache;
  5. link with lld into `module/<id>.tmp/`;
  6. verify the hash, rename, and keep the previous module.
- **Toolchain:** clang/lld 17.0.6, pinned and self-contained, recorded by SHA-256.
- **Module id** = hash of the ELF, the IRX, the config, the recompiler version, the ABI version, the toolchain hash
  and the flags.
- **Protecting the PC:**
  - jobs = min(cores − 1, a RAM model built from the measured per-batch peak);
  - `MemAvailable` and PSI watched twice a second; under pressure no new job starts and the newest is requeued;
  - children get `oom_score_adj=1000`, `PR_SET_PDEATHSIG` and their own process group;
  - nice 19 in GENTLE, 0 in FAST;
  - `TMPDIR` inside `build/`, a timeout of 10 × the expected time per batch, and sleep inhibited while preparing.
- **Estimate:**
  - per-batch time and peak RSS are measured at release and kept in `config/module_build.toml`;
  - before starting, a longest-first schedule is simulated for this PC's job count;
  - a ~3 s calibration compiles our own synthetic unit;
  - while running, the observed rate is used; the time left never rises more than 20 % unless the stage changes.
- **User folder:** the API returns two roots, small data (memory cards, ini, log) and bulk. They are the same folder
  on Linux.
  - `setup.json` is an atomic journal: write, fsync, rename.
  - Bulk holds `game/<variant>/`, `cache/obj/`, `module/<id>/manifest.json` and `build/tmp/`.
  - `memcards/` is never touched by any cleanup.
- **Startup:** single instance (`flock` on `setup.lock`); the module manifest and files are checked; recovery never
  deletes user data.
- **Netplay:** the handshake carries the module id and whether it is verified.
- **COPY REPORT:** versions, ids, the ErrorId, OS, CPU, RAM, free space, the toolchain hash and the last 50 log
  lines. `$HOME` becomes `~` and no generated source is included.

### A2. UX rules

1. Exactly one primary action, gold and preselected.
2. Every wait shows the time left; an estimate is shown before starting.
3. What to bring is named exactly.
4. Never a frozen screen.
5. Every error shows a short code, what happened, why, what to do, and a primary action that fits that error.
6. Stop only when a decision is needed.
7. Accessibility: never colour alone, a UI scale setting, a visible focus ring, reduce-motion, gamepad glyphs, and
   UTF-8 is sanitised.

The look is the existing start screen's (`docs/knowledge/22-netplay.md` 7.3, `src/host/ui/`). Texts are English only.

### A3. Screens and exact copy

| State | Content | Primary / others |
|---|---|---|
| S0 Discovery (≤ 2 s, first run) | Scans `~/Downloads`, Desktop, Documents, `~/Games`, `~/Emulation/roms/ps2`, PCSX2's game list folders, `/media/$USER`, `/run/media/$USER`. Depth ≤ 2; no symlinks out of those folders; no network filesystems; stops after 2 s. For a raw ISO it reads only the volume descriptor, the root directory and `SYSTEM.CNF`, and keeps the file if `BOOT2` names `SLPS_256.75`. Compressed files are listed as "possible". | — |
| S1 Welcome | Title "Set up Battle Stadium D.O.N" and the steps rail `1 Your disc · 2 Prepare · 3 Play`. Body: "This program doesn't include the game. It plays your own copy of Battle Stadium D.O.N for PlayStation 2 (Japan, SLPS-25675). Choose a disc image of it: one file, usually .iso, made from your disc." Then "Disc images with Lord Izen's English fan translation applied are supported too." and the link "How do I make one from my disc?". Panel "What happens next": Check your disc — a few seconds / Prepare the game for this PC — about N min, once / Play. Status: "Needs X GB free on <mount> · you have Y GB ✓", "Everything stays on this PC. Nothing is uploaded.", "We looked in your Downloads, Documents and Games folders." | **USE THIS DISC IMAGE: <name>** (when S0 found one), else **CHOOSE DISC IMAGE…** (native dialog, falling back to BROWSE). Others: BROWSE (in-app, gamepad), READ FROM DISC DRIVE (only when a disc is present), QUIT. Hint: "or drop the file (or a folder with the disc's files) on this window" |
| S2 Checking | "Checking your disc…", a bar, "a few seconds", and the file name muted. Every needed file is checked against the per-file manifest | — |
| Decision (only if needed) | E17 unknown data, low space, battery, low RAM | per case |
| S3 Preparing | Header "✓ <variant description> · checked" with secondary WRONG DISC? CHOOSE ANOTHER. Big percentage, bar, "About N min left". Steps panel: Copy game files · Translate the game's code · Build for this PC 812/1,876 · Final check. Footer: "Your PC may feel slower until this is done. You can close this window: we'll continue from here next time. We'll play a sound when it's ready." Window title "42 % · Preparing — Battle Stadium D.O.N" | **SPEED: FAST ◂▸ GENTLE** / STOP — CONTINUE LATER |
| Close during S3 | "Stop preparing? Your progress is saved." | **KEEP PREPARING** / STOP AND CLOSE |
| Resume (next start) | "Preparing was stopped at 42 %." | **CONTINUE PREPARING · about N min left** / START OVER WITH ANOTHER DISC |
| Done | A chime and `SDL_FlashWindow`. The start screen opens with PLAY preselected and a one-time banner: "Your game is ready. You can move or delete the disc image now — the game doesn't need it any more." First-time controls card for keyboard and gamepad | **PLAY** |
| S5 Error (panel) | Code (e.g. `E-SPACE-2`), title, why, "What you can do: …" | depends on the error: CHOOSE ANOTHER DISC / CONTINUE / PREPARE AGAIN / TRY AGAIN; always COPY REPORT |
| S6 Settings rows | GAME VERSION ◂▸ (installed variants; switching is instant) · ADD ANOTHER DISC… ("a few seconds — no need to prepare again") · STORAGE (sizes; FREE UP SPACE; REMOVE GAME DATA, which never touches memory cards) · REPAIR | — |
| S7 Update | "Updating your game for version v · about N min. You can play once this finishes." Same view as S3; the old module is kept until the new one is checked | — |

Honest phrases: "about N min. You do this once; some updates repeat a shorter version." · "Checked: every file
matches the Japanese release." · "Checked: identical to the official build. Online play works with anyone on version
vX using the same disc version." The space shown is the measured peak, never a round guess.

### A4. Error catalogue (IDs fixed; final wording in T-633)

**Choosing the file**

| ID | Error | What the player is told or what happens |
|---|---|---|
| E01 | Picker cancelled | Nothing; stays on S1 |
| E02 | No system file dialog | The in-app browser opens |
| E03 | A folder is dropped | Accepted if it has `SYSTEM.CNF` and `SLPS_256.75`; else "This folder doesn't contain the game's files." |
| E04 | Several files dropped | The first one that identifies is used |
| E05 | No read permission | "You don't have permission to read it. Copy it to your Downloads folder." |
| E06 | File gone or USB removed | "<name> isn't there any more. Was a USB drive removed?" / TRY AGAIN |
| E07 | Empty or tiny file | "This file is empty or too small to be a disc image." |
| E08 | Truncated or still downloading | "This disc image is incomplete: 1.1 GB of 1.77 GB. It may still be downloading." |
| E09 | `.crdownload`, `.part`, `.!qB` (a hint only) | "This file is still downloading." |
| E10 | CHD, CSO/ZSO, 7z, RAR, ECM, NRG, MDF or a `.cue`, by magic bytes | How to convert it: `chdman extractdvd`, or extract the .iso with 7-Zip |
| E11 | Zip with no ISO, several ISOs, a password, an unsupported method, or split | One sentence each |
| E12 | A patch file (xdelta, PPF, IPS, BPS) | "This is a patch file, not a disc image." |
| E13 | Another PS2 game | "This is <serial>, not Battle Stadium D.O.N (SLPS-25675)." |
| E14 | The GameCube version | "This is the GameCube version. Only the PlayStation 2 version works here." |
| E15 | Not a disc image | "This file isn't a disc image." |
| E16 | Different ELF | "…its program file differs from the Japanese release (SLPS-25675). Was it patched?" |
| E17 | Known ELF, unknown `DATA.AFS` | "…game data we don't recognise. It will probably work. Online play only works with players who have the same files." / **USE IT ANYWAY** (variant `custom-<sha8>`) |
| E18 | A damaged file in a known variant | "Some files on this image are damaged (BGM.AFS)." |
| E19 | Read error part-way | "Reading stopped at 61 % (input/output error)." / resume per file |
| E20 | Slow or network media | The estimate adapts |
| E21 | gz or zip input | Read in one pass |
| E22 | Long or non-ASCII path | Never fails |

**Space, memory and the machine**

| ID | Error | What the player is told or what happens |
|---|---|---|
| E30 | Not enough space at the start | CONTINUE enables itself when there is room |
| E31 | Disk full part-way | Automatic pause; "Your disk is full. Free up X GB. Nothing was lost." |
| E32 | User folder not writable, or `HOME` unset | Says so |
| E33 | Home mounted `noexec` | Says so |
| E34 | User folder on NFS or SMB | Warning |
| E35 | Small `TMPDIR` | Our own `TMPDIR` is used |
| E36 | Low total RAM | The long time is said before starting |
| E37 | Memory pressure | "Waiting for free memory…" and the newest job is requeued |
| E38 | A compiler is OOM-killed | Retried alone; after the second time, explained |
| E39 | On battery | PREPARE ANYWAY |
| E40 | Suspend | Inhibited while preparing |
| E41 | Clock jump | `steady_clock` everywhere |
| E42 | No AVX2 | The existing message |

**Translate and build**

| ID | Error | What the player is told or what happens |
|---|---|---|
| E50 | Bundled tools missing or damaged (hash check) | Says so |
| E51 | `ps2_recomp` fails | Says so |
| E52 | Generated tree hash differs | Retry once, then a faulty-memory hint |
| E53 | Compile error | Retry the batch alone |
| E54 | Compiler crash | Retry the batch alone |
| E55 | Compiler hang | Retry the batch alone |
| E56 | Link failure | Retry with nothing else running |
| E57 | Module hash differs from the reference | Local play allowed, online play off for this copy / PREPARE AGAIN |
| E58 | Module won't load | Automatic S7 |
| E59 | Crash right after a new module loads | PREPARE AGAIN |

**Lifecycle**

| ID | Error | What happens |
|---|---|---|
| E70 | Cancel | Progress is kept |
| E71 | Window closed during preparing | The close confirmation |
| E72 | Killed, crashed or power cut | Atomic journal; `.part` files are removed and each artifact is validated on resume |
| E73 | Orphan compilers | Killed at start |
| E74 | Two instances | Single-instance message |
| E75 | Corrupt `setup.json` | Rebuilt from the manifests; nothing is deleted |
| E76 | Part of the user folder deleted | Only the missing part is redone |
| E77 | Files changed or quarantined | REPAIR |
| E78 | The ISO is deleted after setup | Not needed, and the player is told so |
| E79 | A new module id after an update | S7 |
| E80 | Several program versions | Modules used in the last 30 days are kept, at least two |
| E81 | Netplay peer with another module | REJECT with a clear text |
| E82 | Report leakage | Redacted |
| E83 | Memory cards | Never touched |

`DON_TEST_FAULT` hooks (off by default, zero cost): `enospc@copy:50%`, `eio@read:20%`, `kill@build:30%`,
`sigkill@unit:17`, `hashmismatch@verify`, `oom@unit:5`, `noportal`.

---

## C · Cards

**Waves (at most 4 agents at a time):**

| Wave | Cards |
|---|---|
| 1 | T-671 (baseline, first: uses the build lock), T-600, T-620, T-662. Until T-620 lands, `tests/setup` has 2 known failing checks, so no other card that runs its `ctest` starts before it |
| As slots free | T-670, T-618, T-612, T-617, T-601 → T-602 → T-603 (Gate 1); T-621, T-622, T-629, T-631, T-664, T-660 |
| After Gate 1 | T-604 → T-605 → T-606 → T-607 → T-608 → T-609 → T-610 → T-611 (Gate 2) |
| After Gate 2 | T-613 (after T-606), T-672, T-614 → T-615; T-616 (after T-610); the rest by dependency |

**Critical path:** T-600 → T-601 → T-602 → T-603 → T-604 → T-605 → T-606 → T-607 → T-608 → T-609 → T-610 →
T-611 → T-614 → T-615 → T-635 → T-636a → T-636b → T-637 → T-638a → T-649 → T-650 → T-651 → T-653.

**Same-file chains** (never in parallel):
- `setup_screen.cpp`: T-625 → T-626 → T-628 → T-644 → T-673;
- `start_screen.cpp`: T-626 → T-643 → T-650;
- recompiler tree: T-606 → T-613 → T-614 → T-615.

#### T-671 · Baselines before any change
**Model:** Haiku · **Depends on:** — · **Time box:** 60 min · **Owns:** `work/baseline/`
**Steps**
1. `git stash list` must stay untouched. Note `git rev-parse HEAD` (expected `63fc1b6`).
2. Build linux-dev under the lock: `flock /tmp/don-build.lock env DON_JOBS=4 bash tools/pipeline/build.sh linux-dev`.
   Then copy `build/linux-dev/don` to `work/baseline/don` and record its sha256.
3. Run `perf_fight` 5 times with `--binary work/baseline/don --json`, saving each JSON. Write the median and spread
   of the game-thread ms per blank, and the state hash, to `work/baseline/perf.md`.
4. Run the routes `fight_practice` and `save_and_load` with `DON_BINARY=work/baseline/don` and `DON_TRACE_STATE=240`.
   Save the `[don] state` lines of each to `work/baseline/<route>.state`.
5. A second build with `-DDON_IOP_NATIVE=OFF` into `build/baseline-noiop` (configure flags copied from
   `CMakePresets.json` linux-dev), then 5 `perf_fight` runs. Results go to `work/baseline/perf-noiop.md`.

**Done when:** `work/baseline/` has the binary hash, both perf files and both state files.
**Limits:** no source change.

#### T-670 · Build plumbing and options
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `CMakeLists.txt`, `src/host/CMakeLists.txt`,
`src/host/options.{h,cpp}`, `tests/setup/CMakeLists.txt`
**Steps**
1. `DON_GENERATED_DIR` (a plain `set()` at `CMakeLists.txt:14`) becomes a `CACHE PATH` with the same default. Add the
   option `DON_LINK_GENERATED` (default ON).
2. Add a static library `don_setup` with every `src/setup/*.cpp`, `src/setup/platform/*_posix.cpp` and
   `*_linux.cpp`. Link it into `don`.
3. `tests/setup/CMakeLists.txt` also compiles `src/setup/platform/*_posix.cpp` and `*_linux.cpp`.
4. Parse these options in `options.cpp` (stored, not acted on): `--game-module <path>`, `--prepare <iso>`,
   `--until <stage>`, `--setup-demo`, `--screenshot-dir <dir>`, `--setup-auto-pick <iso>`.

**Done when:** linux-dev builds; `don --help` lists the new options; `ctest` in `build/setup-tests-t670` passes.

### Gate track

#### T-600 · Dependency inventory of the generated code
**Model:** Haiku · **Depends on:** — · **Time box:** 45 min · **Owns:** `work/t600/`
**Read first:** A1; `work/p01a/NOTES.md` (earlier partial inventory); the include list at the top of any
`generated/FUN_*.cpp` (`head -20`).
**Steps**
1. For each **runtime** header that generated files include, list the macros, inline functions and types it declares
   (`grep -n`):
   - `third_party/PS2Recomp/ps2xRuntime/include/ps2_runtime.h`;
   - `ps2_runtime_macros.h`, `ps2_syscalls.h`, `ps2_stubs.h`, `ps2_log.h` (same folder).

   `ps2_recompiled_functions.h` and `ps2_recompiled_stubs.h` are **in `generated/`** (4.9 MB, about 191k lines):
   never `grep -n` or open them.
2. Look up how often each of those names appears in `generated/`, in `work/p01a/idents.txt` (1.9M lines: never
   print it). Use `grep -wE '(NAME1|NAME2|…)$' work/p01a/idents.txt`, or `grep -w NAME`, after checking its format
   with `head -3`.
3. Also count `runtime->` member calls (`grep -rhoE 'runtime->[A-Za-z_]+' generated | sort | uniq -c`), `ctx->`
   fields, `throw`, `try`, `std::`, `_mm_`/`__m128` intrinsics, `thread_local` and `static` locals. Pipe each result
   through `sort | uniq -c | sort -rn | head -60` into a file.
4. Write `work/t600/deps.md`: one table per category (macro, member call, context field, intrinsic, STL, exception),
   name and count, sorted by count.

**Done when:** `work/t600/deps.md` exists, with the commands used at its end.
**Limits:** counting only; no code.

#### T-601 · Draft `don_guest_abi.h` and shim headers
**Model:** Sonnet · **Depends on:** T-600 · **Time box:** 60 min · **Owns:** `work/t601/`
**Read first:** A1; `work/t600/deps.md`; `ps2_runtime.h:67` onwards (`R5900Context`); `ps2_runtime_macros.h`; the
lines of `ee_scheduler.h` that `deps.md` points to.
**Steps**
1. Write `work/t601/don_guest_abi.h`. Include only `<stdint.h>`, `<stddef.h>` and `<immintrin.h>` from clang's
   resource directory. Declare:
   - `R5900Context`, copied field for field;
   - `DonHotState`, with the fields that `dispatchGuestBranch` and `eeCheckpointDue` read;
   - `DonRuntimeCalls`, a table of C function pointers, one per runtime member call in `deps.md`;
   - the inline memory accessors on `rdram` and `DonHotState`.
2. Write `work/t601/layout_check.cpp`, compiled against the real runtime headers. It `static_assert`s every field
   offset and size of the copied structs.
3. Write shim headers in `work/t601/shim/` with today's names (`ps2_runtime.h` and the others). They map today's
   macros and `runtime->X(...)` onto the ABI header. `PS2Runtime` becomes a small shim struct whose methods call
   through `DonRuntimeCalls`.

**Done when:** `tools/container/run.sh clang++ -std=c++20 -fsyntax-only work/t601/layout_check.cpp <the runtime's
include flags>` passes.
**Limits:** no edits outside `work/t601/`.

#### T-602 · Freestanding compile of a coverage sample
**Model:** Sonnet · **Depends on:** T-601 · **Time box:** 60 min · **Owns:** `work/t602/`
**Steps**
1. Pick 500 generated files that together use every name in `deps.md`, plus 2 big files from
   `generated/sources_big.txt`. Write the list to `work/t602/sample.txt`.
2. Compile each one in the container with the A1 flags, `-I work/t601/shim`, `-MD`, `-c`, and one with
   `-ftime-trace`. Run it under the lock with at most 4 jobs.
3. From the `.d` files, list every header that comes from outside `work/t601` or clang's resource directory. The list
   must be empty.
4. Compile the same 500 the normal way (today's flags and headers) and compare total wall time.

**Done when:** `work/t602/report.md` gives the number compiled and failed (with distinct errors and counts), the list
from step 3 (must be empty), and the two times.
**Limits:** do not touch `generated/`.

#### T-603 · Gate 1 probes
**Model:** Sonnet · **Depends on:** T-602 · **Time box:** 60 min · **Owns:** `work/t603/`
**Steps**
1. **Exceptions.** Build a tiny `.so` with the A1 flags plus `-funwind-tables`. It has one function that calls back
   into the host; the host throws through it and catches the exception. Then repeat with option (b) of A1, a flag
   checked after the call.
2. **Checks on the sample objects** from T-602:
   - `readelf -S` shows no `.init_array`;
   - `readelf -s` shows no TLS symbol;
   - `nm -u` minus the names of generated functions lists nothing but `memcpy`, `memset` and `memmove`. 448
     generated files call other generated functions directly, so those names always appear on single objects.
3. **Conformance.** Run the FPU, MMI and VU0 conformance families (`tests/conformance/`; `driver.cpp` is built at
   `tests/conformance/harness.py:86-124`) with the driver compiled against the shim headers. The output must match the
   normal build bit for bit.
4. **Gate 1 report**, criterion by criterion, PASS or FAIL with evidence:
   - dependencies classified;
   - header written;
   - `DonHotState` designed;
   - exception path chosen (a or b);
   - clean `.d` files;
   - no static constructors and no TLS;
   - allowed undefined symbols only;
   - conformance bit-exact.

**Done when:** `work/t603/gate1.md` has every criterion with its evidence.
**Limits:** if a criterion fails, report it. The owner's rule is **stop and rethink**, not work around it.

#### T-604 · Whole module with shims
**Model:** Sonnet · **Depends on:** Gate 1 PASS · **Time box:** 90 min (heavy build) · **Owns:** `work/t604/`
**Steps**
1. Compile all of `generated/` in the container with the T-602 flags, using a small ninja file in `work/t604/` and
   unity batches of 32 as today. Run it under the lock with `DON_JOBS=4`.
2. Link `work/t604/libdon_game.so` with `ld.lld -shared --hash-style=gnu -z now --no-undefined-version`, listing the
   objects in sorted order.
3. Check the result:
   - `nm -D --defined-only` shows only `don_game_module`;
   - `nm -D -u` shows only `memcpy`, `memset` and `memmove`;
   - `readelf -r libdon_game.so | grep -c JUMP_SLOT` is at most 3;
   - `readelf` shows no `.init_array` and no TLS.

**Done when:** `work/t604/report.md` gives the build time, the size, and the three checks with their command output.
**Limits:** do not touch `generated/`, `src/` or `third_party/`.

#### T-605 · PS2Recomp patch: `DonHotState`
**Model:** Sonnet · **Depends on:** T-604 · **Time box:** 90 min · **Owns:** files in
`third_party/PS2Recomp/ps2xRuntime` named in the patch, and a new `patches/ps2recomp/NNNN-don-hot-state.patch`
**Steps**
1. Move the fields of `ee_scheduler.h:570-580` that the inline dispatch path reads into a standard-layout
   `DonHotState` owned by `PS2Runtime`. `EeScheduler` keeps a pointer to it.
2. Make `dispatchGuestBranch` and `eeCheckpointDue` read only `DonHotState` (`ps2_dispatch_inline.h`).
3. Rebuild linux-dev (incremental, under the lock). Run the regression route `fight_practice` and one `perf_fight`;
   compare with `work/baseline/`.
4. Write the patch (AGENTS.md "How to create a patch") and check it with the scratch-worktree recipe of section B.
5. Copy `work/t601/` to `work/t605/abi/`, adjusting `DonHotState` to the patch. From now on, `work/t605/abi/` is the
   ABI draft.

**Done when:** the route passes, the state hash equals the baseline, and the patch applies from clean.
**Limits:** no other behaviour change; `series` is appended by the orchestrator.

#### T-606 · Function table as an exported const array
**Model:** Sonnet · **Depends on:** T-605, T-670 · **Time box:** 60 min · **Owns:**
`third_party/PS2Recomp/ps2xRecomp/src/lib/function_table_emitter.cpp`, the runtime table code
(`ps2xRuntime/src/.../ps2_runtime.cpp` around line 1311) and a new patch
**Steps**
1. The emitter writes a `static const` `{addr, fn}` array and a `don_game_module()` accessor instead of the static
   constructor that fills `g_ps2RecompiledFunctionTable`.
2. At start the runtime builds its writable table from that array. Overrides still write into the writable table.
3. Regenerate with the recompiler into `work/t606/gen` only. Build into `build/t606` with
   `-DDON_GENERATED_DIR=$PWD/work/t606/gen` (T-670), then run `fight_practice` with `DON_BINARY=build/t606/don`.

**Done when:** the route passes, the state hash equals the baseline, and the patch applies from clean.

#### T-607 · Exception path from Gate 1
**Model:** Sonnet · **Depends on:** T-606 · **Time box:** 60 min · **Owns:** the files named in `work/t603/gate1.md`
and a new patch
**Steps:** implement the path that Gate 1 chose:
- option (a), unwind tables: no runtime change, only the module flags (write them into `work/t605/abi/flags.txt`);
- option (b): replace the throws listed in A1 with the unwinding flag.

Then build as in T-606 and run `fight_practice` and `save_and_load`.
**Done when:** both routes pass with state hashes equal to the baseline.

#### T-608 · Overrides by address
**Model:** Sonnet · **Depends on:** T-607 · **Time box:** 60 min · **Owns:** `src/game/don_overrides.cpp`,
`config/native_overrides.txt`
**Steps**
1. Remove the include of the generated declarations header (line 22).
2. Replace the calls by generated name (lines 147, 190, 437) with a lookup by address through the runtime's table,
   resolved once at registration.
3. Build, run `fight_practice`, check the state hash.

**Done when:** `grep -nE 'FUN_00|entry_00' src/game/don_overrides.cpp` lists no call by name (`entry_00138ee0` too),
and the route passes with a state hash equal to the baseline.

#### T-609 · `don` builds without `generated/`
**Model:** Sonnet · **Depends on:** T-608, T-670 · **Time box:** 60 min · **Owns:** `src/host/iop_native.cpp`,
`tests/conformance/driver.cpp`
**Steps**
1. `iop_native.cpp:8` and the conformance driver stop including generated files. IOP native stays disabled until
   T-648.
2. Clone the repository into `work/t609/clone`, since the container mounts only the repository
   (`tools/container/run.sh:44`): `git clone -q . work/t609/clone`. Then link
   `ln -s "$PWD/third_party" work/t609/clone/third_party`, and leave it with no `generated/`.
3. Build it in the container with `-DDON_LINK_GENERATED=OFF`.

**Done when:** the clone builds `don`, and every unit test of `tests/setup`, `tests/net` and `tests/host` passes in it.

#### T-610 · POSIX module loader (prototype)
**Model:** Sonnet · **Depends on:** T-609 · **Time box:** 60 min · **Owns:** `src/host/game_module_posix.cpp`,
`src/host/game_module.h`, `tests/regression/harness.py` (the `DON_GAME_MODULE` pass-through only)
**Steps**
1. Write `load_game_module(path)`: `dlopen` with `RTLD_NOW|RTLD_LOCAL`, then `dlsym("don_game_module")`, check the ABI
   version, and register the function array with the runtime.
2. Act on `--game-module <path>` (parsed by T-670) when built with `DON_LINK_GENERATED=OFF`.
3. Make `harness.py` pass `--game-module $DON_GAME_MODULE` when that variable is set.
4. Build the module (the T-604 recipe with the patched tree) and run `fight_practice` with it.

**Done when:** the route passes with the module, with a state hash equal to the baseline.

#### T-611 · Gate 2 measurement
**Model:** Haiku · **Depends on:** T-610 · **Time box:** 60 min · **Owns:** `work/t611/`
**Steps:** run `perf_fight` 5 times with the module build (IOP native is off in it). Compare the median with
`work/baseline/perf.md` (linked, with IOP native) and `perf-noiop.md`. Compare the state hash too.
**Done when:** `work/t611/gate2.md` holds the three medians and their spread, and says PASS or FAIL against the rule
"at most 3 % slower than `work/baseline/perf.md` in total, same state hash".
**Limits:** never in the same command line as a build. On FAIL the owner's rule is **stop and rethink**.

### Recompiler and toolchain

#### T-612 · Frozen recompiler config
**Model:** Sonnet · **Depends on:** — · **Time box:** 60 min · **Owns:** `tools/pipeline/freeze_recomp_config.py`,
`config/recomp.toml` (new), `work/t612/`
**Read first:** `tools/pipeline/generate.sh` (analyzer around line 97, `make_config.py` around 110, the recompiler
and `audit.py` around 125), `tools/pipeline/make_config.py`.
**Steps**
1. The script produces `config/recomp.toml` from today's `work/don.toml`, and freezes every file that config refers to
   (for example `functions.filtered.csv`) into `config/recomp/`.
   - Allowed: addresses, sizes, names and options.
   - Not allowed: any bytes of the game, or absolute paths. Paths become placeholders that the player's PC fills in.
2. Run `ps2_recomp` with the frozen config into `work/t612/gen`. Compare its tree hash (a sorted `sha256sum` list)
   with today's `generated/`.

**Done when:** the trees are identical (one hash), and `grep -rcE '/media|/home' config/recomp.toml config/recomp/`
prints only zeros.

#### T-672 · Self-contained `ps2_recomp`
**Model:** Sonnet · **Depends on:** T-613 · **Time box:** 60 min · **Owns:** the recompiler's CMake files in
`third_party/PS2Recomp/ps2xRecomp`, a new patch
**Steps:** link `ps2_recomp` statically against libstdc++, zlib and zstd (`ldd` today lists `libz`, `libzstd` and
`libstdc++`). Copy it into `build/toolchain/linux/bin/` with its sha256.
**Done when:** `ldd` lists only libc, libm and the loader, and the binary produces the T-613 tree inside `debian:12`.

#### T-613 · Recompiler patch: progress, exit codes, determinism
**Model:** Sonnet · **Depends on:** T-612, T-606 · **Time box:** 90 min · **Owns:** files in
`third_party/PS2Recomp/ps2xRecomp`, a new patch
**Steps**
1. Add `--out <dir>` and `--elf <path>` overriding the config.
2. Print `[progress] <done>/<total>` lines.
3. Use distinct exit codes for bad input, I/O error and internal error.
4. Make the output order deterministic (sorted).
5. Run it twice into two folders; `diff -r` must be empty.

**Done when:** both runs are identical, and progress lines and exit codes are shown in the report.

#### T-614 · The generator emits against the ABI
**Model:** Sonnet · **Depends on:** T-611 PASS, T-613 · **Time box:** 90 min · **Owns:** `ps2xRecomp` emitter files,
`src/game_module/don_guest_abi.h` (moved from `work/t601`), a new patch
**Steps:** the generated includes become `don_guest_abi.h` only, and the T-601 shims are no longer needed. Regenerate
into `work/t614/gen`, build the module and run `fight_practice`.
**Done when:** the module is built from `work/t614/gen` with no shim directory and the route passes with an equal hash.

#### T-615 · Stable batches and no per-instruction comments
**Model:** Sonnet · **Depends on:** T-614 · **Time box:** 60 min · **Owns:** `ps2xRecomp` emitter, a new patch
**Steps:**
1. The recompiler writes about 1,800 batch files grouped by address range, with a stable rule, instead of 57,727 files.
2. Add an option to drop the per-instruction comments.
3. Compare the module build time and the module hash before and after a change that touches a single function.
4. Write the generated tree's hash (sorted `sha256sum` list, then one sha256 of it) to
   `config/module_reference.toml` under `[generated] sha256`. T-637 rewrites it at release time.

**Done when:** that one-function change rebuilds only one batch (shown by the cache test), the route passes, and the
reference file exists.

#### T-616 · Production module loader
**Model:** Sonnet · **Depends on:** T-610 · **Time box:** 90 min · **Owns:** `src/host/game_module.{h,cpp}`,
`game_module_posix.cpp`
**Steps:**
1. Read `module/<id>/manifest.json` (fields: program version, ABI version, module id, input hashes, toolchain hash,
   module SHA-256, verified).
2. Check the ABI version and the module id against the program; map E58 and E59 (crash marker).
3. Write tests for a missing, wrong-ABI and corrupt manifest, using a dummy module built in the test.

**Done when:** the tests pass and the regression route passes with the module.

#### T-617 · Reference hash of the generated tree
**Model:** Haiku · **Depends on:** — · **Time box:** 45 min · **Owns:** `src/setup/tree_hash.{h,cpp}`,
`tests/setup/test_setup_tree_hash.cpp`
**Steps:**
1. Write a deterministic hash of a folder (sorted relative paths plus contents, SHA-256).
2. Test it: the same tree gives the same hash, and changing one byte, renaming a file or adding a file each give a
   different hash.

**Done when:** `ctest` passes in `build/setup-tests-t617`.

#### T-618 · Pinned toolchain
**Model:** Haiku · **Depends on:** — · **Time box:** 45 min · **Owns:** `tools/pipeline/fetch_toolchain.py`,
`tools/pipeline/toolchain.sha256`
**Steps:**
1. Download the official LLVM 17.0.6 Linux x86_64 release archive and record its SHA-256.
2. Extract only these into `build/toolchain/linux/`, **keeping the symlinks** (`tar` with the member list):
   - the real binaries `bin/clang-17` and `bin/lld`;
   - the symlinks `bin/clang`, `bin/clang++` and `bin/ld.lld`;
   - the clang resource `lib/clang/17/include/`;
   - the LICENSE and NOTICE files.
3. Write `build/toolchain/linux/TOOLCHAIN.sha256` with one hash over the whole tree.
4. Check `ldd build/toolchain/linux/bin/clang` and list the system libraries it needs.

**Done when:** `build/toolchain/linux/bin/clang --version` prints 17.0.6, and the report lists its size and its `ldd`.
**Limits:** do not install anything on the system.

#### T-619 · Toolchain on other distributions
**Model:** Haiku · **Depends on:** T-618, T-603 · **Time box:** 45 min · **Owns:** `work/t619/`
**Steps:** in `docker run --rm debian:12` and in `fedora:40`, with the toolchain folder mounted read-only, compile
`work/t603`'s test unit with the A1 flags and link the tiny `.so`.
**Done when:** both containers compile and link it, and the two object files have the same SHA-256 as on the host.

### Disc layer

#### T-620 · Shared types and the two bugs
**Model:** Sonnet · **Depends on:** — · **Time box:** 45 min · **Owns:** `src/setup/setup_types.h` (new),
`src/setup/disc_image.{h,cpp}`, `src/host/setup_screen_model.{h,cpp}`, `tests/setup/*`
**Steps**
1. Create `setup_types.h` with:
   - `enum class DiscVerdict { Ok, Unreadable, NotDon, Modified, UnknownData }`;
   - `enum class ErrorId : uint16_t`, every ID of A4 as `E01`…`E83`;
   - `struct DiscIdentity` (one definition), `Stage` and `Progress`.
2. Make `disc_image` and the model both use it. The `reason` field becomes a `DiscVerdict`; the English sentences
   move to the model.
3. Fix the model's two failing checks: delete the wrong `a.empty()` at test line 256, and make the saved-progress
   message win over the space line in `view()`. Also fix the `-Wparentheses` warning (cpp:149).

**Done when:** `ctest` in `build/setup-tests-t620` passes both test programs, and `grep -n 'struct DiscIdentity'
src/setup src/host -r` prints exactly one line.

#### T-621 · Disc classifier by magic bytes
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `src/setup/disc_classify.{h,cpp}`,
`tests/setup/test_setup_classify.cpp`
**Steps:**
1. `classify(path) → {kind, detail}`. Kinds: iso, gz, zip, chd, cso, zso, 7z, rar, ecm, nrg, mdf, cue, xdelta, ppf,
   ips, bps, gamecube (magic at 0x1C, `RVZ\1`), folder, directory-with-game, too-small, unknown. Use the first bytes
   only, never the extension, except E09's partial-download names, which are a hint.
2. Test each kind with a synthetic file built in the test.

**Done when:** `ctest` passes with one case per kind.

#### T-622 · Allow-list extraction and per-file manifest
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `tools/disc/make_disc_manifest.py`,
`config/disc_manifest.toml` (new), `src/setup/disc_image.{h,cpp}`
**Steps**
1. The script reads our three discs (paths from `local.env`) and writes, per variant, the allow-listed files with their
   size and SHA-256: `SYSTEM.CNF`, `SLPS_256.75`, `DIR.DIR`, `DATA.AFS`, `BGM.AFS`, `SE.AFS`, `MOVIE.AFS`,
   `MODULES/*`. Hashes only.
2. `extract_disc` copies only the allow-list, hashing each file as it streams it. It returns E18 with the file name on
   a mismatch.

**Done when:** the variant folder made from our English disc totals 263 MB and every file matches the manifest; the
synthetic tests still pass.

#### T-623 · Truncation, read errors, unknown data, folder and device input
**Model:** Sonnet · **Depends on:** T-621, T-622 · **Time box:** 60 min · **Owns:** `src/setup/disc_image.{h,cpp}`,
`tests/setup/test_setup_disc_image.cpp`
**Steps:**
1. Tell E08 (the volume size × 2048 is larger than the file, or a gz/zip CRC or length error) from E19 (`EIO`). Map
   `EACCES` to E05, and `ENOENT`, `ENODEV` and `EIO` at open to E06.
2. Map the classifier's verdicts to E07, E09-E16. For E13 and E16, read `SYSTEM.CNF`'s serial.
3. Resume extraction per file. Read compressed inputs in a single pass, in LBA order (E21).
4. For E17, produce variant `custom-<sha8>`.
5. Accept a folder (E03) and a read-only block device.
6. Call the T-634 hooks `eio@read` and `enospc@copy` once that card has landed; until then, leave a `// T-634 hook`
   comment.
7. Add a test that the built-in table equals `config/discs.toml`.

**Done when:** `ctest` passes, with a case for each of E03, E05-E19, E21 and the table check.

### Engine

#### T-629 · Job queue, journal, resume, stop, speed
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `src/setup/prepare_engine.{h,cpp}`,
`tests/setup/test_setup_engine.cpp`
**Steps:**
1. Write a queue of jobs with weights.
2. Write an atomic journal in `setup.json` (write, fsync, rename).
3. On resume, skip finished jobs and validate them by their output hash.
4. Add `set_speed(FAST|GENTLE)`, `stop()` and `poll()`. Jobs go through a `JobRunner` interface, with a fake runner
   for tests.

**Done when:** `ctest` passes. It covers killing in the middle of a write (journal intact), resume skipping finished
jobs, and stop then continue.

#### T-630 · Time estimate
**Model:** Sonnet · **Depends on:** T-629 · **Time box:** 60 min · **Owns:** `src/setup/estimate.{h,cpp}`,
`tests/setup/test_setup_estimate.cpp`
**Steps:**
1. Before starting: simulate a longest-first schedule over the per-batch times and peak RSS (read from a
   `module_build.toml` given to it), for N jobs and a speed factor. The speed factor comes from compiling our own
   synthetic calibration unit, `src/setup/calibration_unit.cpp`, which this card writes: about 3 s at
   `-O1` on the owner's PC, with no game content.
2. While running: an exponentially weighted rate.
3. Smoothing: at most +20 % between two updates unless the stage changes.
4. Strings, in the model's format.

**Done when:** `ctest` passes, with known schedules and their expected makespans, and the smoothing cases.

#### T-631 · Process platform interface (POSIX)
**Model:** Sonnet · **Depends on:** — · **Time box:** 60 min · **Owns:** `src/setup/platform/process.h`,
`process_posix.cpp`, `tests/setup/test_setup_process.cpp`
**Steps:**
1. `spawn(argv, env, cwd, nice, oom_score_adj)`: a new process group with `PR_SET_PDEATHSIG(SIGKILL)` and
   `TMPDIR` set.
2. `kill_group`, `wait`/`poll` with exit status or signal, captured stderr (bounded), and a per-job timeout.
3. Test with `/bin/sh` children: timeout, kill, the death signal on parent exit (a forked helper) and stderr capture.

**Done when:** `ctest` passes.

#### T-632 · Memory governor and sleep inhibit (Linux)
**Model:** Sonnet · **Depends on:** T-631, T-670 · **Time box:** 60 min · **Owns:** `src/setup/platform/machine.h`,
`machine_linux.cpp`, `src/setup/governor.{h,cpp}`, tests
**First:** `git ls-files | grep -E 'sysinfo|user_dir'`. The host already has `src/host/sysinfo.h`. Reuse what it
offers by calling it, never duplicate it; only add what is missing.
**Steps:**
1. `MemAvailable` and PSI (`/proc/pressure/memory`) twice a second. Under pressure, hold new jobs and ask the engine
   to requeue the newest.
2. Free space with `statvfs` on the nearest existing parent folder.
3. Battery from `/sys/class/power_supply`.
4. Sleep inhibit through a `systemd-inhibit --what=sleep --why=… sleep infinity` child, killed when done; skipped
   when missing.

**Done when:** `ctest` passes, and a run under `systemd-run --user --scope -p MemoryMax=3G` with fake jobs that
allocate memory finishes without OOM-killing the test.

#### T-633 · Error table
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `src/setup/errors.{h,cpp}`,
`tests/setup/test_setup_errors.cpp`
**Steps:**
1. For each A4 ID: code string (`E-SPACE-2` style), title, why, what to do, and primary action. Final English
   wording follows A2 and A3.
2. Test: every `ErrorId` has every field, the codes are unique, and nothing is longer than 2 lines at the S5 width (a
   character budget).

**Done when:** `ctest` passes.

#### T-634 · `DON_TEST_FAULT` hooks (API only)
**Model:** Sonnet · **Depends on:** T-633 · **Time box:** 45 min · **Owns:** `src/setup/faults.{h,cpp}`,
`tests/setup/test_setup_faults.cpp`
**Steps:**
1. Parse `DON_TEST_FAULT` once at start; when it is unset, every hook is a `false` branch.
2. Provide `fault_at(point, progress)` for the points of A4: copy, read, build, unit, verify, oom, noportal. The
   pipeline cards (T-623, T-635, T-636a/b, T-628) call it at their points.

**Done when:** a test proves the parser and each point's trigger, and that with the variable unset `fault_at` costs one
flag check.

#### T-635 · Real pipeline, part 1: extract, translate, check
**Model:** Sonnet · **Depends on:** T-623, T-629, T-631, T-615, T-617, T-672 · **Time box:** 60 min · **Owns:**
`src/setup/steps_translate.cpp`, `tests/setup/test_setup_steps_translate.cpp`
**Steps:** three `JobRunner` implementations:
- extract, through `disc_image`;
- `ps2_recomp` (the self-contained one from T-672), through `process`, reading its `[progress]` lines;
- the tree hash (T-617) against `config/module_reference.toml [generated] sha256` (T-615), giving E52 on a mismatch.

**Done when:** `don --prepare <iso> --until translate` produces `build/gen` with the reference hash; a second run skips
both steps; `DON_TEST_FAULT=hashmismatch@verify` gives E52.

#### T-636a · Real pipeline, part 2: compile with the object cache
**Model:** Sonnet · **Depends on:** T-635, T-618, T-630, T-632 · **Time box:** 60 min · **Owns:**
`src/setup/steps_compile.cpp`
**Steps:**
1. Compile each batch into `cache/obj/<sha of source+flags+toolchain>.o` with the pinned clang and the A1 flags
   (`work/t605/abi/flags.txt`), through the governor.
2. Map E53 (exit 1), E54 (signal) and E55 (timeout) to one retry alone. A missing or damaged tool is E50.
3. ENOSPC on an object (E31) pauses the run and polls the free space; `fault_at("unit")` drives the test.

**Done when:** all batches compile; a second run compiles nothing; `DON_TEST_FAULT=sigkill@unit:17` retries batch 17
and finishes.

#### T-636b · Real pipeline, part 3: link, verify, install
**Model:** Sonnet · **Depends on:** T-636a, T-616 · **Time box:** 60 min · **Owns:** `src/setup/steps_link.cpp`
**Steps:**
1. Link with lld into `module/<id>.tmp/` (sorted objects, deterministic build-id).
2. Compare the SHA-256 with the reference (E57), write `manifest.json`, rename to `module/<id>/`, and keep the previous
   module.
3. Link failure (E56) is retried with nothing else running.

**Done when:** `don --prepare <iso>` builds the module from scratch; after `kill -9` at 30 % it resumes; the module
plays `fight_practice` with `DON_GAME_MODULE`.

#### T-637 · Release references
**Model:** Sonnet · **Depends on:** T-636b · **Time box:** 60 min · **Owns:** `tools/pipeline/make_release_refs.py`,
`config/module_build.toml`, `config/module_reference.toml`
**Steps:** from one clean preparation, write:
- the per-batch time and peak RSS;
- the measured peak disk use of the whole preparation (the "Needs X GB" figure);
- the generated-tree hash;
- the module SHA-256;
- the toolchain hash;
- the module id.

**Done when:** running the script twice gives identical reference files, apart from the time, RSS and disk columns.

#### T-638a · Determinism on this PC
**Model:** Haiku · **Depends on:** T-637 · **Time box:** 60 min · **Owns:** `work/t638/`
**Steps:** prepare the module four ways:
- a clean run;
- another user folder path;
- `DON_JOBS=2` against all cores;
- `LC_ALL=de_DE.UTF-8 TZ=Asia/Tokyo`.

**Done when:** the four SHA-256 equal `config/module_reference.toml`, or the report names the first case that differs
with `cmp` offsets and section names.

#### T-638b · Determinism in another distribution
**Model:** Haiku · **Depends on:** T-638a · **Time box:** 45 min · **Owns:** `work/t638b/`
**Steps:** inside `debian:12`, with the repository and the toolchain mounted read-only and the outputs going to
`work/t638b/`, run the compile and link steps on the T-638a translated tree.
**Done when:** the SHA-256 equals the reference, or the report names the first difference.

### Screens

#### T-624 · Screen model for the new flow
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 60 min · **Owns:** `src/host/setup_screen_model.{h,cpp}`,
`tests/setup/test_setup_screen_model.cpp`
**Steps:** implement A3 exactly:
- S0 results feeding S1;
- no stop after a good check;
- the decision panel;
- S3 with SPEED and STOP;
- close confirmation;
- resume with CONTINUE as the primary action;
- the error panel with the ErrorId's primary action;
- S6 rows and S7.

Every A2 rule gets a test.
**Done when:** `ctest` passes.

#### T-625 · Drawing, part 1: S1, S2, decision, S3
**Model:** Sonnet · **Depends on:** T-624, T-670 · **Time box:** 90 min · **Owns:** `src/host/setup_screen.{h,cpp}`
**Read first:** `src/host/start_screen.cpp` (how the start screen draws), `src/host/ui/theme.cpp`, `widgets.cpp`.
**Steps:** draw from the model's view strings only, with the start screen's widgets and theme, a `FakePrepareEngine`
and `--setup-demo` mode.
**Done when:** `don --setup-demo --screenshot-dir work/t625` writes one PNG per state, and the report shows them.
**Limits:** only `SDL_RenderDebugText` is banned. No new UI library.

#### T-626 · Drawing, part 2: resume, close, S5, banner, controls card
**Model:** Sonnet · **Depends on:** T-625 · **Time box:** 60 min · **Owns:** `src/host/setup_screen.cpp`,
`start_screen.cpp` (banner only)
**Done when:** the demo writes PNGs of these states, and a checklist in the report confirms, per state:
- one gold item;
- reachable with keyboard only and with gamepad only;
- focus visible;
- nothing overlaps at 1280×720 and 3840×2160.

#### T-627 · Discovery
**Model:** Sonnet · **Depends on:** T-621 · **Time box:** 60 min · **Owns:** `src/setup/discover.{h,cpp}`, tests
**Steps:** implement A3 S0:
- the folders listed there, depth ≤ 2;
- no symlinks out of those folders;
- skip network filesystems (`statfs` magic numbers);
- 2 s budget;
- raw ISO checked by descriptor, root and `SYSTEM.CNF` `BOOT2`;
- compressed files listed as "possible".

**Done when:** a test with a fake home tree (the good ISO, another PS2 ISO, a symlink loop, a zip) returns only the
good ISO plus the zip as possible, within the budget.

#### T-628 · In-app browser and drops
**Model:** Sonnet · **Depends on:** T-627, T-626 · **Time box:** 90 min · **Owns:** `src/host/file_browser.{h,cpp}`,
`src/host/setup_screen.cpp` (browser and drop glue only)
**Steps:**
1. A gamepad-friendly list of folders and files: the start places of S0 plus `/`.
2. Each entry shows the classifier's verdict.
3. Native dialog with fallback (E02).
4. Dropping a file or a folder.

**Done when:** demo screenshots show navigation to a file with a gamepad, and a test drives the browser model by key events.

#### T-643 · Settings rows (S6)
**Model:** Sonnet · **Depends on:** T-626, T-645 · **Time box:** 60 min · **Owns:** `src/host/start_screen_model.*`
and `start_screen.cpp` (the settings rows only)
**Done when:** model tests cover:
- switching variants;
- ADD ANOTHER DISC;
- STORAGE sizes;
- FREE UP SPACE (never `memcards/`);
- REPAIR.

#### T-644 · Title progress, chime, flash, battery
**Model:** Sonnet · **Depends on:** T-628 · **Time box:** 45 min · **Owns:** `src/host/setup_screen.cpp` (small block)
**Done when:** the demo shows the title text changing, `SDL_FlashWindow` is called at the end, and the battery
decision appears with a fake `sysinfo`.

#### T-673 · Accessibility and gamepad glyphs
**Model:** Sonnet · **Depends on:** T-644 · **Time box:** 60 min · **Owns:** `src/host/setup_screen.cpp`,
`src/host/ui/widgets.cpp` (additions only)
**Steps:**
1. Gamepad button glyphs from `SDL_GetGamepadButtonLabel`.
2. A UI scale setting (0.75-2.0, saved).
3. Reduce-motion (no animations).
4. Never colour alone: icons with text.
5. Sanitise invalid UTF-8 in names and paths.

**Done when:** demo screenshots at scale 1.0 and 2.0, and with reduce-motion, show no overlap at 1280×720 and
3840×2160.

### Integration, safety, report

#### T-645 · User folder roots and single instance
**Model:** Sonnet · **Depends on:** T-620 · **Time box:** 45 min · **Owns:** `src/setup/user_paths.{h,cpp}`, tests
**First:** find the existing user-folder code (`git grep -n 'DON_USER_DIR' src | head`) and build on it. The program
already decides one user folder (D15, T-524); do not create a second decision.
**Done when:** tests show:
- two roots, equal on Linux;
- a second `acquire_single_instance()` fails while the first is held;
- an unset `HOME` gives a clear E32, not a relative path.

#### T-646 · Startup integrity and recovery
**Model:** Sonnet · **Depends on:** T-645, T-616, T-629 · **Time box:** 60 min · **Owns:** `src/setup/startup.{h,cpp}`,
tests
**Done when:** tests cover:
- E33: a home mounted `noexec` (detected with `statvfs` `ST_NOEXEC`; faked in the test) gives its message;
- E50: the bundled tools' hashes are checked against `build/toolchain/linux/TOOLCHAIN.sha256`;
- E73: stale pids are killed;
- E75: the journal is rebuilt from the manifests;
- E76: only the missing part is redone;
- E77: REPAIR is offered;
- E80: old modules are kept for 30 days, at least two.

Nothing under `memcards/` is touched in any test.

#### T-641 · Netplay handshake carries the module id
**Model:** Sonnet · **Depends on:** T-616, T-662 · **Time box:** 45 min · **Owns:** `src/host/net/protocol.*`, `netplay.*`,
`tests/net/*`
**Done when:** `tests/net` passes, with a new case where peers with different module ids get REJECT and the text
"The host's game was prepared differently (module <id8>)."

#### T-642 · COPY REPORT
**Model:** Haiku · **Depends on:** T-633 · **Time box:** 45 min · **Owns:** `src/setup/report.{h,cpp}`, tests
**Done when:** a test feeds a log with `$HOME` paths and a quoted generated-source line, and the report contains `~`,
no source line, and every A1 field.

#### T-649 · Integration of the first-run flow
**Model:** Sonnet · **Depends on:** T-636b, T-628, T-644, T-646, T-634 · **Time box:** 90 min · **Owns:**
`src/host/main.cpp`, `host.cpp` (the setup entry only)
**Done when:** a new user folder (`DON_USER_DIR=/tmp/t649-user`) goes from the first start to playing through the
setup screens. The disc is chosen with `--setup-auto-pick <iso>` for the test, so no file dialog is needed. Every
setup error also prints one line `[don-setup] error <code> <title>` to the log, which is how drills read it headless.

#### T-650 · Integration: settings, update, netplay, report
**Model:** Sonnet · **Depends on:** T-649, T-643, T-641, T-642 · **Time box:** 60 min · **Owns:** the glue in
`start_screen.cpp`
**Done when:**
- changing the program version string triggers S7 and keeps the old module until the new one is checked;
- COPY REPORT puts the report on the clipboard;
- netplay refuses a mismatched module.

#### T-651 · Like a player
**Model:** Haiku · **Depends on:** T-650, T-661 · **Time box:** 60 min · **Owns:** `work/t651/`
**Steps:** in a real window on the owner's display, with sound to a file and a new user folder, run `build/linux-dev/don`.
Choose the English ISO through the UI, prepare, play 2 minutes. Do it with `DON_JOBS=4` and with all cores.
**Done when:**
- each real time is within ±25 % of the shown estimate;
- the module SHA-256 equals the reference;
- the report has screenshots of S1, S3 and the banner.

#### T-652 · Drills, set 1: disc and file errors
**Model:** Haiku · **Depends on:** T-649 · **Time box:** 60 min · **Owns:** `work/t652/`, `tests/setup/drills_1.sh`
**Done when:** each of E03, E05-E19 gives its exact message and primary action. Inputs are synthetic or derived by
truncating or compressing our ISO into `/tmp`, deleted after.

#### T-653 · Drills, set 2: lifecycle and machine
**Model:** Sonnet · **Depends on:** T-650, T-634 · **Time box:** 90 min · **Owns:** `work/t653/`, `tests/setup/drills_2.sh`
**Done when:** each case passes with its A4 behaviour:
- `DON_TEST_FAULT` kill at 30 % then resume;
- ENOSPC (a small loop-mounted folder is not allowed without sudo, so use `enospc@copy`);
- EIO;
- a second instance;
- the module deleted;
- a corrupt `setup.json`;
- `MemoryMax=3G`;
- a netplay peer with another module.

### Optional / late

#### T-647 · Read from disc drive
**Model:** Haiku · **Depends on:** T-623, T-628 · **Time box:** 45 min
**Done when:** the S1 entry appears only when `/dev/sr*` holds a readable ISO9660, tested with a fake device path
given by an environment variable.

#### T-648 · IOP native in a second ABI (only if T-611 fails on it)
**Model:** Sonnet · **Depends on:** T-611 · **Time box:** 90 min
**Done when:** `perf_fight` meets the 3 % budget with the IOP module loaded from the module folder.

#### T-661 · Developer workflow after the split
**Model:** Sonnet · **Depends on:** T-609, T-636b · **Time box:** 60 min · **Owns:** `tools/pipeline/*.sh`,
`docs/RUNNING.md`
**Done when:** `generate.sh` plus `build.sh linux-dev` still give a playable developer build (linked or with the
module), and `docs/RUNNING.md` says how.

#### T-666 · Developer launcher
**Model:** Haiku · **Depends on:** T-645, T-662 · **Time box:** 45 min · **Owns:** `tools/launcher/*`, `tests/test_launcher.py`
**Done when:** the launcher follows the new user folder layout and its tests pass.

### Legal track (any free slot)

#### T-660 · Scan docs for game content
**Model:** Haiku · **Time box:** 45 min · **Owns:** `work/t660/`
**How:** write a small regex script in `work/t660/scan.py` that finds runs of fenced code with assembly mnemonics or
C bodies longer than 15 lines, and runs of hex-dump lines. Run it over the files and read only its hits, never whole
documents.
**Done when:** `work/t660/list.md` lists, as file:line ranges with a one-line description each:
- every disassembly or decompiled block longer than 15 lines;
- every run of hex dump lines (lines of 16 bytes), such as `03-data-formats.md` and journal 013;
- every game text quoted at length;

in `docs/`, `tests/` and `config/`.

#### T-663 · Apply the trims
**Model:** Sonnet · **Depends on:** T-660, T-664 · **Time box:** 60 min · **Owns:** the files in `work/t660/list.md`
**Done when:** every listed block is reduced to a short fragment or a description, findings keep their meaning
(AGENTS.md: correct, never delete findings), and `public_check` reports no content warning.

#### T-662 · Apply the personal-data replacements
**Model:** Haiku · **Time box:** 45 min · **Owns:** the files in `work/public/personal.md`
**Done when:** the patterns of `personal.md` give 0 hits with `git grep`, and `tests/net` and `tests/test_launcher.py`
still pass. The upstream author's e-mail in patch 0002 stays (owner decision).

#### T-664 · `public_check.py`
**Model:** Haiku · **Depends on:** T-622 · **Time box:** 45 min · **Owns:** `tools/verify/public_check.py`,
`tests/test_public_check.py`
**Done when:** the test passes. The check fails on:
- forbidden extensions (iso, elf, irx, afs, bin…);
- files over 1 MB outside an allow-list;
- personal patterns;
- 16-byte hex lines in runs;
- `generated/` content;
- the SHA-256 of any file listed in `config/disc_manifest.toml`.

#### T-665 · Notices, README, LEGAL, how-to page
**Model:** Sonnet · **Time box:** 60 min · **Owns:** `THIRD_PARTY_NOTICES.md`, `README.md`, `LEGAL.md`,
`docs/HOW_TO_MAKE_A_DISC_IMAGE.md`
**Done when:** the four files exist, with every third-party piece and its licence. That includes LAS Recomp under
GPL-3.0 (owner decision), LLVM, Nunito (OFL) and FFmpeg (stating whether the configuration is LGPL or GPL). The
how-to page links no download site.
