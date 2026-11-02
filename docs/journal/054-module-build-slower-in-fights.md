# 054 · Module build slower in a fight on Windows, not on Linux

- **Date:** 2026-10-28
- **Task:** T-753
- **Status:** open (cause on Windows not determined; proposed fix not done)

## Symptom
Owner's Windows PC (i5-9600K), `tools/run/scripts/multiplayer_ougi.txt`, 4 players, headless Vulkan, sound on, 7800 blanks,
three runs each. Old monolithic build 63fc1b6: worst 10 s window 58.5 / 58.4 / 58.4 fps, wall 130.9 s. Module build 38ecd3f:
54.4 / 54.3 / 54.1 fps, wall 132.5-133.0 s. Same screenshots. In windowed play CPU reached 142 % and the sound broke up.
T-752 had shown the module's -O level is not the cause on Linux (-O1 10.93, -O2 10.55, -O3 11.08 ms per blank).

## Classification
Performance regression, no catalog row. Not a crash.

## Investigation
Linux A/B on the same machine with `tools/run/perf_fight.py --renderer vulkan --sound on` (a scratch copy that takes
`DON_USER_DIR` from the environment, because the original uses a fresh empty user dir and the module build needs a
prepared one; the per-run user dir was a copy of one prepared with `don --prepare` from the English jpvoice image).
A = the 63fc1b6 build folder copied; B = release package 4643d4d (SHA-256 starts 27fe7cecd6df8d22).

| build | run | worst 10 s window (fps) | GameThread CPU % | ms per blank |
|---|---|---|---|---|
| A | 1 / 2 / 3 | 34.7 / 34.1 / 31.8 | 41.1 / 40.4 / 40.1 | 11.09 / 11.13 / 11.75 |
| B | 1 / 2 / 3 | 33.8 / 0.0* / 34.9 | 40.5 / 40.9 / 41.3 | 11.17 / 11.05 / 11.16 |

\* One window of B run 2 showed 0.0 fps (wall 175 s vs 156-158 s) with the same CPU per blank; a rerun gave 35.3 fps, 11.11
ms. Not reproduced; treated as a one-off stall of the machine.
All seven runs print the same state hash (0cd1035476...ab49d, 129 state lines): same game. The fps figures are limited
by llvmpipe (software Vulkan, headless on this box), so the CPU ms per blank of the game thread is the comparable number.
GsThread: 1.41 vs 1.40 ms per blank. Game thread mean: A 11.32, B 11.12 ms per blank: B is not slower.

Because B is not slower on Linux, no `DON_PROFILE` diff was made (the task asks for it only if B is slower). It follows
that the runtime changes since 63fc1b6 and the module boundary itself (calls through the host-function table,
`src/game_module/don_host_fns.h`) cost nothing measurable on Linux, and the `-O1` module code costs nothing measurable
there either (T-752). The regression is Windows-only.

Windows-only differences between the old exe (monolithic, clang-cl, Release, runtime inlined with the generated code) and
the module path (`don_game.dll`, `clang --target=x86_64-pc-windows-msvc -O1`, `lld-link /NOENTRY /NODEFAULTLIB`,
loaded by `LoadLibraryExW` in `src/host/game_module_win32.cpp`), ranked by likelihood. None is measured on Windows yet.

1. **The -O1 vs -O2 result does not transfer from Linux to the Win64 ABI.** The module is built at `-O1`
   (`src/setup/module_build.cpp:29`, `tools/module/make_ninja.py:28`); the exe is Release. On Win64 xmm6-xmm15 are callee-saved,
   so a float-heavy function that is not inlined saves/restores them in its prologue/epilogue; -O1 inlines less than -O2 and
   so makes more such calls. SysV has no callee-saved vector registers, which is why T-752 saw no difference on Linux. Evidence:
   the code is float/VU-heavy and was built with `-march=x86-64-v3`; the loss is ~7 % in the worst window, ~1 % in wall time,
   i.e. concentrated in the heaviest scenes. Cheapest test: build the Windows module at -O2 and rerun the fight script on the PC.
2. **Calls across the module boundary** go through a pointer table; on Win64 each such call from the module to the runtime
   also needs the 32-byte shadow space and, if the callee is not inlined, the xmm save above. The old exe could inline
   runtime helpers (its Release build has link-time optimisation; the runtime only enables it with Release). Same structure on
   Linux with no cost, so this matters only combined with item 1.
3. **Hand-written `__chkstk`** (`src/game_module/win_runtime.cpp`) is called by every function with a frame of 4 KiB or more;
   the old exe uses the CRT's. Both are the same probing loop; a cost only if generated functions with big frames are hot.
   Unlikely.
4. **Scheduling and sound on Windows.** The 142 % CPU and broken sound in windowed play are consistent with the game thread
   taking longer per blank and the sound thread starving; the Windows timer, thread priority and the mixer are the same code
   as in the old exe, so this is a consequence of 1-2, not a separate cause.
5. **Image layout:** a DLL of ~1900 units at -O1 with no `/OPT` tuning and loaded at a relocated base; code density affects
   the 9600K's instruction cache/iTLB. Possible, no evidence.

## Cause
Not determined on Windows. Determined: not cross-platform (Linux module build equals the monolithic build within noise).

## Fix
Proposed, not done: (a) on the Windows PC, rebuild the module at `-O2` (the `-O1` in `module_build.cpp` / `make_ninja.py`
is a build-time choice for Linux, per T-752) and rerun the fight script 3 times against 63fc1b6; (b) if still slower,
profile with `DON_PROFILE` on Windows for both builds (needs a Windows run; Wine timings are not usable); (c) if the xmm
save is confirmed (disassemble hot functions with `llvm-objdump` from the DLL), consider `-fno-...` inlining thresholds or
compiling the hottest units into the exe instead of the module.

## Verification
`work/t753/results.md` (git-ignored) holds the table; logs in `work/t753/logs/`.

## Temporary diagnostics
None in the repository. The scratch `work/t753/perf_fight_t753.py` (outside git) differs from `tools/run/perf_fight.py` in
three lines (root, `DON_USER_DIR` from the environment, log folder).
