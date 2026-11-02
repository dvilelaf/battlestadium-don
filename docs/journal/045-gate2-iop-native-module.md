# 045 · Gate 2 again: the module with IOP native against the linked build

- **Date:** 2026-10-26
- **Task:** T-648b (after T-648a, which put IOP native into `libdon_game.so`)
- **Status:** resolved (Gate 2 PASS by the owner's rule)

## Symptom
Entry 044 failed Gate 2 only because the module build had no IOP native: B (module, IOP native off) against A
(linked, IOP native on) was 5.7 % slower in total CPU per blank. This entry measures the module with IOP native
on (B) against the linked build with IOP native on (A), the pair the rule asks for. The rule: at most 3 % slower
in total than the linked build, with the same state hash.

## Measurement
Command, per run (10 runs, interleaved A, B, A, B... five times each):

    uv run --no-project python -I tools/run/perf_fight.py --binary <bin> [--game-module <so>] --renderer software --json

| Binary | Build | IOP native | Game code | sha256 (first 16 hex) |
|---|---|---|---|---|
| A | `work/baseline/don` (main checkout) | on | linked | `df12d161…` |
| B | `build/t648-module/don` + `--game-module build/t648-module/libdon_game.so` (main checkout) | on | module | `ce6fb24b…` (`don`), `98719d05…` (`libdon_game.so`) |

Environment set, the same for both binaries and all runs, with absolute paths to the main checkout (the clone has
no `build/`): `DON_GS_DIR=<main checkout>/build/gs`,
`DON_VU_DIR=<main checkout>/build/vu`,
`DON_SPU2_DIR=<main checkout>/build/spu2`. No `LD_LIBRARY_PATH`.

Clone at `9c38c7f` (origin/main, no merge needed). Journal 044 is the method this repeats.

Log line of B in every run: `[don-iop] native module cri_adxi active (2328 entry points)`.

Medians of the five runs per binary, with min to max in brackets (`work/t648b/summary.txt`, scratch):

| Binary | GameThread ms/blank | Total CPU ms/blank (all threads) | fps | state sha256 |
|---|---|---|---|---|
| A | 10.27 (10.22 to 10.32) | 22.52 (22.29 to 22.57) | 59.77 | 9f253556… |
| B | 10.40 (10.20 to 10.76) | 22.69 (22.25 to 23.13) | 59.77 | 9f253556… |

Ratios of medians:

- **Total CPU per blank, B against A (the rule): 1.0075, that is 0.75 % slower. PASS (limit 3 %).**
- GameThread ms per blank, B against A: 1.013 (1.3 %, inside the spread of the runs).
- fps: 59.77 in every run (frame rate capped, so it does not show the cost).

All 10 runs: exit 0, fight end reached (7800 frames), worst 10 s window 60.0 fps, state hash identical in all 10
runs and equal to the hash of entry 044 and `work/baseline/perf.md`. Load (1 min) before each run 1.0 to 2.7 and
after each run 1.3 to 2.7, below the card's limit of 4. No build or other game was running during the runs.
No run was invalid; none was repeated.

A single trial run of B was made before the ten (startup check, not counted); it printed the same state hash and
the same native module line.

## PASS or FAIL
PASS. B is 0.75 % slower than A in total CPU per blank, under the 3 % rule, with the same state hash. The spread of
B (22.25 to 23.13) overlaps the spread of A (22.29 to 22.57); the margin to the limit is wide, about 2.25 points.

## Not measured
Software renderer only; one fight route; one machine. Module load time and memory were not measured. GameThread
+1.3 % (B against A) is within B's wider spread and is not separated from noise. Five runs per binary.

## Cause
Not applicable: the gap of entry 044 (B against A) was the missing IOP native in the module, and it is closed.
The earlier A to C gap (IOP native off, about 6.6 %) is still the same fact as in 044: IOP native itself costs
about 6 % when it is off, and the module with it on costs about 0.75 % more than the linked build.

## Verification
    ls work/t648b/*.json | grep -vc INVALID     # 10 valid runs (A-1..5, B-1..5)
    grep -n 'PASS\|FAIL' docs/journal/045-gate2-iop-native-module.md | head -5
    git log --oneline -2

## Temporary diagnostics
None.
