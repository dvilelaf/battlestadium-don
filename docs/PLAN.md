# Stage 1 plan

**Goal.** A version of *Battle Stadium D.O.N* that is built from the user's ISO and played on PC
with Windows and Linux, with a gamepad, at 60 fps, with sound, videos, memory card and
4 players, in Japanese or in English.

**How.** Static recompilation of the PS2 executable with PS2Recomp; the graphics chip, the sound
chip and the VU1 vector unit are handled with PCSX2 parts linked as libraries.

**Out of scope.** macOS, browser, more than 4 players, a custom renderer, widescreen,
mods. Reasons in [DECISIONS.md](DECISIONS.md), entry D3.

Related documents: [knowledge/](knowledge/README.md) (what is known),
[DECISIONS.md](DECISIONS.md) (why), [TASKS.md](TASKS.md) (the work split into pieces),
[STATUS.md](STATUS.md) (what is done).

---

## 1. Architecture

```
 User's ISO ──▶ SLPS_256.75 ──▶ Ghidra (function map) ──▶ ps2_recomp ──▶ generated/*.cpp
                                                                                        │
┌───────────────────────────────────────── `don` executable ────────────────────────────┼──────┐
│                                                                                       ▼      │
│  src/host (SDL3)            src/game                       recompiled game code              │
│  window, gamepads,          DON-specific tweaks            (main CPU, VU0 in macro mode)     │
│  audio output,              (PS2_REGISTER_GAME_OVERRIDE)                 │                   │
│  headless mode,                                                          ▼                   │
│  screenshots, scripted  ┌──────────────── PS2Recomp runtime + patches ─────────────────────┐ │
│  input                  │ memory · kernel and system calls · DMA · VIF1 · GIF              │ │
│        │                │ disc (ISO image) · memory card · gamepad                         │ │
│        │                │ IOP emulator running the original .IRX files                     │ │
│        │                └───┬──────────────────┬──────────────────────┬────────────────────┘ │
│        │                    │ GIF bytes        │ microprograms        │ sound registers      │
│        │                    ▼                  ▼                      ▼ and DMA              │
│        │              src/gsbridge       src/vubridge           src/spu2bridge               │
│        │              PCSX2 GS           PCSX2 microVU          PCSX2 SPU2                   │
│        │              (Vulkan)           (milestone M2)         (milestone M3)               │
│        └── window handle ──────────▶ presents                         │                      │
│                                                                       ▼                      │
│                                                              audio output (SDL3)             │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

Who owns what:

| Layer | Origin | Changed with |
|---|---|---|
| Game code | Generated locally from the ISO. Never edited or versioned | `config/don.toml` and `src/game/` |
| Runtime, recompiler, IOP emulator | `third_party/PS2Recomp`, pinned commit | Patches in `patches/ps2recomp/` |
| GS, microVU, SPU2 | `third_party/pcsx2`, v2.8.2 | Patches in `patches/pcsx2/` and glue in `src/*bridge/` |
| Window, gamepads, audio, tests | Own code | `src/host/` |
| DON-specific tweaks | Own code | `src/game/don_overrides.cpp` |

## 2. Repository structure

```
battlestadium-don/
├── README.md                  What it is, status, how to start
├── AGENTS.md  (CLAUDE.md)     Rules for agents and people
├── CMakeLists.txt             Builds `don` (created in T-014)
├── CMakePresets.json          linux-release, windows-release
├── config/
│   ├── discs.toml             Hashes of the supported disc variants
│   ├── layout.toml            Code/data boundary and ELF ranges
│   ├── functions.ghidra.csv   Function map exported from Ghidra
│   ├── extra_entry_points.txt Functions reachable only through a pointer
│   ├── code_pointers_reject.txt
│   ├── stub_policy.toml       Which SDK functions are replaced and which are recompiled
│   ├── instruction_patches.toml
│   └── ghidra_language.txt    PS2 processor identifier in Ghidra
├── patches/
│   ├── ps2recomp/             Numbered patches + `series`
│   └── pcsx2/                 Numbered patches + `series`
├── src/
│   ├── host/                  SDL3: window, gamepads, audio, headless mode
│   ├── game/                  DON settings
│   ├── gsbridge/              C API over the PCSX2 GS
│   ├── vubridge/              C API over microVU
│   └── spu2bridge/            C API over the SPU2
├── tools/
│   ├── setup/                 Download and preparation of dependencies
│   ├── disc/                  Disc extraction and verification
│   ├── funcmap/               Function map: export, validate, complete
│   ├── pipeline/              Generate, audit, build
│   ├── run/                   Smoke run, log classification
│   ├── verify/                Image comparison, checks
│   ├── ref/                   Reading PCSX2 savestates and dumps
│   └── *.py                   Existing analysis scripts
├── tests/                     Tests that do not need the game
├── docs/
│   ├── knowledge/             Knowledge base
│   ├── journal/               One entry per investigated blocker
│   ├── DECISIONS.md  PLAN.md  TASKS.md  STATUS.md
└── (ignored by git) disc/ en_patch/ gc_rvz/ generated/ work/ build/ third_party/
```

## 3. Milestones and deliverables

Each deliverable has a criterion that is checked with a command or a concrete observation.
"Done" means the criterion was run and its output is in the task report, not that the code is
written.

### Phase 0 · Foundations and decision to continue

Planned duration: 1 week. Time box: 2 weeks.

| # | Deliverable | Done when |
|---|---|---|
| E0.1 | Environment setup scripts | `tools/setup/check_env.sh` exits with 0; `tools/setup/fetch_ps2recomp.sh` run twice exits with 0 both times |
| E0.2 | Disc verification and extraction | `tools/disc/verify_disc.py` correctly identifies the three available ISOs and rejects an arbitrary file |
| E0.3 | Function map | `tools/funcmap/validate.py` exits with 0: no overlaps, everything inside the code, all `jal` targets covered, no unclassified code pointer |
| E0.4 | Clean recompilation | `work/reports/translation_audit.json` with `unhandled_in_text = 0` and `functions_in_data = 0` |
| E0.5 | Executable linked with the game on Linux | `cmake --build --preset linux-release` produces `build/linux-release/don` |
| E0.6 | The game reaches its frame loop | `tools/run/smoke.sh` classifies the run as `OK_VSYNC_LOOP` (at least 300 vsyncs in 60 s) |
| E0.7 | PCSX2 reference material | Savestates and GS dumps in `work/ref/pcsx2/` according to the T-017 list |
| E0.8 | Inventory of IOP modules | `docs/knowledge/data/irx_imports.tsv` and a list of unsupported imports |

**Decision gate at the end of phase 0.** Continue if E0.4 and E0.6 are met. Stop and
rethink if, once the 2-week time box is used up:

- the recompilation cannot be left without fake functions, or
- the boot gets stuck on something that requires rewriting a large part of the runtime, or
- E0.8 shows that the sound modules depend on IOP kernel functions that the
  emulator lacks and there are many of them (more than about 30).

### Milestone M1 · Menu visible

| # | Deliverable | Done when |
|---|---|---|
| E1.1 | PCSX2 GS built as a library on Linux | `tools/pipeline/build_gs.sh` produces the library and `gsreplay` |
| E1.2 | Graphics bridge | `gsreplay` replays a PCSX2 GS dump and the image matches the PCSX2 screenshot (SSIM ≥ 0.98) |
| E1.3 | Hook in the runtime | The GIF bytes reach the bridge; the privileged registers and the vsync do too |
| E1.4 | Host program with SDL3 | Window, headless mode, periodic PNG screenshots and scripted input |
| E1.5 | Full IOP boot | All the modules the game requests are loaded; no remote call is left unanswered during boot |
| E1.6 | Videos skipped | The game gets past the opening without hanging |
| E1.7 | **Title and main menu** | With an input script, the program reaches the main menu; the title and menu screenshots match the PCSX2 ones (SSIM ≥ 0.90) and a real gamepad navigates the menu |

### Milestone M2 · Match at full speed

| # | Deliverable | Done when |
|---|---|---|
| E2.1 | Match with a correct image (VU1 interpreter) | Screenshots of a 1v1 match compared with PCSX2: characters, stage and scoreboards present and well formed (human review plus SSIM ≥ 0.80 on the match start screenshot) |
| E2.2 | Performance measurement | Report with the frame time split by subsystem in a 4-player match |
| E2.3 | microVU linked (or alternative decided at gate D5) | Same screenshots as E2.1, no regression |
| E2.4 | **60 fps** | 4-player match, 10 minutes, stable 60 fps (1st percentile ≥ 57 fps) on the reference machine |
| E2.5 | Correct game speed | The match clock advances at the same rate as in PCSX2 (±1 %) |
| E2.6 | Function coverage | 30 minutes of scripted play without `No exact recompiled function` |

### Milestone M3 · Playable

| # | Deliverable | Done when |
|---|---|---|
| E3.1 | Music, voices and effects | They are heard in title, menu and match; 30 s WAV capture without silences or clipping; human review |
| E3.2 | Videos | Opening and ending are seen and heard, or skipping them is decided and documented |
| E3.3 | Memory card | Save, quit, re-enter and load works; the save survives an abrupt shutdown |
| E3.4 | 4 players | Four gamepads control four characters; plugging and unplugging while running does not hang |
| E3.5 | English disc | Both English variants boot and complete a match |
| E3.6 | **Full playthrough** | The single-player main mode is completed from start to credits without hangs, in Japanese and in English; 30-minute session with 4 players |

### Milestone M4 · Windows and delivery

| # | Deliverable | Done when |
|---|---|---|
| E4.1 | Windows build | `tools/pipeline/build_windows.sh` produces `don.exe` from Linux; under Wine it passes the headless menu and 1v1 match scripts; on the owner's Windows PC it passes criteria E1.7, E2.4 and E3.6 |
| E4.2 | One-command build | `tools/build.py --iso <path>` does the whole process on Linux and on Windows from a clean clone |
| E4.3 | Settings | `don.ini` file: resolution scale, full screen, vertical sync, button mapping |
| E4.4 | User documentation | `README.md`, `docs/BUILDING.md`, `docs/CONTROLS.md` |
| E4.5 | Continuous integration | The tests that do not need the game pass on Linux and Windows |
| E4.6 | Clean machine verification | A person who has not taken part follows the documentation and plays |

## 4. Three parallel tracks

So as not to depend on a single chain of tasks:

| Track | What | Starts | Tasks |
|---|---|---|---|
| A · Boot | Function map, recompilation, reaching the frame loop and the menu | Now | T-001 … T-016, T-114 |
| B · Graphics | Build the PCSX2 GS, bridge, dump replay | Now (does not need the game to boot) | T-103 … T-108 |
| C · Sound | Module inventory, SPU2 as a library | The inventory now; the rest in M2 | T-101, T-301 … T-304 |

Tracks A and B meet at T-108 (runtime hook) and T-114 (reach the menu).

## 5. How it is verified

1. **PCSX2 is the oracle.** For each key screen there is a screenshot and, when possible, a
   savestate and a GS dump made with PCSX2 on the same ISO (T-017).
2. **First divergence.** When something looks or behaves differently, look for the first point
   where the port and PCSX2 differ (memory, GIF stream, registers); do not patch the symptom.
3. **Tests without the game.** Every script in `tools/` and every bridge has tests that work
   with synthetic data (`tests/`), so they can run on any machine and in CI.
4. **Headless run.** Every check with the game is a command that ends by itself and
   leaves a log and screenshots: `tools/run/smoke.sh`, input scripts, image comparison.
5. **Nothing is declared playable because of a frame that is not black.** Each report says
   exactly what was run and what was observed.

## 6. Risks

Ordered by impact. "Early signal" is what warns before the risk costs weeks.

| # | Risk | Early signal | Mitigation |
|---|---|---|---|
| R1 | The sound modules do not work in the IOP emulator | T-101: many unsupported imports | Implement the missing ones; as a last resort, rewrite the sound interface at a high level only for what the game uses |
| R2 | Functions reachable only through a pointer go undetected | Repeated `No exact recompiled function` after T-010 | Complete with pointers read from PCSX2 savestates (T-018); coverage runs (T-208) |
| R3 | Building the PCSX2 GS outside its tree costs more than planned, above all on Windows | T-103/T-105 exceed their time box | Reuse the `CMakeLists` from Fukami (Linux) and from Killzone (Windows) as a starting point |
| R4 | The PS2Recomp VU1 interpreter runs the RenderWare microcode wrongly | Broken geometry in E2.1 | Bring microVU forward (T-204/T-205) and use it for validation too |
| R5 | The Windows build is only checked under Wine until the very end | T-400 shows that a route does not build or does not run under Wine | Run T-400 right after the first Linux build; portability rules from day one; CI for what does not need the game; final validation on the owner's Windows PC |
| R6 | PS2Recomp changes daily | A patch stops applying when updating | Pinned commit; update only with a reason and with all the tests |
| R7 | Floating point differences between x86 and the PS2 | Odd physics or collisions, performance drops due to `NaN` | Build options from T-206; lessons from Killzone |
| R8 | The CRI file reader depends on disc timing details | Hangs while loading | Use an ISO image; known latency fixes (catalog in knowledge/08) |
| R9 | The videos require emulating the video hardware | T-305 | Skip them until M3; two alternatives are described |

## 7. Questions for the owner

Answered on 2026-10-11:

| # | Question | Answer | Consequence |
|---|---|---|---|
| P1 | Is there a Windows machine to validate the build? | Yes, but the owner prefers Wine or a VM prepared by the agents. Linux is always the priority | Decision D13: Windows is built from Linux and smoke-tested under Wine; the real Windows PC is used only for the final validation (T-408) |
| P2 | Can the owner generate the reference material with PCSX2? | The owner has a PS2 BIOS and asked what exactly is needed | T-024 prepares a ready-to-run PCSX2; T-017 is then about 15 minutes of play pressing three keys at six moments |
| P3 | Will the repository be published on GitHub? | Yes | Decision D12 (everything in English); continuous integration (T-406) and `.recomp.json` (T-407) are in scope |
| P4 | How many gamepads are available? | 4 | 4 players can be validated with real hardware (T-306) |
| P5 | May the agents accept Microsoft's license terms to fetch the Windows CRT and SDK with `xwin`? | Yes | T-400 can try both routes (`clang-cl` + `xwin`, and `llvm-mingw`) |
| P6 | Where is the PS2 BIOS on this machine? | In a zip in the owner's downloads folder | Two Japanese BIOS files were extracted to a folder outside the repository; T-024 uses that folder through `DON_PS2_BIOS_DIR` (set in `local.env`, which is not versioned) |

Resolved without asking: the compiler. This machine only has GCC 11, which is enough for
PS2Recomp; the PCSX2 parts will be built in a Docker container (decision D11), so
nothing needs to be installed on the system.

Nothing is open at the moment.

## 8. How this plan is run with agents

- **Orchestrator** (a capable model, or a person): assigns tasks, reviews reports, makes the
  decisions marked as gates, runs the unblocking loops and maintains `STATUS.md`.
- **Cheap agents** (Sonnet, Haiku): run tasks from [TASKS.md](TASKS.md), one each,
  with the context that the task itself indicates. They do not make design decisions.
- Each task says which model is enough. The rule: **Haiku** if the task is specified down to the
  command; **Sonnet** if third-party code must be read, integrated, or a build debugged;
  **orchestrator** if game behavior must be diagnosed or something decided.
- The unblocking loops (T-016, T-114, T-202, T-304) cannot be delegated whole, but each
  iteration can be split: a cheap agent runs, collects the log and classifies it; the
  orchestrator decides the fix; another cheap agent applies it and runs again.
- After each task, the agent returns a report in the format of `AGENTS.md`. Without the output
  of the verification command, the task does not count as done.

## 9. Estimate

With one person orchestrating and agents executing, several hours almost every day:

| Stretch | Cumulative weeks |
|---|---|
| Phase 0 | 1–2 |
| M1 · Menu visible | 2–4 |
| M2 · Match at 60 fps | 4–8 |
| M3 · Playable | 6–14 |
| M4 · Windows and delivery | +2–4 over M3 |

This is a judgment based on little data: God of War reached gameplay in 6 days (at 12 fps),
Dragon Quest VIII and SOCOM II in about 2.5 weeks, and Budokai Tenkaichi 3 was declared
playable after 6 weeks. For the two full-speed projects it is not known how long they took.
Phase 0 is what narrows the range.
