# 057 · CPU saturation breaks fights; the speed comparison before every handover

- **Date:** 2026-10-28
- **Task:** T-756
- **Status:** resolved (tool added); the module-build slowdown itself is journal 054

## Symptom
Twice a build reached the owner slower than the previous one, and fights broke:

- 2026-10-14..18, Windows PC (i5-9600K, 6 cores): the 4-player fight ran at 29-40 fps with the game thread at 100 % of a
  core (the menus ran at 60). The owner saw broken triangles and textures in the specials and noise in the sound
  (also reported by a friend on Windows NVIDIA; never reproduced elsewhere, see `work/HANDOFF.md` section 5).
- 2026-10-28: the module build 38ecd3f against the monolithic 63fc1b6, same fight script, 3 runs each on that PC:
  worst 10 s window **58.5 / 58.4 / 58.4 fps** (old) vs **54.4 / 54.3 / 54.1 fps** (new), wall 130.9 s vs 132.5-133.0 s.
  Screenshots identical. In windowed play the process went to 142 % CPU and the sound broke up.

Every check before the handover measured correctness only (state hashes, 60 fps over a 33 s route), which both builds pass.

## Classification
Performance regression; no catalog row.

## Investigation
- 2026-10-18 perf review (`work/perf-review/report.md`, git-ignored): in the fight the game thread spent ~19 ms per
  blank; the 60 fps budget is 16.7 ms, so it could not keep up. Game code was 2.5 ms of it; the rest was the IOP, the
  scheduler, function dispatch, graphics and memory copies. Timer resolution and power plan were fine.
- Journal 031: the pacing log (`DON_PACING_LOG`) splits each blank into renderer, pacing wait and game-thread work.
- Journal 054: the module build is not slower on Linux (A/B on llvmpipe: game thread 11.05-11.75 ms per blank both),
  so the 2026-10-28 loss is Windows-only; candidates are ranked there.

The mechanism of both symptoms is the same: when the game thread cannot finish a blank in 16.7 ms, the GS packets and the
sound are produced late or dropped, which shows as broken geometry/textures and as noise. What the earlier checks lack
is any number about speed in the part of the game that is slow.

## Cause
Not a bug in one place: nothing in the handover process measured the speed of a fight, so a loss of 7 % (or of a whole
sprint's gain) was invisible until the owner played.

## Fix
- The perf sprint of 2026-10-18 took the game thread from 19 to 12.5 ms per blank in the fight (sprint gain verified).
- Now: `tools/run/fight_compare.py` (Linux) and `tools/run/windows/fight_compare.ps1` (Windows, run over SSH) run the
  fight script N times for a baseline and a candidate, alternating, and print per run the worst 10 s window, the game
  thread's CPU and the state hash. The verdict is FAIL when a run has no result, when the hashes differ, or when the
  candidate's median worst window is below the baseline's median by more than the baseline's own spread (max - min).
  The spread and not a fixed percentage because the noise depends on the machine and the renderer; the baseline's runs
  measure it. `tools/run/perf_fight.py` got `--user-dir DIR` (a temporary copy of a prepared user folder, for module
  builds).
- **Rule:** the fight comparison, new build against the last good one, runs before every handover to the owner, on the
  PC that the owner plays on (or the closest equivalent), with at least 3 runs each.

## Verification
`tests/test_fight_compare.py` pins the verdict on fake run lists (pass inside the spread, pass when faster, fail by
spread with the 58.4 -> 54.3 numbers, fail by hash, fail on a run without result).

Real run on this Linux box, `fight_compare.py` with the same binary as baseline and candidate (logs in `work/t756/`,
git-ignored; load average on the box was 6-20 because other agents were running):
- `--runs 1`: worst window 26.4 vs 20.7 fps, game thread 35.6 vs 35.7 %, same hash: FAIL (spread is 0 with one run, and
  the box was loaded). One run cannot give a verdict.
- `--runs 2`: the first baseline run hit the 200 s limit of `perf_fight.py` at blank 7563 under load and was reported
  as "no result" (hash differs because the trace stops early): FAIL by design. The other three runs: 26.9 / 31.1 / 31.1 fps,
  same hash `0cd1035476044e29`.

## Viability as an orq gate
Per comparison: 2 builds x 3 runs x ~130-200 s = 13-20 min of wall time, and the runs must not share the machine with
builds or other agents (`perf_fight.py` already waits while a build runs, but not for other games or tests). In the sandbox,
T-753 measured llvmpipe fps 31.8-35.3 (spread 3.5, 10 %) while the game thread was steady (11.05-11.75 ms per blank,
6 %); today, with the box loaded, the same binary gave 20.7-31.1 fps. A real 7 % regression (58.4 -> 54.3) is inside that
noise, so **the fps verdict is not usable as an automatic orq gate on a shared Linux box**.
Recommendation: (1) run the PowerShell comparison on the owner's Windows PC (no llvmpipe, measured spread 0.1 fps) as
the handover gate, by the orchestrator, serially; (2) in orq, gate only on the cheap parts (tests, state hash through
`perf_fight.py`), and use the game thread's CPU ms per blank, not fps, if a Linux speed gate is wanted: it was stable
(+-6 %) under llvmpipe. A follow-up could make `fight_compare.py` compare that metric (`cpu_ms_per_blank`), which `perf_fight.py`
already prints.

## Temporary diagnostics
None.
