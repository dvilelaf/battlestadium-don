# 055 · Windows module at -O1, -O2, -O3: builds and xmm6-15 saves

- **Date:** 2026-10-28
- **Task:** T-754
- **Status:** closed for the `-O` part (PC measured, `-O2` adopted by T-752); a gap of about 1.6 fps remains, see journal 059

## Symptom
Fight slowdown on the owner's Windows PC (worst 10 s window 54.1-54.4 fps with module build 38ecd3f vs 58.4-58.5 with the
monolithic 63fc1b6); Linux shows no such gap (journal 054). Candidate: module compiled at `-O1`; under the Win64 ABI
xmm6-xmm15 are callee-saved, so less inlining could mean more vector save/restore in prologues.

## Classification
Performance, Windows-only. No catalog row.

## Investigation
- Module tree: regenerated with the 4643d4d Linux package's `--prepare` of the English jpvoice image (the T-752 tree was
  not readable); 57725 files. Its Linux module id is `d86e182144617e50`; the id `3bfc5d11cdb626bf` of the PC cannot be
  compared from Linux (the toolchain hash is part of the id), so the tree match is **not verified** from Linux.
  On the PC it is confirmed in practice: the 38ecd3f exe accepted module id `3bfc5d11cdb626bf` with each DLL swapped in
  (the manifest's `module_sha256` was updated for each).
- Three cross-builds of `don_game.dll` (clang 17.0.6, `--target=x86_64-pc-windows-msvc`, `make_ninja.py --target windows`,
  game units with `-O1`/`-O2`/`-O3` edited in the generated `build.ninja`; the registration unit and `win_runtime.cpp`
  stay at `-O1`), `-j4`, one at a time:

| level | build s | size | functions | functions saving xmm6-15 in prologue | prologue saves | all xmm6-15 stack stores | imports |
|---|---|---|---|---|---|---|---|
| -O1 | 679 | 120331264 | 72326 | 2318 | 3550 | 4822 | 0 |
| -O2 | 678 | 110123520 | 72690 | 4153 | 7887 | 9941 | 0 |
| -O3 | 683 | 110605824 | 72690 | 4155 | 7889 | 9943 | 0 |

- All three export `don_game_module_v1`, have 0 imports; `DON_ABI_VERSION` is 1 and `src/game_module` is unchanged since 38ecd3f.
- Counting: `llvm-objdump -d` of the per-unit objects (the DLL has no symbols); prologue = first 40 instructions of a
  function; a save is a `movaps/movups/movdqa/movdqu` (and `v` forms) of `%xmm6..15` to `(%rsp)`.

## PC measurement
Owner's PC (i5-9600K), ougi fight, `tools/run/windows/fight_compare.ps1`, worst 10 s window, all runs with state hash
`0cd1035476044e29`:

| build | runs (fps) | mean |
|---|---|---|
| old monolithic 63fc1b6 | 58.7 - 59.1 | 58.9 |
| module, `-O1` (built here) | 54.9 / 54.1 | 54.5 |
| module, `-O1` (the PC's own build) | 54.2 | 54.2 |
| module, `-O2` | 57.7 / 57.6 | 57.65 |
| module, `-O3` | 57.0 / 57.5 | 57.25 |
| module, `-O2` (second pair, compared with `-O3`) | 57.6 / 57.3 | 57.45 |

`-O3` equals `-O2` within the noise. `-O2` recovers about 3.2 fps of the 4.4 fps gap that `-O1` had to the monolith
(means of the runs above).

## Cause
The evidence ran against the hypothesis in its simple form: the static count of xmm6-15 saves goes up at `-O2`, yet
`-O2` is the faster build, so the static count does not predict the cost. Which functions are hot was not measured here.

## Fix
`-O2` for game units was adopted (T-752, registration unit at `-O1`). Status for the `-O` part: closed. A gap of about
1.6 fps remains against the monolith at `-O2` (journal 059).

## Lessons
The build time does not depend on the level here (679/678/683 s).
