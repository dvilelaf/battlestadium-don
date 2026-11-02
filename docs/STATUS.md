# Status

Maintained by the orchestrator. Last update: **2026-10-15**.

## Summary

Stage 1 in progress: phase 0 started on 2026-10-11. Tasks are run by agents and reviewed by the orchestrator (up to four Haiku and Sonnet agents on
2026-10-11; from 2026-10-12, by decision of the owner, one single agent on DeepSeek V4.1, one
task at a time), who re-runs each task's "Done when"
check before marking it done.

## Milestones

| Milestone | Status |
|---|---|
| Phase 0 · Foundations | In progress |
| M1 · Menu visible | Not started |
| M2 · Match at 60 fps | Not started |
| M3 · Playable | Not started |
| M4 · Windows and delivery | Not started |

## Already done during the evaluation

| What | Where | Verified |
|---|---|---|
| Knowledge base | `docs/knowledge/` | Each fact carries its mark: measured, declared or unverified |
| Analysis scripts | `tools/*.py` | Used to produce the data; `tools/fpk.py` has tests |
| Patch to build with GCC 11 | `patches/ps2recomp/0001-…` | With it `ps2_analyzer` and `ps2_recomp` build |
| `sqrt.s` patch (PS2Recomp PR #277) | `patches/ps2recomp/0002-…` | `git apply --check` succeeds on the pinned commit. **Not applied yet** in the local clone and not checked in generated code (that is T-002 and T-013) |
| Tests | `tests/test_fpk.py` | 6 tests passing |
| Baseline recompilation (without Ghidra) | `work/analyzer/` | 5,817 functions; 0 errors in real code; 15 fake functions in data |
| PS2Recomp runtime without the game | `third_party/PS2Recomp/out/build-rt/` | Builds with GCC 11.4 |

## Tasks

Statuses: `pending`, `in progress`, `done`, `blocked`.

| Task | Title | Model | Status | Notes |
|---|---|---|---|---|
| T-001 | Environment check | Haiku | done | Verified by the orchestrator on 2026-10-11 |
| T-002 | Fetch and patch PS2Recomp | Haiku | done | Verified by the orchestrator on 2026-10-11; patches 0001 and 0002 now applied in the local clone |
| T-003 | Build the PS2Recomp tools | Haiku | done | Verified by the orchestrator on 2026-10-11 |
| T-004 | Disc verification and extraction | Haiku | done | Verified by the orchestrator on 2026-10-11 against the three ISOs |
| T-005 | Fetch Ghidra and the PS2 extension | Haiku | done | Verified by the orchestrator on 2026-10-11; language id r5900:LE:32:default |
| T-006 | Export the function map with Ghidra | Sonnet | done | Verified 2026-10-11: 7,152 functions and 49,094 labels; analysis takes about 1 minute |
| T-007 | Tests for the existing tools | Haiku | done | Verified by the orchestrator on 2026-10-11 |
| T-008 | Pin the code layout | Sonnet | done | Verified 2026-10-11: main text 0x100000-0x3902D4; one kernel patch island and two false ones in the data |
| T-009 | Function map validator | Sonnet | done | Verified 2026-10-11: text 97.89 % covered; 322 holes of valid code (39 KB) in no row |
| T-010 | Classify the code pointers | Sonnet | done | Verified 2026-10-11: 255 extra entry points (225 inside a row, 30 in no row), nothing rejected |
| T-011 | Assemble the recompiler configuration | Sonnet | done | Verified 2026-10-11: work/don.toml with 164 stubs and 571 entry points; the filtered map passes the validator; hand-added entry points go in config/manual_entry_points.txt |
| T-012 | Generate the code and audit the translation | Haiku | done | Verified 2026-10-11: 57,720 functions recompiled in 11 s, 0 unhandled instructions, 3,617 unresolved-jump warnings; 408 MB of C++ |
| T-013 | Check the `sqrt.s` fix in the generated code | Haiku | done | Verified 2026-10-11 (and by an independent scan): 96 sqrt.s checked, 0 errors, 47 of them with ft != fs |
| T-014 | Build the `don` executable | Sonnet + review | done | 2026-10-13: build scripts never write in place into a mapped file (tools/pipeline/copy_safe.sh, journal 018); optional sanitizer preset linux-asan, not linkable until the container image has the compiler's runtime (a future image change) |
| T-015 | Smoke run and log classification | Sonnet | done | Verified 2026-10-11: tools/run/smoke.sh and the classifier work; first run of the game: starts and stalls silently (NO_PROGRESS) |
| T-016 | Boot loop up to the first frame loop | Orchestrator | done | Verified 2026-10-12: OK_VSYNC_LOOP in three 60 s runs; the game boots to its memory-card dialog and draws it. Five blockers, journals 002 to 006: CD search binding; module load from IOP memory (0004); InitThread priority (0030); ChangeThreadPriority return (0031); the runtime's own guest memory and the console's heap (0032). Open: only about 9 vertical blanks per second reach the game at the dialog (it polls the memory card), and the picture is half width until patch 0021 |
| T-017 | Reference captures with PCSX2, without a person | Sonnet | done | Verified 2026-10-11: 18 captures; route repeatable from power-on, each dump checked by replay and retaken if poor; reference BIOS is the 0200j (the 0210j shows black) |
| T-018 | PCSX2 savestate reader | Sonnet | done | Verified 2026-10-11 on the six real savestates: text unmodified; VU1 programs 0x393D5C, 0x47B89C (menus) and 0x3FFE1C (matches) loaded |
| T-019 | String reference finder | Haiku | done | Verified by the orchestrator on 2026-10-11 |
| T-020 | SDK function policy: recompile or replace | Sonnet | done | Verified 2026-10-11: 164 stub, 33 recompile; the 24 review entries were decided by the orchestrator (knowledge/11, Review) |
| T-021 | Review the 7 instructions the analyzer wants to nop | Sonnet | done | Verified 2026-10-11: all 42 analyzer patches are false positives, none is applied |
| T-022 | Map pack names to `DATA.AFS` indices | Haiku | done | Verified 2026-10-11: alphabetical order of the 135 names (fpack/story/ excluded) fits 133 of 135 packs; to be confirmed at run time |
| T-023 | Decompile a function by address | Sonnet | done | Verified 2026-10-11; one decompile call takes about 3 s |
| T-024 | Portable PCSX2 for reference captures | Sonnet | done | Verified 2026-10-11: PCSX2 v2.8.2 boots the disc with the Japanese BIOS; guide in docs/REFERENCE_CAPTURE.md |
| T-025 | Export contiguous function ranges from Ghidra | Sonnet | done | Verified 2026-10-11 (patch 0003): no nested functions, spans add up to 2.48 MB; text 97.36 % covered |
| T-026 | Make Ghidra claim the code that no function owns | Sonnet | done | Verified 2026-10-11: 870 functions created; every non-zero word of the text is in a row; two runs from scratch give the same 57,789 rows |
| T-027 | Refresh the environment and tools document | Haiku | done | Verified 2026-10-11: knowledge/09 lists the 38 scripts and the pipeline in order |
| T-028 | Static map of the boot sequence | Sonnet | done | Verified 2026-10-11: knowledge/16; frame loop at 0x131CA0 without a blocking vsync; nine likely failures listed in order |
| T-101 | Import inventory of the IOP modules | Sonnet | done | Verified 2026-10-11: 709 imports, 34 unsupported, none of them in the sound modules |
| T-102 | Which modules the game loads, and in what order | Sonnet | pending | |
| T-103 | PCSX2 sources and build audit | Sonnet | done | Verified 2026-10-11; recommends building the bridge inside the PCSX2 CMake project |
| T-104 | Build container image | Sonnet | done | Verified 2026-10-11: image don-build:1 (1.06 GB), clang 17.0.6, cmake 3.31.9, SDL3 3.4.12; software Vulkan (lavapipe 1.3) and patchelf added later the same day |
| T-105 | Build the PCSX2 GS as a library | Sonnet + review | done | Verified 2026-10-11: libdon_gs.so (33 MB) builds in the container in 40 s without Qt and loads on the host; 3 PCSX2 patches |
| T-106 | GS bridge API | Sonnet + review | done | Verified 2026-10-11: API complete; synthetic scene correct with the software renderer (container) and Vulkan (host); state load tested only by round trip |
| T-107 | Replay PCSX2 GS dumps and compare | Sonnet | done | Verified 2026-10-11: the six real dumps replay identically to PCSX2's own runner on both renderers (12 of 12, SSIM 1.0000); 06-results retaken |
| T-108 | Host GS hook in the runtime | Sonnet + review | done | Verified 2026-10-12: patches 0005 (host GS hook) and 0021 (display registers with a host GS); the game's first screen through the PCSX2 GS is 640x448 and equal to the replay of the reference dump except for text that depends on the memory card's state. Open differences in knowledge/18: frame-buffer alternation, DISPLAY1, CSR revision, IMR |
| T-109 | How the runtime wires window, gamepad and audio today | Sonnet | done | Verified 2026-10-11: raylib is called directly from the runtime; 7 runtime patches listed for an SDL3 host (knowledge/13) |
| T-110 | SDL3 host program | Sonnet + review | in progress | Parts 1 to 3 reviewed: runtime patches, the host program, and (2026-10-12) the real game linked with it and built in the container with clang (full dev build 5 min). The card's run gives exit 0 but no screenshot yet, because the game does not draw. Pending: input wiring (T-111, T-112) and the display registers (patch 0021) |
| T-111 | Gamepad (one player) | Sonnet | in progress | Reviewed 2026-10-12: wired into the host program; the game reads the pad (multitap wrappers replaced for two ports, no multitap) and its first dialog is answered by a script. Pending: a real gamepad (T-114) |
| T-112 | Scripted input | Sonnet | in progress | Reviewed 2026-10-12: wired; a script answers the memory-card dialog and takes named shots, two runs identical. The card's own run and docs done except the route to the menu (T-114) |
| T-113 | Skip the videos | Sonnet + Orchestrator | in progress | 2026-10-13: in progress together with microVU as the default (session ses_eca6135d1ffe3XOi3GfPuDTuVa); the committed routes will need retiming afterwards |
| T-114 | Loop up to the main menu | Orchestrator | in progress | 2026-10-13: the route to the title and the main menu works and repeats (tools/run/scripts/to_main_menu.txt; Start skips the black video). Title: logo, characters and text right, background wrong; main menu: garbage (journal 008: the runtime's VU1 interpreter stops the microprogram of those screens). In progress: the same route with PCSX2's two VU1 engines, and who overwrites one word of the game's code |
| T-115 | Image comparator | Haiku | done | Verified by the orchestrator on 2026-10-11 |
| T-120 | Does the game read pixels back from the GS? | Sonnet | done | Verified 2026-10-11: no game code reads pixels back; only RenderWare's raster lock can, through a pointer (run-time check pending) |
| T-121 | Direct accesses to vector unit memory | Sonnet | done | Verified 2026-10-11: the 12 lui 0x1100 build constants (VIF FLUSH words, ids); no direct access to VU memory found |
| T-201 | Census of the VU1 microprograms the game uses | Sonnet | pending | |
| T-202 | Loop up to a match with a correct image | Orchestrator | in progress | 2026-10-13: on the shared build (22 patches plus 0040 and 0042) the picture was compared with the console's screen by screen: title 0.965 of the console's screenshot, the settled menu 0.88, the character select 0.90, the fighters' colours equal to a unit; the only residue is a small brightness difference in the title's crowd (journal 013). The English menu is right (0042). Open: one periodic shot of the Japanese match differs with 0042; the match is reached only with DON_PLAY_MOVIES=1 until the video skip is replaced (T-113) |
| T-203 | Per-subsystem performance measurement | Sonnet | in progress | 2026-10-14: a fight's vertical blank costs the game thread 11.5 ms of 16.67 after patch 0060 (13.3 before); the runtime's own plumbing is about 5 ms of it, the recompiled game code 2 (journal 031). A window now presents without waiting for the display (mailbox); tools/run/headless_desktop.sh gives a real window without the owner's display. Open: the owner has not said whether the stutters are gone; further cuts in the scheduler (the per-dispatch debug stores, the double table read; charging cycles per batch would change guest timing: a decision); whether 0060 alone leaves the state line untouched on the final tree (its proof predates 0059) |
| T-204 | PCSX2 microVU as a library | Sonnet + review | done | Verified 2026-10-11: libdon_vu.so (2.5 MB), microVU and interpreter behind one API; self-test 64 checks, both engines agree; no real game microcode run yet |
| T-205 | Connect the runtime VU1 to microVU | Sonnet + review | in progress | Reviewed 2026-10-13: hook (0020), glue and wiring behind DON_VU1. Measured with the game: with PCSX2's microVU or its interpreter the main menu is drawn as a menu (SSIM 0.08 to 0.48 against the reference) and the runtime's reserved-instruction stops disappear; the two engines agree with each other. Pending decision: make it the default; the title's background is wrong with every engine (another cause) |
| T-206 | Floating point semantics | Sonnet | in progress | Reviewed 2026-10-13: src/host/fp_env sets the game thread to the console's mode (round toward zero, FTZ, DAZ), default mode around GS library calls, DON_FP_MODE=console|host, a test through the synthetic guest; the port now stores the console's bit pattern for the measured constant. Pending from the card: the check with a second guest thread (argued from the code, not measured) and the MSVC flags |
| T-207 | Frame pacing and game speed | Sonnet | in progress | 2026-10-13: series complete to 0039, runtime tests 18 pass, the state at a fixed blank does not depend on host time. Speed without sound about 40 vertical blanks per wall second in the match under load; with sound not measured on this build |
| T-208 | Coverage runs | Haiku + Sonnet | pending | |
| T-209 | Performance: extra measures if 60 fps is not reached | Orchestrator | in progress | 2026-10-14: tests/regression (run with -m slow, about 15 minutes): four routes of the real game with 21 landmarks, liveness and exit code; the same routes with sound for liveness alone. Its first catch: patches 0059 and 0060 moved the Multiplayer route's timing (recorded again). Not in it: the main mode's ten-minute route, items, Tickets Mode, save and load |
| T-210 | Instruction conformance tests of the recompiler and the runtime | Sonnet | in progress | 2026-10-14: the six families are done and the eleven patches 0043 to 0053 are in the series and in the generated code since the regeneration of 2026-10-13 (journal 025, knowledge 21): conformance 215 passed and 133 expected failures, 198 of 331 instructions equal; pictures of two routes byte-identical before and after. Left: the VU0 operations not covered and microprograms; the fetch script's check without --reset misreports (three-line task in journal 025) |
| T-301 | PCSX2 SPU2 as a library | Sonnet + review | done | Verified 2026-10-11: libdon_spu2.so (492 KB), separate library with replacement headers; self-test 24 checks pass; reverb and streaming DMA not exercised |
| T-302 | Connect the IOP emulator to the SPU2 | Sonnet + review | in progress | Patch 0006 in the series (2026-10-11); connection to libdon_spu2.so and real sound modules pending |
| T-303 | Audio output and capture | Sonnet | in progress | 2026-10-14: sound is on by default (--sound off for a silent run); a missing library or device degrades to silence with a [don] line. The fights have effects and sequenced music since patch 0057 (the sound processor's heap; journal 011): by level and chip activity, not yet listened to. Open: the main menu is silent; the heap never reuses freed blocks (being measured over a long session); nobody has judged the sound by ear |
| T-304 | Loop up to sound in the game | Orchestrator | in progress | 2026-10-14: the music plays after the title (patch 0062: the sound processor's scheduler no longer switches a thread out of a section with interrupts suspended; journal 011, piece 15): by measurement on the route (the menu's player plays, the recording has sound in the menu and near full level in the fights, deterministic). Being checked in every mode; nobody has listened yet |
| T-305 | Videos: investigate and decide | Sonnet + Orchestrator | done | 2026-10-15: the game's videos play (FFmpeg on by default on Linux) and Start skips them; patches 0063, 0064, 0068, 0070, 0071, 0072; full regression suite 6/6. Windows package with FFmpeg still to do (patch 0069 drafted) |
| T-306 | Four players: multitap and several gamepads | Sonnet | in progress | 2026-10-14: --players 4 gives the game a multitap with four slots (patch 0061; journal 026): headless, four script-driven fighters played a Multiplayer match to its end, each answering only its own pad. Pending: real gamepads (connection order, hot-plug), the owner's try |
| T-307 | Memory card | Sonnet | in progress | 2026-10-14: checked with today's program (journal 033): save from the options, load at start, load from the options, the option values and the automatic save all persist across runs; a load leaves the file unchanged; a missing card folder is created. Open: a save that cannot be written (read-only card) is silent and returns to the same question; no date is shown anywhere; the card's free space and states as the console reports them |
| T-308 | Button mapping, rumble and gamepad settings | Sonnet | pending | |
| T-320 | Verify the English discs | Haiku | in progress | 2026-10-13: the English main menu is drawn right with patch 0042 (in the series; the orchestrator looked at the shot). Both English variants boot, show English text on every screen checked and reach a fight (without sound). Pending from the card: the runs with sound; a human look at every menu; one periodic shot of the Japanese match differs with 0042 (not explained) |
| T-321 | Soak tests per game mode | Sonnet + Haiku | pending | |
| T-400 | Windows toolchain: choose how to build from Linux | Sonnet + Orchestrator | done | Verified 2026-10-11 (investigation): clang-cl + xwin chosen (D13); GS library builds but cannot run under Wine 6.0.3; full don.exe not built |
| T-401 | Build the runtime and the game for Windows | Sonnet | done (Wine) | 2026-10-14: tools/pipeline/package_windows.py builds build/windows-package/don-windows from the committed HEAD (BUILT_FROM.txt; start.bat prints the commit); under Wine headless the state line equals Linux's. Not tried on a real Windows PC |
| T-402 | PCSX2 bridges for Windows | Sonnet + review | done | 2026-10-14: build/player/don-linux built from the committed HEAD by tools/pipeline/package_linux.py; the launcher prefers it and prints its commit. First folder from 9bb3fe1, state line on quit70 (sound off) equal to the clean one (ramHash=1bc3b23d1246d942) |
| T-403 | One-command build | Sonnet | in progress | 2026-10-14: tools/pipeline/package_windows.sh assembles a folder with don.exe, its libraries, a start.bat and a README for a player (133 MB; nothing of the game, nothing of Microsoft's in it); copied outside the repository and started under Wine with no option it gives the same state line as Linux. Not done: any run on a real Windows PC (the owner's: T-408); the same for Linux (a folder a player can copy) |
| T-404 | Settings file | Haiku | pending | |
| T-405 | User documentation | Haiku | pending | |
| T-406 | Continuous integration | Sonnet | pending | Question P3 |
| T-407 | Metadata and license | Haiku | done | Verified 2026-10-11: LICENSE (GPL-3.0), .recomp.json valid against the published schema, docs/PUBLISHING.md |
| T-408 | Clean machine verification | Human | pending | Includes the owner's Windows PC |
| T-409 | Launcher: choose the language and start the game | Sonnet | in progress | Reviewed 2026-10-13: tools/launcher (21 tests; the picker seen under a private virtual display; the three variants started headless through it). Pending: the owner's try on a real display; Windows; the default binary is build/linux-dev/don |

## Open questions for the user

See [PLAN.md](PLAN.md), section 7.
