# Milestone M4 · Windows and delivery

Goal: what works on Linux works on Windows, and anyone with their ISO can
build and play by following the documentation. Deliverables E4.1 to E4.6 of [../PLAN.md](../PLAN.md).

Before any task: read [../../AGENTS.md](../../AGENTS.md).

> Linux is always the priority. Windows is built from Linux and smoke-tested under Wine
> (decision D13); the owner's real Windows PC is used only for the final validation (T-408).
> T-400 can run early, right after the first Linux build, to find portability problems while
> they are cheap. The other cards fix scope and verification; they are made concrete when M3 closes.
> The portability rules in `AGENTS.md` apply from day one so that this milestone
> is one of validation and not of rewriting.

---

### T-400 · Windows toolchain: choose how to build from Linux

**Model:** Sonnet (investigation) + Orchestrator (decision) · **Depends on:** T-014, T-104 · **Time box:** 6 h

**Read first:** [../DECISIONS.md](../DECISIONS.md) D13; [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md)

**Output:** `docs/knowledge/17-windows-toolchain.md` with a recommendation; experimental scripts under `tools/container/` if useful

**Context.** Linux is the priority. Windows binaries are built from Linux and smoke-tested
under Wine (6.0.3 is installed on this machine). This task can run as soon as the Linux build
of `don` exists (T-014): finding portability problems early is cheaper than at the end.

**Steps**

1. Try both candidate toolchains inside the build container, building only the PS2Recomp
   runtime plus the generated code (no PCSX2 pieces yet):
   - `clang-cl` + `lld-link` with the Microsoft CRT and Windows SDK fetched by `xwin`.
     `xwin` requires accepting Microsoft's license terms; the owner authorized this on
     2026-10-11 (question P5 of the plan), so the acceptance flag may be used. The downloaded
     CRT and SDK stay in `third_party/` and are never versioned or redistributed.
   - `llvm-mingw` (clang targeting `x86_64-w64-mingw32`), which needs no license step.
2. For each route that builds: run the result under Wine with a 60 s headless smoke run and
   record the outcome with `tools/run/classify_log.py`.
3. Check which route can also build the PCSX2 pieces: read how PCSX2 v2.8.2 builds on Windows
   (its dependency bundle is built for MSVC) and whether its GS, microVU and SPU2 sources
   compile with MinGW.
4. Check that each route can also be used natively on Windows by an end user (decision D13,
   "Constraint"): what they would have to install and whether the same CMake preset works.
5. Document: what built, what ran under Wine, compile times, blockers, what an end user on
   Windows would need, and a recommendation.

**Done when** the document has, for each route, a "builds: yes/no", a "runs under Wine:
yes/no" and the evidence (command and trimmed output), and ends with a recommendation.

**Limits.** No changes to shared source files in this task; list the portability problems
found so they become their own tasks. Do not install anything on the host.

---

### T-401 · Build the runtime and the game for Windows

**Model:** Sonnet · **Depends on:** T-400, milestone M3 closed · **Time box:** 10 h

**Read first:** `docs/knowledge/17-windows-toolchain.md`; in God of War (`KIexster/god-of-war-recomp`), `scripts/compilar.ps1`; in Killzone, the `clang-cl` build notes in `docs/findings.md`

**Output:** preset `windows-release` in `CMakePresets.json`, `tools/pipeline/build_windows.sh` (cross-build from Linux), `docs/BUILDING.md` (Windows section)

**Steps**

1. Preset and toolchain file for the route chosen in T-400, built inside the container.
2. Equivalents of the Linux options: AVX2, no floating point contraction, no fast-math.
3. Build `don.exe` without the PCSX2 bridges (internal GS of the runtime) and pass the smoke
   run under Wine.

**Done when**

```bash
bash tools/pipeline/build_windows.sh; echo "exit=$?"                         # exit=0, produces build/windows-release/don.exe
wine build/windows-release/don.exe --headless --seconds 60 --iso "<Japanese ISO>" disc/SLPS_256.75; echo "exit=$?"   # exit=0
bash tools/pipeline/build.sh; echo "exit=$?"                                 # the Linux build still works, exit=0
```

**Limits.** No change may break the Linux build. Linux remains the priority: if a Windows fix
conflicts with Linux behavior, stop and report.

---

### T-402 · PCSX2 bridges for Windows

**Model:** Sonnet + review · **Depends on:** T-401 · **Time box:** 16 h

**Read first:** `docs/knowledge/12-building-pcsx2.md` (Windows section); `docs/knowledge/17-windows-toolchain.md`; in Killzone, `ext/kzgs/CMakeLists.txt` and `ext/kzgs/README.md`

**Output:** `tools/pipeline/build_gs_windows.sh`, `build_vu_windows.sh`, `build_spu2_windows.sh`; minimal changes in `src/*bridge/CMakeLists.txt`

**Steps**

1. Build `don_gs.dll`, `don_vu.dll` and `don_spu2.dll` with the same bridge code as on Linux.
2. Window: pass the SDL3 `HWND` (`window_type = 3`).
3. Run under Wine each bridge's own tests (`gsreplay --synthetic` with the software renderer,
   `vu_selftest`, `spu2_selftest`) and `tools/verify/replay_all.sh` with the software renderer.

**Done when** the three self-tests pass under Wine, the replay of the 6 GS dumps with the
software renderer gives SSIM >= 0.98, and the headless scripts `tests/scripts/menu.txt` and
`tests/scripts/match_1v1.txt` finish with exit 0 under Wine.

**Limits.** The graphics backend on Windows is Vulkan, as on Linux; Direct3D only if Vulkan
turns out not to be viable, and as an orchestrator decision. Vulkan presentation in a real
window is validated on the owner's Windows PC (T-408), not under Wine.

---

### T-403 · One-command build

**Model:** Sonnet · **Depends on:** T-402 · **Time box:** 8 h

**Output:** `tools/build.py`, `tests/test_build_plan.py`

**Steps.** `tools/build.py --iso <path> [--jobs N] [--skip-deps]` (Python standard library only)
runs in order, skipping what is already done and up to date:

1. environment check;
2. disc verification and extraction;
3. fetch and patching of PS2Recomp and PCSX2;
4. build of tools and bridges;
5. generation of the code (the function map comes from the repository: it does not need Ghidra);
6. build of `don`;
7. 30 s smoke run.

Each step prints `[n/7] <name> ... ok|skipped|ERROR`. With `--dry-run` it only prints the plan.
Test: the plan generated for a synthetic folder with some steps already done.

**Done when**, from a clean clone on Linux and on Windows,

```bash
python tools/build.py --iso "<ISO>"      # ends with "[7/7] smoke run ... ok"
```

**Limits.** The end user does not need Ghidra or Docker if that can be avoided; if the PCSX2
bridges require the container on Linux, document it as a requirement.

---

### T-404 · Settings file

**Model:** Haiku · **Depends on:** T-110, T-308 · **Time box:** 3 h

**Output:** `src/host/settings.h/.cpp`, `tests/host/test_settings.cpp`, `docs/SETTINGS.md`

**Specification.** `don.ini` file in the user's data folder (`$XDG_CONFIG_HOME/don/`
or `%APPDATA%\don\`); it is created with default values if it does not exist.

```ini
[video]
renderer = vulkan        ; vulkan | software
upscale = 2              ; 1..8
fullscreen = false
vsync = true

[audio]
volume = 100             ; 0..100

[disc]
iso =                    ; path of the last ISO used

[input.p1]               ; and [input.p2], [input.p3], [input.p4]
device = auto
cross = a                ; SDL button name
circle = b
; ... one value for each PS2 button
```

Command-line options take priority over the file. Invalid values: warning
on `stderr` and the default value. Tests: reading, default values, invalid values,
command-line priority.

**Done when** the tests pass and `docs/SETTINGS.md` documents each key.

---

### T-405 · User documentation

**Model:** Haiku · **Depends on:** T-403, T-404 · **Time box:** 3 h

**Output:** `README.md` (rewritten for users), `docs/BUILDING.md`, `docs/CONTROLS.md`

**Minimum content**

- `README.md`: what it is, real status (what works and what does not, without embellishment), requirements, the three
  supported discs and how they are recognized, how to build in one sentence with a link to
  `BUILDING.md`, legal notice (it does not include the game; each user uses their own disc), license and credits
  (PS2Recomp, PCSX2, Fukami, KillzoneRecomp, Lord Izen for the translation).
- `docs/BUILDING.md`: requirements per system, the single command, measured times and disk space,
  known problems.
- `docs/CONTROLS.md`: default gamepad and keyboard mapping, how to change it.

**Done when** a person who has not taken part (T-408) manages to build by following only
these documents.

**Limits.** No link to downloads of the game or of the BIOS. Do not state anything that is not
verified in `docs/STATUS.md`.

---

### T-406 · Continuous integration

**Model:** Sonnet · **Depends on:** P3 (repository published) · **Time box:** 6 h

**Output:** `.github/workflows/ci.yml`

**Steps.** On Linux and Windows, without any game file:

1. tests in `tests/` with `uv`;
2. `fetch_ps2recomp` and `fetch_pcsx2`: all patches apply from clean;
3. build of tools, runtime and bridges;
4. the bridges' own tests (`gsreplay --synthetic`, `vu_selftest`, `spu2_selftest`);
5. check that the repository does not contain forbidden files (extensions from
   `.gitignore`, files larger than 5 MB).

**Done when** the workflow passes on a test branch on both systems.

**Limits.** Never upload to a runner or to an artifact anything derived from the game.

---

### T-407 · Metadata and license

**Model:** Haiku · **Depends on:** nothing (can be done when publishing is decided) · **Time box:** 1 h

**Output:** `LICENSE` (GPL-3.0 text), `.recomp.json`, `docs/PUBLISHING.md`

**Steps**

1. `LICENSE`: official text of the GNU GPL version 3.
2. `.recomp.json` in the root, according to `https://recomp.fyi/spec`:

   ```json
   {
     "$schema": "https://recomp.fyi/schema/v1.json",
     "game": "Battle Stadium D.O.N",
     "system": "PS2",
     "type": "recomp",
     "toolchain": "PS2Recomp",
     "status": "in-progress",
     "targets": ["Windows", "Linux"],
     "original": { "region": "Japan", "serial": "SLPS-25675" }
   }
   ```

   The `status` field must reflect the real status at the time of publishing (`exploring`,
   `in-progress`, `playable`...).
3. `docs/PUBLISHING.md`: checklist before making the repository or a
   release public: no game file or derivative in the history (`git log --all --stat`
   reviewed), no binary linked with the game in the downloads, third-party credits and licenses,
   `.recomp.json` status up to date, and no machine-specific paths or user names left in the
   documentation (the knowledge base and the task cards currently name local paths of the
   development machine).

**Done when** the three files exist and `python -m json.tool .recomp.json` (with `uv`) gives no error.

---

### T-408 · Clean machine verification

**Model:** Human · **Depends on:** T-405

Two checks, both by a person:

1. Someone who has not taken part, on a Linux machine without the environment set up, follows
   `README.md` and `docs/BUILDING.md` with their own ISO and plays a match. They note each
   point where they had to ask or guess; those points are fixed in the documentation.
2. On the owner's Windows PC: build natively with `python tools/build.py --iso <path>`
   following `docs/BUILDING.md`, run it with a real window, 4 gamepads and sound, and check
   criteria E1.7, E2.4 and E3.6 of the plan. This is the first time the Windows build runs
   outside Wine, so problems are expected here; each one becomes a journal entry and a task.

**Done when** both checks are completed without help from the people who did the port.

---

### T-409 · Launcher: choose the language and start the game

**Model:** Sonnet · **Depends on:** T-320 · **Time box:** 4 h

**Read first:** [../knowledge/07-english-patch.md](../knowledge/07-english-patch.md); `docs/RUNNING.md`;
the launcher of the owner's other project, which the owner asked to copy and adapt (2026-10-13):
repository `f7gs`, file `runtime/scripts/launch.py` with `launch.sh` and `launch.bat` (a Python
launcher: a Tk picker when Tk is available, a terminal prompt otherwise; it remembers the choice;
a language code on the command line skips the picker; `--print` shows the command).

**Output:** `tools/launcher/launch.py`, `tools/launcher/launch.sh`, `tools/launcher/launch.bat`,
`tests/test_launcher.py`, a section in `docs/RUNNING.md`

**Steps**

1. The picker offers the variants whose files are present: Japanese, English with Japanese voices,
   English with English voices (the same executable serves the three). The last choice is
   remembered in the user's configuration folder (`$XDG_CONFIG_HOME/don/` on Linux,
   `%APPDATA%\don\` on Windows). Cancelling exits without starting anything.
2. It prepares the chosen variant's folder with `tools/disc/make_variant_dir.py` (T-320) if it does
   not exist yet, and starts the port with that folder's executable file and the variant's image.
   Arguments after `--` are passed through to the port unchanged.
3. Where the discs and the port are comes from the environment (`local.env`'s variables, `DON_BIN`)
   or from the launcher's own settings file, never from a path written in the code.
4. Tests without the game and without a display: the choice logic, the remembered choice, the
   command built for each variant, the pass-through of arguments, a missing variant.

**Done when** `uv run --no-project --with pytest python -I -m pytest tests/test_launcher.py -q`
passes, `launch.py <variant> --print` prints the right command for each of the three variants, and
a headless start through the launcher (`launch.py <variant> -- --headless --seconds 20`) ends with
exit code 0 for each.

**Limits.** Standard library only (Tk is optional). Linux and Windows. The file's header names
the origin of the adapted code (repository, commit, file).
