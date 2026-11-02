# 044 · Gate 2: cost of the game as a separate module

- **Date:** 2026-10-26
- **Task:** T-611
- **Status:** open (Gate 2 FAILED by the owner's rule; the A to C part of the gap is not yet explained)

## Symptom
Gate 2 asks whether the game code loaded from `libdon_game.so` (T-610) costs more than the linked build: at most
3 % slower in total than the linked build (A), with the same state hash.

Attempt 1 (53913e5) could not run B: the module build was not reachable from the sandbox and the machine was loaded.
Attempt 2 (this entry) ran it.

## Measurement

Command, per run (15 runs, interleaved A, C, B, A, C, B... five times):

    uv run --no-project python -I tools/run/perf_fight.py --binary <bin> [--game-module <so>] --renderer software --json

| Binary | Build | IOP native | Game code |
|---|---|---|---|
| A | `work/baseline/don` | on | linked |
| C | `build/baseline-noiop/don` | off | linked |
| B | `build/t610-module/don` + `--game-module build/t610-module/libdon_game.so` | off | module |

Binary hashes (sha256, first 16 hex): B `don` 8fe1acf7…, B `libdon_game.so` a5bfce5a…. A and C as in
`work/baseline/don.sha256` and `work/baseline/noiop-don.sha256`.

Environment set, the same for all three binaries and all 15 runs: `DON_GS_DIR=build/gs`, `DON_VU_DIR=build/vu`,
`DON_SPU2_DIR=build/spu2` (main checkout). The trial run of B without any of them found the same libraries by its
default search (`../gs` next to the executable), so the set is the one that is needed; no `LD_LIBRARY_PATH` is set.

Medians of the five runs per binary, with min to max in brackets. Clone commit 52ab099 (tool fixes below).

| Binary | GameThread ms/blank | Total CPU ms/blank (all threads) | fps | state sha256 |
|---|---|---|---|---|
| A | 10.45 (10.26 to 10.48) | 22.81 (22.57 to 22.95) | 59.77 | 9f253556… |
| C | 11.97 (11.68 to 12.24) | 24.31 (23.98 to 24.97) | 59.77 | 9f253556… |
| B | 11.87 (11.78 to 12.02) | 24.11 (23.76 to 24.65) | 59.77 | 9f253556… |

All 15 runs: exit 0, fight end reached, last blank 7740 on the trace, worst 10 s window 60.0 fps (A) or 60 fps (B, C).
The state hash is identical in all 15 runs and equals the hash recorded in `work/baseline/perf.md`.
Load (1 min) before each run: 1.5 to 3.3, below the card's limit of 4.

Ratios of medians:

- **Total CPU per blank, B against A (the rule): 1.057, that is 5.7 % slower. FAIL (limit 3 %).**
- Total CPU per blank, B against C (the module's own cost): 0.992, that is 0.8 % less. No measurable module cost.
- Total CPU per blank, C against A: 1.066 (6.6 % slower). The gap comes with IOP native off, which B and C share.
- GameThread ms per blank: B/A 1.136, B/C 0.992, C/A 1.146.
- fps is 59.77 in every run: the frame rate is capped, so it does not show the cost.

## PASS or FAIL
FAIL. B is 5.7 % slower than A in total CPU per blank, above the 3 % rule. The state hash matches, so the result is
not a behaviour change. Against C, B costs nothing measurable. The gap is the IOP native switch, not the module.
Not measured: a B run with IOP native on (it does not exist as a build), so this entry does not prove that the
module on its own is free with IOP native on.

## Cause
Not determined for the A to C gap (IOP native off costs about 6.6 %). The module's own cost is not visible in the
data (B/C within 1 %, inside the spread of the runs).

## Investigation
- Attempt 1 blocker: B was not reachable, and the load was above 4 (53913e5). The owner has since confirmed the
  machine is quiet, and the binaries were copied to `build/t610-module/`.
- Trial of B (one run, not counted): `perf_fight.py` crashed after the run, on `relative_to(ROOT)` for a binary outside
  the clone. The game run itself finished (exit 0, 7800 frames). Fixed (`shown_path`), see below.
- Round 1, first attempt: all six runs exited 1 (the tool reported the fight end as not reached). The game had exited 0 at 7800 frames; the state trace prints every 60 blanks, so its last line was 7740.
  Fixed (`fight_reached`), see below. The 15 runs in the table come after that fix.

## Fix
No game or runtime change. Changes in the tool, with tests first:
- `tools/run/perf_fight.py`: `shown_path()` prints a binary outside the clone by its absolute path (commit b57299e).
- `tools/run/perf_fight.py`: `fight_reached()` reads the fight end from the exit line's frame count, which is 7800 at the
  end of the fight; the state trace stops one interval (60 blanks) short. The threshold is still the fight end, 7800
  (commit 52ab099).
- `tools/run/perf_fight.py`: `--game-module` passed to the game (commit 90cc77e, attempt 1).

## Verification
    ls work/t611/*.json | grep -vc INVALID             # 15 valid runs (A-1..5, C-1..5, B-1..5)
    uv run --no-project --with pytest python -I -m pytest tests/test_perf_fight_guard.py -q   # 13 passed
Summary per binary: `work/t611/summary.txt` (scratch, git-ignored), made by `work/t611/summarize.py`.
Invalid runs kept as `work/t611/INVALID-*.json`: 6 runs of round 1 with the fight-end bug (exit 1, game exit 0).

## Temporary diagnostics
None.
