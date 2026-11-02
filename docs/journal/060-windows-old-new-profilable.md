# 060 · Two profilable Windows exes: old 63fc1b6 and main, both static, with link maps

- **Date:** 2026-10-28
- **Task:** T-762
- **Status:** closed (PC profiles run; symbol diff in journal 062)

## Symptom
Owner's PC, ougi fight, `DON_PACING_LOG=6000` (fight blanks only): game-thread work per blank 14.29 ms with the old
monolithic 63fc1b6 and 14.96 ms with 9dc800e (+0.67 ms, +4.7 %); GS wait ~0 in both. The -O2 module and a static
main are equally fast (journals 055, 056), so the extra work is in the runtime/host changes since 63fc1b6, Windows only
(Linux shows none, journal 054). Two exes are needed that the Windows sampler (`DON_PROFILE`, T-759) can profile and
that can be compared symbol by symbol.

## Classification
Performance, Windows only. No catalog row.

## Investigation
**A (old).** A git worktree of 63fc1b6 under `work/t762/old/` (only the owned journal is versioned; the worktree was
removed after the build). Its own trees, so the shared `third_party/` and `generated/` were not touched:
- `third_party/` a real folder: symlinks to the main checkout's entries except `PS2Recomp`; `PS2Recomp` is a clone of
  the pinned commit (2c5fbb9, the same pin as today) with `tools/setup/fetch_ps2recomp.sh`, which applied
  `patches/ps2recomp/series` **as it was at 63fc1b6** (0001-0086; main adds 0087-0092).
- `work/analyzer/` copied from the main checkout (the analyzer config is made from the disc, same input).
- Host tools built with `cmake ... -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF`, `-j4`
  (the content of `tools/pipeline/build_tools.sh`); `bash tools/pipeline/generate.sh` (18 s): 57724 files added,
  audit exit 0, then `iop_native`.
- Cherry-picked from main (T-759, `032e221`/`05a31e8`), nothing else: the whole file `src/host/profile_win32.cpp`;
  `src/host/profile.cpp` line 20 `#if !defined(__linux__)` -> `#if !defined(__linux__) && !defined(_WIN32)`;
  `src/host/CMakeLists.txt`: `profile_win32.cpp` added to the `don_host` sources after `profile_linux.cpp`, and
  `target_link_libraries(don_host PRIVATE psapi)` inside `if(WIN32)`.
- `DON_JOBS=4 bash tools/pipeline/build_windows.sh runtime spu2 vu gs` (4 min with the lock wait), then
  `DON_SKIP_CONFIGURE=1 DON_SLICE_SECONDS=1700 ... game` (1366 s, 1877 steps), one slice.

**B (new).** Main at 47467a0 in this clone, same commands against the shared `generated/` (main's patches, e.g. 0088):
runtime/spu2/vu/gs 3 min 36 s; game 1605 s, 1909 steps. `DON_LINK_GENERATED=ON` (the default; the link command has no
`don_game.dll`).

**Flags.** Both: preset `windows-release` (Release, clang-cl + lld-link, `CMAKE_CXX_FLAGS_RELEASE=/O2`,
`-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH`), no LTO, noffmpeg, `-ffp-contract=off -march=x86-64-v3` as in the
presets. The game units get the Release level in both (the `-O2` recipe of T-752 is for the module only).

**Link maps.** No change to any file: from the build folder `ninja -t commands don.exe | tail -1` gives the link command,
re-run in the container with `/MAP:don.map` added after `/out:don.exe` (4 s). Only the PE header differs from the
unmapped link (timestamp / checksum fields; first differing byte 129 in A). The map is `don.map` beside each exe
(A 12.2 MB, B 13.2 MB).

**Folders.** Like the old test bundle without `game\`: `don.exe`, `don.map`, the dependency DLLs of
`third_party/win-deps/clangcl/bin` (as journal 056), the three VC runtime DLLs beside the exe and in `gs/ vu/ spu2/`,
`gs/don_gs.dll` + `gs/resources/`, `vu/don_vu.dll`, `spu2/don_spu2.dll`, `README-TEST.txt`. The bridge DLLs of A were built
from the old tree. `msvcp140_atomic_wait.dll` is not included (journal 058; the PC resolves it).

**Wine start check.** `wine don.exe --help` in each folder exits with code 53 with no program output (no display,
as journal 056); the exes load. Not a functional test.

## Cause
Not determined here. The PC profiles of these two exes are measured and compared symbol by symbol in journal 062.

## Fix
None.

## Verification
Zips in `work/t762/` (not versioned):
- `A-63fc1b6.zip` 39756849 bytes, SHA-256 `76f4a87fb2ade6da966591ff77545fea840b8951a011992639750d5fc900bb1c`
- `B-main.zip` 43092569 bytes, SHA-256 `314b678a5706863fc93819f64fbb4b6bef192d566efd2402380f39afd44d0627`

Build logs: `work/t762/A-build.log`, `B-build.log`.

**PC run (second measurement, orchestrator).** Owner's PC, ougi fight, `DON_PROFILE` + `DON_PACING_LOG=6000`, both
exit 0. Work per blank (fight blanks): A 63fc1b6 14.73 ms (68654 samples), B main 15.37 ms (68306 samples): +0.64 ms.
This repeats the earlier gap of the zip pair (14.29 vs 14.96 ms, +0.67 ms, Symptom above), with new exes and a
different run. B here is main at 47467a0, the commit of the build above; the symptom zip was 9dc800e. Symbol diff,
per-symbol deltas and the large `memmove` copy: journal 062.

## Temporary diagnostics
None.
