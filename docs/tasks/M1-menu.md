# Milestone M1 · Menu visible

Goal: with an input script, the program reaches the main menu and what it shows matches
PCSX2. Deliverables E1.1 to E1.7 of [../PLAN.md](../PLAN.md).

Two tracks run in parallel and meet at T-108:

- **Graphics** (does not need the game to boot): T-103 → T-104 → T-105 → T-106 → T-107.
- **Boot**: T-101, T-102, T-109 … T-113 and the loop T-114.

Before any task: read [../../AGENTS.md](../../AGENTS.md).

> The task cards of this milestone fix scope and verification. Their inner steps are made
> concrete when phase 0 closes, with what has been learned (see "Level of detail" in [../TASKS.md](../TASKS.md)).

---

### T-101 · Import inventory of the IOP modules

**Model:** Sonnet · **Depends on:** nothing (can be done during phase 0) · **Time box:** 4 h

**Read first:** [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md); `third_party/PS2Recomp/ps2xIOP/README.md`; code in `third_party/PS2Recomp/ps2xIOP/src/` (how it resolves a module's imports)

**Output:** `tools/re/irx_imports.py`, `tests/test_irx_imports.py`, `docs/knowledge/data/irx_imports.tsv`, `docs/knowledge/14-iop-modules.md`

**Steps**

1. `tools/re/irx_imports.py <package.FPK>`: decompress each `.irx` with `tools/fpk.py` (in
   memory, without writing it to disk) and extract:
   - module name and version (`.iopmod` section);
   - import tables: each one starts with the word `0x41E00000`, followed by a zero, the
     version and the library name (8 bytes), and then pairs of instructions
     `jr $ra` (`0x03E00008`) + `li $zero, <ordinal>` (`0x2400NNNN`), up to two zero words;
   - export tables (word `0x41C00000`): name of the library that the module offers.
2. Run it on `disc/MODULES/0000.FPK` and `1000.FPK` and write `irx_imports.tsv` with
   columns `module`, `imported_library`, `version`, `ordinal`.
3. Find out in the `ps2xIOP` code which (library, ordinal) pairs its virtual core implements
   and add the column `supported` (yes/no).
4. `14-iop-modules.md`: per module, which libraries it offers and which it needs, and the list
   of **unsupported** imports, grouped by library, with a count. Highlight those of
   `sdrdrv`, `modhsyn`, `modmidi`, `modmsin`, `sndfi`, `cri_adxi` and `libsd`.
5. Test of `irx_imports.py` with a minimal synthetic module.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_irx_imports.py -q
uv run --no-project python -I tools/re/irx_imports.py disc/MODULES/1000.FPK | head -20
cut -f1 docs/knowledge/data/irx_imports.tsv | sort -u | wc -l     # 17 if all import something (16 distinct modules + header)
```

The report must give the total number of imports, how many are unsupported and the breakdown per module.

**Limits.** Do not write the decompressed `.irx` files to disk outside `work/`. If the count of
unsupported imports in the sound modules is above 30, highlight it in the first line of the
report: it affects the decision gate of phase 0.

---

### T-102 · Which modules the game loads, and in what order

**Model:** Sonnet · **Depends on:** T-016 · **Time box:** 3 h

**Output:** new section in `docs/knowledge/14-iop-modules.md`

**Steps**

1. Enable (or add as a diagnostic in `src/game/don_overrides.cpp`, controlled by the
   environment variable `DON_TRACE_MODULES=1`) a trace of each module load: name,
   size, result.
2. Run the boot and collect the sequence.
3. Document: load order, whether `tsnddrv`, `tsnddrvc` and `rtfssiop` are loaded, and which
   remote services (SID identifiers) each module registers.
4. Close question Q-LOAD of [../knowledge/11-stub-policy.md](../knowledge/11-stub-policy.md):
   the game loads its modules through `_sceSifLoadModuleBuffer` (`0x114C68`) and
   `_sceSifLoadModule` (`0x115278`), reached from unnamed wrappers at `0x115238` and
   `0x115258`; both are replaced by the runtime handlers of the public functions. Check with
   the caller at `0x136710` that the arguments the handlers read (`$a0` to `$a2`) are what the
   internal functions receive, and what the game does with the result pointer in `$a3`,
   which the handler does not write. If something does not match, propose the fix (bind the
   wrappers instead, or write the result in a game override).

**Done when** the document contains the load table with at least the modules of
`0000.FPK`, and the log of the run is cited (path in `work/logs/`).

**Limits.** The trace must be possible to disable; off by default.

---

### T-103 · PCSX2 sources and build audit

**Model:** Sonnet · **Depends on:** nothing · **Time box:** 4 h

**Read first:** [../knowledge/08-external-references.md](../knowledge/08-external-references.md) (Fukami and Killzone sections); [../DECISIONS.md](../DECISIONS.md) D4, D7 and D11. In `/tmp/don-refs/fukami`: `BUILDING.md`, `scripts/build_pcsx2_gs_bridge.sh`, `tools/linux-build/`, `tools/patches/pcsx2/README.md`. In `/tmp/don-refs/KillzoneRecomp`: `ext/kzgs/README.md`, `ext/kzgs/CMakeLists.txt` (clone both with `git clone --depth 1` if they are not there)

**Output:** `tools/setup/fetch_pcsx2.sh`, `patches/pcsx2/series` (empty), `docs/knowledge/12-building-pcsx2.md`

**Steps**

1. `tools/setup/fetch_pcsx2.sh`: same behavior as `fetch_ps2recomp.sh` (T-002) for
   `https://github.com/PCSX2/pcsx2` in `third_party/pcsx2`, commit
   `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3` (tag `v2.8.2`), with the submodules the
   project needs and the patches of `patches/pcsx2/series`.
2. `12-building-pcsx2.md`, answering with file and line citations:
   - minimum compiler and version, minimum CMake version;
   - list of required dependencies with the Qt interface disabled, and the versions Fukami uses;
   - CMake options Fukami uses for Linux;
   - how Fukami hooks its bridge into the PCSX2 tree (patch `0001`) and how Killzone does it
     (list of files compiled outside the tree, plus replacement files);
   - for Windows: which dependency package PCSX2 uses and what Killzone had to replace;
   - **reasoned recommendation** between the two integration styles, so as to have Linux and
     Windows with the same bridge code.

**Done when**

```bash
bash tools/setup/fetch_pcsx2.sh; echo "exit=$?"                   # exit=0
git -C third_party/pcsx2 rev-parse HEAD                           # fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3
grep -c '^## ' docs/knowledge/12-building-pcsx2.md                # at least 5 sections
```

**Limits.** Do not build anything yet. Do not copy code from Fukami or Killzone into the
repository in this task.

---

### T-104 · Build container image

**Model:** Sonnet · **Depends on:** T-103 · **Time box:** 5 h (includes building dependencies)

**Read first:** `docs/knowledge/12-building-pcsx2.md`; [../DECISIONS.md](../DECISIONS.md) D11

**Output:** `tools/container/Dockerfile`, `tools/container/build_image.sh`, `tools/container/run.sh`

**Steps**

1. `Dockerfile` on `ubuntu:22.04` with the compiler and the dependencies that T-103
   determined (those not packaged, built from source at a pinned version and
   installed in `/opt/deps`), plus `cmake`, `ninja`, `git`, `python3`, `pkg-config`.
2. `build_image.sh`: builds the image with the tag `don-build:1`.
3. `run.sh <command...>`: runs the command inside the container with the repository mounted
   **at the same absolute path** as on the host and with the same user and group (so that
   generated files do not end up owned by `root`).

**Done when**

```bash
bash tools/container/build_image.sh; echo "exit=$?"                         # exit=0
bash tools/container/run.sh bash -c 'c++ --version | head -1; cmake --version | head -1; pkg-config --modversion sdl3'
bash tools/container/run.sh bash -c 'touch work/.container_test' && ls -l work/.container_test
# the file belongs to the login user, not to root
```

**Limits.** Do not install anything on the host. The image must not contain game files.

---

### T-105 · Build the PCSX2 GS as a library

**Model:** Sonnet + review · **Depends on:** T-104 · **Time box:** 8 h

**Read first:** `docs/knowledge/12-building-pcsx2.md`; in Fukami, `tools/patches/pcsx2/0001-*.patch`, `0004-*.patch`, `0005-*.patch` and `tools/pcsx2-gs-bridge/CMakeLists.txt`

**Output:** `patches/pcsx2/000N-*.patch` + `series`, `src/gsbridge/CMakeLists.txt`, `src/gsbridge/don_gs.h` (only `don_gs_version`), `src/gsbridge/don_gs.cpp`, `tools/pipeline/build_gs.sh`

**Steps**

1. Hook patch in the PCSX2 CMake to build an external directory
   (`src/gsbridge`) against its internal targets, and the two generic GS fixes that
   Fukami uses (split data in `GSState::Transfer`, texture cache). Each patch carries in
   its header the source project and commit and the license (GPL-3.0-or-later).
2. Shared library `libdon_gs.so` with a C API. In this task, only
   `int don_gs_version(void)`, which returns `1`. The library statically links its own
   C++ standard library if needed (D11).
3. `tools/pipeline/build_gs.sh`: configures and builds inside the container and leaves the
   result in `build/gs/libdon_gs.so`.

**Done when**

```bash
bash tools/pipeline/build_gs.sh; echo "exit=$?"                   # exit=0
nm -D --defined-only build/gs/libdon_gs.so | grep don_gs_version
ldd build/gs/libdon_gs.so | grep -c 'not found'                   # 0 (run on the host, outside the container)
```

The report must include build time, library size and the list of dynamic libraries it
depends on.

**Limits.** Do not include any Ridge Racer V specific tweak (Fukami's patches `0006`–`0009`
or its "RR5 profile").

---

### T-106 · GS bridge API

**Model:** Sonnet + review · **Depends on:** T-105 · **Time box:** 10 h

**Read first:** in Fukami, `tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h` and `bridge.cpp` (model for the API and for the calls into PCSX2); in Killzone, `ext/kzgs/include/kzgs.h`; [../knowledge/04-graphics.md](../knowledge/04-graphics.md)

**Output:** `src/gsbridge/don_gs.h`, `src/gsbridge/don_gs.cpp`, `src/gsbridge/gsreplay.cpp` (command-line program), `tests/gs/`

**API to implement** (all calls from a single thread):

```c
typedef struct DonGsConfig {
    int   renderer;          /* 0 = Vulkan, 1 = software */
    int   upscale;           /* internal resolution multiplier, 1 = native */
    int   vsync;
    void* native_display;    /* Display* (X11), wl_display* (Wayland), NULL on Windows or headless */
    void* native_window;     /* Window (X11), wl_surface*, HWND; NULL = headless */
    int   window_type;       /* 0 = none, 1 = X11, 2 = Wayland, 3 = Win32 */
    int   width, height;
} DonGsConfig;

int      don_gs_version(void);
int      don_gs_open(const DonGsConfig* cfg, char* err, int err_len);     /* 0 = success */
void     don_gs_close(void);
void     don_gs_reset(void);
void     don_gs_gif_transfer(int path, const uint8_t* data, uint32_t bytes);  /* path 1..3, bytes a multiple of 16 */
void     don_gs_vsync(const uint64_t priv_regs[19], int field);
void     don_gs_read_signals(uint64_t* csr, uint64_t* siglblid);
uint32_t don_gs_read_fifo(uint8_t* dst, uint32_t bytes);                  /* GS → CPU read */
int      don_gs_load_state(const uint8_t* data, uint32_t bytes);          /* GS state from a PCSX2 dump */
int      don_gs_snapshot_rgba(uint8_t* dst, int* width, int* height, int max_bytes);
void     don_gs_resize(int width, int height);
```

**Steps**

1. Implement the API on top of the PCSX2 GS, without its own thread (synchronous call to the
   GIF transfer, as Fukami does).
2. Default configuration: the PCSX2 one for this game. Look for `SLPS-25675` in
   `third_party/pcsx2/bin/resources/GameIndex.yaml`; if it has graphics settings (`gsHWFixes`),
   apply them and document them; if not, PCSX2 defaults.
3. `gsreplay --synthetic <output.png>`: opens the GS headless, sends a GIF stream generated
   by the program itself (clear the screen to one color and a rectangle of another color) and
   saves the image.
4. Test that does not need the game: runs `gsreplay --synthetic` with the software
   renderer and checks the color of two specific pixels.

**Done when**

```bash
bash tools/pipeline/build_gs.sh
bash tools/container/run.sh build/gs/gsreplay --synthetic work/gs_synthetic.png --renderer software; echo "exit=$?"
uv run --no-project --with pytest --with pillow python -I -m pytest tests/gs -q      # green
```

**Limits.** The header `don_gs.h` uses only C types; no PCSX2 header crosses the
API. If an API function cannot be implemented yet (for example `don_gs_read_fifo`),
it is left declared, returns an error and is noted in the report; it is not silently omitted.

---

### T-107 · Replay PCSX2 GS dumps and compare

**Model:** Sonnet · **Depends on:** T-106, T-017, T-115 · **Time box:** 6 h

**Read first:** in PCSX2, the dump replayer (`pcsx2/GS/GSDump*.cpp`, `GSDumpReplayer`) for the `.gs`/`.gs.zst` format, and PCSX2's own dump runner (`pcsx2-gsrunner/Main.cpp`, `pcsx2-gsrunner/test_run_dumps.py`)

**Output:** `gsreplay --dump <file.gs.zst> <output.png>`, `tools/verify/replay_all.sh`, `docs/knowledge/15-gs-dumps.md`

**Context.** A dump taken with Shift+F8 comes with a screenshot, but the two are one frame
apart: PCSX2 saves the screenshot of the frame on screen and then records the commands of the
frame that follows (`pcsx2/GS/Renderers/Common/GSRenderer.cpp`, lines 712-812). On a screen
that does not move they are the same image; in a match they are not. So the exact reference
for a dump is not that screenshot but what PCSX2 itself draws when it replays the dump: its
dump runner (`pcsx2-gsrunner`, a target of the same CMake project, not built by default) does
that without a window.

**Steps**

1. Document the dump format (header, initial GS state, packets: transfer
   per PATH, vsync, FIFO read, privileged registers).
2. `gsreplay --dump`: loads the initial state with `don_gs_load_state`, replays the packets
   with the bridge API and saves the image of the last frame.
3. Build `pcsx2-gsrunner` in the container from the same PCSX2 tree and learn how it writes
   the image of a replayed dump (its options are in `Main.cpp`).
4. `tools/verify/replay_all.sh`: for each dump in `work/ref/pcsx2/`, replays it with
   `gsreplay` and with `pcsx2-gsrunner`, both with the same renderer, and compares the two
   images with `tools/verify/image_parity.py` (this is the check that counts). It also
   compares the `gsreplay` image with the screenshot of the same name and prints that
   figure as information.

**Done when**

```bash
bash tools/verify/replay_all.sh
# one line per dump: <name> SSIM=<value> OK|FAIL (gsreplay against pcsx2-gsrunner) and the
# SSIM against the screenshot as information; all 6 with SSIM ≥ 0.98 against pcsx2-gsrunner
```

**Limits.** If any dump falls below 0.98, do not adjust the threshold: deliver the two
images and the difference in `work/` and describe what differs.

---

### T-108 · Host GS hook in the runtime

**Model:** Sonnet + review · **Depends on:** T-106, T-016 · **Time box:** 8 h

**Read first:** in Killzone, `patches/ps2recomp/0002-*.patch` and `0012-*.patch` and `docs/gs_integration.md`; in `third_party/PS2Recomp/ps2xRuntime`, `src/lib/gs/ps2_gif_arbiter.cpp` and `src/lib/gs/gs_frontend.cpp`; [../knowledge/04-graphics.md](../knowledge/04-graphics.md) (section "What this means for the port"); [../knowledge/13-runtime-host.md](../knowledge/13-runtime-host.md) (rows 3 and 4 of "What a host program of our own needs"); [../knowledge/11-stub-policy.md](../knowledge/11-stub-policy.md) (question Q-GS)

**Output:** `patches/ps2recomp/000N-host-gs-hook.patch`, test in `tests/runtime/`

**Context.** The runtime has an interface for drawing backends (`GSRasterBackend`), but it
receives batches of primitives that the runtime's own GS has already decoded. The PCSX2 GS
needs what the game sent: the GIF packets and the privileged registers. So the hook goes
before the runtime's GS, at the GIF arbiter, and not behind `GSRasterBackend`.

**Steps**

1. Add to the runtime a "host GS" interface: a table of functions (`gif_transfer`,
   `vsync`, `reset`, `read_signals`, `read_fifo`) that can be installed from outside.
2. With a host GS installed: the GIF arbiter forwards to it the packets of the three
   paths; on each vsync the 19 privileged registers are passed to it; the runtime's
   internal GS keeps processing **only** the writes to `SIGNAL`, `FINISH` and `LABEL`, so that
   the game's queries to the status register keep working.
3. Without a host GS installed, the behavior does not change.
4. Test that does not need the game: installs a fake host GS that records the calls,
   sends a minimal GIF packet on each path and checks that they arrive with the exact bytes.
5. Close question Q-GS: 15 `sceGs*` functions of the game are replaced by runtime handlers
   that call the runtime's GS object directly (`runtime->gs()`). List what each handler does
   to the GS (register writes, packets, resets) and say, for each, whether with a host GS
   installed its effect reaches the host GS through the hook of step 2. If any does not,
   propose one of the two fixes for the whole set: route it through the hook, or change the
   15 to `recompile` in `config/stub_policy.toml` (the orchestrator decides).

**Done when** the test passes, and also:

```bash
bash tools/setup/fetch_ps2recomp.sh --reset; echo "exit=$?"       # the new patch applies from clean
bash tools/run/smoke.sh 60                                        # still gives OK_VSYNC_LOOP
```

**Limits.** Do not remove the runtime's internal GS. Do not change the default behavior.

---

### T-109 · How the runtime wires window, gamepad and audio today

**Model:** Sonnet · **Depends on:** nothing · **Time box:** 2 h

**Output:** `docs/knowledge/13-runtime-host.md`

**Steps.** Read `third_party/PS2Recomp/ps2xRuntime` (`include/ps2_host_backend.h`,
`src/main.cpp`, `src/lib/ps2_runtime.cpp`, `src/lib/ps2_pad.cpp`, `src/lib/ps2_audio.cpp`,
`src/lib/Kernel/Stubs/Pad.cpp`, `src/lib/gs/gs_frontend.cpp`) and answer, with file and line:

1. Who creates the window and on which thread the main loop runs.
2. Where each frame is presented.
3. How the gamepad state reaches the game: which structure is filled, who reads it, whether it
   supports several ports and a `slot` other than 0.
4. How audio is output today.
5. What has to change, and where, so that the window, gamepads and audio are provided by a
   program of our own with SDL3 (D9), and whether that can be done without patching the runtime.

**Done when** the document answers the five questions with at least one file and line
citation each.

**Limits.** Read only.

---

### T-110 · SDL3 host program

**Model:** Sonnet + review · **Depends on:** T-108, T-109 · **Time box:** 10 h

**Read first:** `docs/knowledge/13-runtime-host.md`; [../DECISIONS.md](../DECISIONS.md) D9 and D11

**Output:** `src/host/` (`main.cpp`, `options.h/.cpp`, `window.cpp`, `screenshots.cpp`), changes in `CMakeLists.txt`, `tools/pipeline/build.sh` (now builds inside the container)

**Command line options**

| Option | Effect |
|---|---|
| `<ELF path>` | Required |
| `--iso <path>` | Disc image (equivalent to `DON_ISO`) |
| `--headless` | No window; the GS opens without a surface |
| `--seconds <n>` | Ends after `n` seconds with exit code 0 |
| `--shots <folder>` `--shot-every <frames>` | Saves PNGs with `don_gs_snapshot_rgba` |
| `--input-script <file>` | Scripted input (T-112) |
| `--renderer vulkan\|software` `--upscale <n>` | Passed to the bridge |

**Steps**

1. The program creates the window with SDL3 (except with `--headless`), opens the GS bridge with the
   native handle, installs the host GS (T-108), starts the runtime and handles events.
2. Presentation: on each game vsync, `don_gs_vsync`.
3. Exit codes: 0 normal or end of `--seconds`; 2 argument error; 3 error opening
   the GS; 4 the runtime ended with an error.

**Done when**

```bash
bash tools/pipeline/build.sh
build/linux-release/don --headless --seconds 20 --iso "<Japanese ISO>" \
    --shots work/shots/t110 --shot-every 120 disc/SLPS_256.75; echo "exit=$?"       # exit=0
ls work/shots/t110/*.png | wc -l                                                     # at least 5
build/linux-release/don --does-not-exist; echo "exit=$?"                             # exit=2
```

**Limits.** No operating-system-specific API in `src/host/` outside a clearly separated
per-platform file. Paths with `std::filesystem`.

---

### T-111 · Gamepad (one player)

**Model:** Sonnet · **Depends on:** T-110 · **Time box:** 5 h

**Read first:** `docs/knowledge/13-runtime-host.md`; [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md) (section "Gamepad")

**Output:** `src/host/input.h/.cpp`, `tests/host/test_input_mapping.cpp` (or equivalent test)

**Steps**

1. Input layer: state of a PS2 gamepad (16 digital buttons, two sticks, 12 pressure
   values) independent of SDL.
2. Translation from an SDL3 gamepad (standard layout) and from the keyboard (fixed mapping
   documented in the code itself).
3. Delivery of the state to the runtime for port 0.
4. Test that needs neither the game nor a gamepad: the translation function, with
   synthetic inputs, produces the expected button word (active low, as on the PS2) and
   the pressure bytes in the order L1, R1, L2, R2 that Killzone documents.

**Done when** the test passes. The check with a real gamepad belongs to T-114.

**Limits.** One player only; the multitap is T-306.

---

### T-112 · Scripted input

**Model:** Sonnet · **Depends on:** T-111 · **Time box:** 4 h

**Output:** `src/host/input_script.h/.cpp`, `tests/host/test_input_script.cpp`, `docs/INPUT_SCRIPTS.md`

**Input script format** (text, one command per line, `#` for comments):

```
# frame  command   arguments
  600    tap       START 6          # press START for 6 frames
  900    press     DOWN
  906    release   DOWN
  1200   shot      menu             # saves work/shots/<folder>/menu.png
  1500   quit
```

Buttons: `UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 L2 R1 R2 START SELECT L3 R3`.
Optional prefix `P2:`, `P3:`, `P4:` for other players (default `P1:`).

**Steps.** Parser of the format (errors with line number), playback counting presented
frames, and the `shot` and `quit` commands. Tests of the parser and of the sequence
of states that an example script produces.

**Done when** the tests pass and

```bash
printf '120 shot start\n180 quit\n' > work/script_min.txt
build/linux-release/don --headless --iso "<Japanese ISO>" --shots work/shots/t112 \
    --input-script work/script_min.txt disc/SLPS_256.75; echo "exit=$?"     # exit=0
ls work/shots/t112/start.png
```

**Limits.** The script counts presented frames, not real time.

---

### T-113 · Skip the videos

**Model:** Sonnet (investigation) + Orchestrator (decision) · **Depends on:** T-023, T-016 · **Time box:** 5 h

**Read first:** [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md) (section "Video")

**Output:** entry in `docs/journal/`, change in `src/game/don_overrides.cpp`

**Steps**

1. Locate the code that plays a video: with `tools/re/strxref.py` look for references to
   `movie.afs`, `.sfd` and the `mwPly` messages; with `tools/re/callers.py` and
   `tools/re/decompile.sh`, go up to the game function that starts the video and the one that
   checks whether it has finished.
2. Propose in the journal the minimal cut that makes the game believe the video has finished,
   with the decompiled fragment that justifies it.
3. After the orchestrator approves, implement it in `don_overrides.cpp`, active by
   default and disabled with `DON_PLAY_MOVIES=1`.

**Done when** the boot gets past the opening video point (the log shows later
activity: load of `fpack/title/0000.fpk` or equivalent).

**Limits.** Do not touch the CRI libraries more than strictly necessary. Document exactly
which addresses are replaced and why.

---

### T-114 · Loop up to the main menu

**Model:** Orchestrator (with delegated iterations) · **Depends on:** T-107, T-110, T-112, T-113

**Read first:** [../TASKS.md](../TASKS.md) (section "Unblocking loops")

**Output:** patches, settings, journal entries, `tests/scripts/menu.txt` (script that goes from boot to the menu)

**Done when**

```bash
build/linux-release/don --headless --iso "<Japanese ISO>" --shots work/shots/m1 \
    --input-script tests/scripts/menu.txt disc/SLPS_256.75; echo "exit=$?"              # exit=0
uv run --no-project --with pillow --with numpy --with scikit-image python -I tools/verify/image_parity.py \
    work/shots/m1/title.png work/ref/pcsx2/01-title.png --min-ssim 0.90                 # OK
uv run --no-project --with pillow --with numpy --with scikit-image python -I tools/verify/image_parity.py \
    work/shots/m1/menu.png work/ref/pcsx2/02-menu.png --min-ssim 0.90                   # OK
```

and the user confirms that, with a window and a real gamepad, they can navigate the menu (a 5-minute human task).

---

### T-115 · Image comparator

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Output:** `tools/verify/image_parity.py`, `tests/test_image_parity.py`

**Steps.** `tools/verify/image_parity.py <a.png> <b.png> [--min-ssim 0.90] [--crop x,y,width,height] [--diff output.png]`:

1. Loads both with Pillow and converts them to RGB. If the sizes differ, resizes `b` to the
   size of `a` with a bilinear filter.
2. With `--crop`, crops both to that region.
3. Computes SSIM (`skimage.metrics.structural_similarity`, `channel_axis=2`) and the mean
   absolute difference per channel.
4. Prints `SSIM=<4 decimals> MAD=<2 decimals> OK|FAIL` and exits with 0 if SSIM ≥ threshold, 1 otherwise.
5. With `--diff`, saves an image with the absolute difference amplified ×4.
6. Tests with synthetic images: identical (SSIM 1), one with a changed rectangle
   (lower SSIM), different sizes.

**Done when**

```bash
uv run --no-project --with pytest --with pillow --with numpy --with scikit-image python -I -m pytest tests/test_image_parity.py -q
```

**Limits.** None in particular.

---

### T-120 · Does the game read pixels back from the GS?

**Model:** Sonnet · **Depends on:** T-023 · **Time box:** 2 h

**Output:** new section in `docs/knowledge/04-graphics.md`

**Steps.** With `tools/re/callers.py`, list who calls `sceGsExecStoreImage` (`0x00101728`)
and `sceGsSetDefStoreImage` (`0x001015E8`); decompile each caller and explain what it is
used for (for example, a screenshot for the memory card or a screen effect). State whether it happens
during a match or only in menus.

**Done when** the document lists the callers and gives a conclusion: "it is used in …" or "there
are no direct calls".

**Limits.** Read only.

---

### T-121 · Direct accesses to vector unit memory

**Model:** Sonnet · **Depends on:** T-023 · **Time box:** 2 h

**Output:** new section in `docs/knowledge/04-graphics.md`

**Steps**

1. Locate the 12 `lui` instructions with upper part `0x1100` in the code (addresses
   `0x1100xxxx`: microcode and data memory of VU0 and VU1).
2. For each one: the function that contains it and what it does (load microcode, read results...).
3. Check in `third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_memory.cpp` whether that range is
   mapped for reading and writing.

**Done when** the document has the table of the 12 accesses and the conclusion about the
runtime's support.

**Limits.** Read only.
