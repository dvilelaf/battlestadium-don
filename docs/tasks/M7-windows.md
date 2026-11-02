# M7 · Windows: the same first-run preparation as Linux, on the player's Windows PC

Owner, 2026-10-27: "b, y poneos ya" — do not publish until Windows works too; start now. M6 (Linux) is done and
played by the owner with a gamepad; its release archive is t-726.

**Goal.** A Windows download (zip) with `Battle Stadium DON.exe`, the PCSX2 DLLs, `ps2_recomp.exe`, a pinned
LLVM 17.0.6 for Windows, the module headers, configs, licences and the GPL source offer. On first run the player
picks their disc image, the program checks it, translates the game code and compiles `don_game.dll` on the player's
PC with that LLVM, then plays. Same screens, same error table, same module ABI as Linux; nothing from the game in
the zip. Out of scope: installers, code signing, Steam, disc drives on Windows (an image file only; drive letters are
"Later"), macOS.

**Design.** The M6 design is platform-independent; Windows adds twins, never forks:
- Module: `don_game.dll`, built by `lld-link` as a DLL with no imports at all (no CRT: `/NODEFAULTLIB /NOENTRY`),
  exporting `don_game_module_v1`; clang `--target=x86_64-pc-windows-msvc`, the same freestanding flags; `__chkstk`
  and `_fltused` supplied by the module itself; unwind through the PE `.pdata` clang emits. The module file name
  and the toolchain tool names (`.exe`) come from one per-platform place, used by the recipe, the loader, startup
  and the manifest.
- Loader: `src/host/game_module_win32.cpp` (`LoadLibraryExW`), twin of `game_module_posix.cpp`.
- Platform twins in `src/setup/platform/`: `process_win32.cpp` (CreateProcessW, a Job object for the group, pipes,
  below-normal priority), `instance_win32.cpp`, `fs_win32.cpp`, `machine_win32.cpp`; `startup.cpp` pid checks
  behind the platform layer; `governor` uses `SetThreadExecutionState`; `discover` searches Windows roots
  (Downloads, Documents, Desktop, PCSX2's ini folder). `crash_trace` gets its unhandled-exception filter.
- Toolchain: the official `LLVM-17.0.6-win64` release, pinned SHA-256, unpacked to `toolchain/` in the zip.
- `ps2_recomp.exe`: built with clang-cl `/MT`, static dependencies, in the existing `don-build-win:1` container
  (decision D13: cross-build from Linux).
- Build: `tools/pipeline/build_windows.sh` gains the module variant (`DON_LINK_GENERATED=OFF`); packaging by
  `package_windows.py` with the M6 layout (shared code with `stage_install.py`).

**Fast check and gates.** Gates stay as in M6 (setup tests, public_check with the owner's patterns, the ASan browser
test) plus, from step 1 on, the Windows cross-build of the setup tests (`don-build-win:1`) running them under Wine
(`tools/container/run_wine.sh`). The fast check for Windows is `tools/run/setup_check.py` driven under Wine once
step 2 delivers a zip (the first card of step 3 makes it run there and lists every failure). Wine is a signal; the
proof is the real Windows PC (step 4) and the owner (step 5).

**Go/no-go gate (step 1, t-740): PASSED 2026-10-27** — whole module-ABI tree (front half tree hash = reference)
compiled with LLVM 17.0.6 `--target windows` into a 120 MB `don_game.dll`, 0 imports, one export
`don_game_module_v1`, 589 s at -j4, no code change (evidence `<swarm>/try/t740/`). The generated tree compiles with the pinned LLVM targeting windows-msvc and
links into `don_game.dll` with an empty import table (`llvm-readobj --coff-imports`), and `don.exe` (module
variant) loads it under Wine and runs the headless route with the 32 state lines equal to Linux. If it fails, the
fallback decided now: link the module against the static MS CRT pieces it needs (`libvcruntime`/`libucrt` from
xwin), still no runtime DLLs.

## Critical path
1. [t-730, t-731, t-732, t-733, t-734, t-735, t-737, t-738, t-739, t-740] Parallel pieces: module DLL recipe and the whole-tree gate (passed), loader, process twin, platform twins, startup + discover, Windows LLVM pin, ps2_recomp.exe (4-7 h)
2. [t-741, t-742, t-743, t-746, t-748] Wire everything (setup tests under Wine, don.exe links), the first real zip proven under Wine: prepare, PLAY, route (2.5-4 h)
3. The orchestrator on the real Windows PC by SSH: unzip, prepare, PLAY, fight script; list every failure (2-4 h)
4. [t-752] One fix round, one card per failure, all at once; new zip (1-3 h). Failure found 2026-10-28: fights
   slower than the old monolithic build (worst 10 s window 54.4 vs 58.5 fps on the owner's PC), module at -O1;
   t-752 fixes the recipe (2-3 h), then new zip + prepare on the PC (1-1.5 h)
   and the speed gate: ougi script on the PC, worst window >= the old build's (0.3-0.5 h)
   Re-estimated 2026-10-28 19:30 after 6 h on this step: -O2 integrated (+3.2 fps); a ~1.6 fps gap remains
   (+0.67 ms work per blank, journal 059), ~0.4 ms of it one large memmove per blank (journal 061). Left: name the
   caller (t-765 sampler with callers, t-762 old/new profiles; 1.5-2.5 h), fix it (1-2 h), new zip + PC speed
   check (1.5 h): 4-6 h
5. Owner on his Windows PC with a gamepad (0.5-1.5 h), only after that speed gate passes

Re-planned 2026-10-27 after a drift audit: the Wine fast check (t-744) and the non-ANSI path fix (t-745) are held
("Later"): the real PC lists failures faster than Wine, and the owner's account name is ASCII. Their cards are in
`<swarm>/.orq/tasks/t-744|t-745/source-card.md`.

## Ownership (step 1)
- t-730: `src/setup/module_build.*`, `src/setup/module_platform.h` (new: module file name, tool names), `src/game_module/**`,
  `tools/module/make_ninja.py`, module tests.
- t-731: `src/host/game_module_win32.cpp`, `src/host/game_module.h`, root `CMakeLists.txt`, `src/host/CMakeLists.txt`,
  `tests/host/CMakeLists.txt`, `tools/pipeline/build_windows.sh`.
- t-732: `src/setup/platform/process.h`, `src/setup/platform/process_win32.cpp`, `src/setup/governor.cpp`, its tests.
- t-733: `src/setup/platform/{instance,fs,machine}_win32.cpp`, `machine.h`, tests.
- t-737: `src/setup/startup.*`, `src/setup/platform/pid_check*`, `src/setup/discover.cpp`, tests.
- t-734: `tools/pipeline/fetch_toolchain.py`, `tools/pipeline/toolchain-windows.sha256`.
- t-738: a new ps2recomp patch, `tools/pipeline/build_tools_windows.sh`.
- t-740 (after t-730): the whole-tree build; may touch t-730's files.
- t-735: `src/host/crash_trace.cpp`, `src/host/crash_trace_win32.cpp`.
Heavy builds one at a time (`sandbox.buildSlots` 1).
