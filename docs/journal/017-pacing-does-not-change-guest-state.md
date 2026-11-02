# 017 · The pacing must not change what the guest sees (patch 0038)

- **Date:** 2026-10-13
- **Task:** T-207 (follows 014)
- **Status:** resolved in the runtime tests; open for the game until the orchestrator integrates patch 0038 and
  rebuilds (the check is below)

## Symptom

Journal 014: the same input script gives two different, each reproducible, guest states at vertical blank 60
depending on how much host time a blank carries. Re-measured today with the binary `build/linux-dev/don`
(14:20:07), `work/det/quit70.txt` (`70  quit`), `--headless --renderer software`, two runs each:

| `DON_TRACE_STATE` | iopCycles | iopInstructions | ramHash |
|---|---|---|---|
| 1 (both runs) | 40651185 | 36027780 | b8accb615b71cdaf |
| 60 (both runs) | 40645063 | 35924193 | 5846af9aaaf75faf |

`eeCycle=334387800` in all four. The difference is 40651185 - 40645063 = 6,122 IOP cycles (journal 014 says
6,111: its subtraction was off by 11; the finding is the same).

## Investigation

Read `EeScheduler.cpp` of the series through 0036 (`processDueDeadlines`, `processPendingEvents`,
`waitForEvent`, `scheduleEvent`, `accountCycles`, `flushIopDebt`, `slowPathCheckpoint`, the VBlankStart
handler). What was found, in order:

1. The blank's CYCLE schedule is not host dependent: `processDueDeadlines` reschedules the next blank and the
   VBlankEnd from `scheduled.deadlineCycle` and `scheduled.hostDeadline` (the previous deadlines, not from the
   cycle or the clock at which the event was processed). The busy path waits for the host deadline and then
   processes the event at the same emulated cycle whether it waited or not. The IOP's feed (one cycle per 8) and
   its flush points in these two functions do not depend on the clock either.
2. The idle path does. `waitForEvent` (before the patch, lines 2175-2185) chooses the next stop between the
   earliest scheduled event and the next EE timer interrupt by comparing HOST deadlines:

   ```
   timerHostDeadline = now() + eeCyclesToHostDuration(timerCycles);
   if (timerHostDeadline < hostDeadline) { deadlineCycle = m_eeCycle + timerCycles; ... }
   ```

   The event's host deadline is a fixed point in real time (reset time + k periods); the timer's is "now + its
   duration". With a host that is early, the two orderings agree. With a late host (the blank's host deadline is
   already in the past) the blank is always "earlier": the idle advance jumps straight to the blank's cycle,
   over every timer interrupt that falls before it. `accountCycles` ORs the timer interrupts into a bit mask, so
   seven interrupts become one, delivered at the blank instead of at their cycles; the guest runs different
   code and the IOP is flushed at different points (each stop calls `flushIopDebt`). The same happens, in a
   smaller dose, with a host that is early but lags the absolute schedule by a few ms: two executions of the
   base test, undisturbed, gave 6 and 7 timer interrupts at blank 1.
3. A second, smaller host-clock input in `processDueDeadlines`: after the wait, only the events whose own host
   deadline had passed were taken, the rest left for a later pass. Two events due together were processed
   together (sorted by cycle, type, id) with a late host, or one after the other in host-deadline order, after
   the first one's handler had run, with an early host. Alarms have `now + x` host deadlines, so this can
   differ from cycle order. No test here builds that scenario (see "Not verified").

## Cause

`EeScheduler.cpp` `waitForEvent`, the comparison of host deadlines that decides whether the idle clock stops at
the next timer interrupt or at the next scheduled event (`if (timerHostDeadline < hostDeadline)`). Whether the
game's own divergence at blank 60 goes through this exact branch (the game uses the EE timers and idles on them)
is not verified in the game: the patch cannot be tried until it is integrated and built.

## Fix

`patches/ps2recomp/0038-pacing-does-not-change-guest-state.patch` (not in `series`: the orchestrator integrates
it with the next full build; it applies on the series through 0036, checked on a fresh worktree):

- `waitForEvent` compares the emulated cycles of the two candidates (the timer wins only when strictly earlier,
  a tie goes to the event as before); the host deadline of the chosen one still decides how long the thread
  sleeps.
- `processDueDeadlines` takes all the events whose cycle has been reached once the host has waited for the
  earliest of their host deadlines.

Kept: blank schedule in cycles; the thread still sleeps when early and does not when late; the IOP's feed and
flush points; the fast path (nothing in it changed).

## Verification

`tests/runtime/pacing_neutral_test.cpp` (new): one real `PS2Runtime`, a host function as the guest. A frame loop
that waits for the blank (idle path), two stretches (blanks 10-13 and 24-27) where it spins across the blank
(busy path), TIM0 with a compare every 640,000 EE cycles (ZRET) and an INTC handler. Three runs of 36 blanks in
one process: undisturbed, with 10 ms and with 20 ms of host time burnt in the host GS's vsync callback per blank
(20 ms is more than one period: always late). At every blank: EE cycle, IOP batch statistics, what a host SPU2
sees through `advance()`, guest calls, timer interrupts taken and the cycle of the last. Traces must be equal.

- Base tree (series through 0036), `build/rt-tests-0038-base`: fails, `36 of 36 blanks differ` for both 10 and 20
  ms, e.g. at blank 2 undisturbed `iopRuns=31 timerIrqs=14 lastIrqEe=8960016` against 20 ms `iopRuns=19
  timerIrqs=7 lastIrqEe=4480016`, and 37 blanks recorded instead of 36 in the disturbed runs.
- Patched tree, `build/rt-tests-0038`: `pacing_neutral_test: all checks passed`, three executions in a row. The
  undisturbed trace is now exact arithmetic: `timerIrqs=15 lastIrqEe=9600016` at blank 2 (15 x 640,000).
- Every other test: same last line in both trees (all `all checks passed`) except `mmi_lanes_test`, which is
  another agent's (patch 0037, not in either tree): `26 checks, 11 failed` in both.
- `scheduler_fast_path_test` nanoseconds per dispatch, three runs each (the machine was loaded by other work):
  base 20.31, 22.09, 22.83; patched 30.50, 20.51, 22.13. The fast path is untouched; the 30.50 is noise.

## Check to run after integration (the game)

```bash
set -a; . ./local.env; set +a
for n in 1 60; do
  DON_TRACE_STATE=$n timeout 300 build/linux-dev/don --headless --renderer software --seconds 300 \
    --input-script work/det/quit70.txt disc/SLPS_256.75 2>&1 | grep 'state blank=60 '
done
```

Expected today (without 0038, shown above): the two lines differ (iopCycles 40651185 against 40645063 and
different hashes). Expected with 0038: the two `state blank=60` lines are byte-for-byte equal. If they are not,
the game's divergence goes through another path, and the test above still proves this one. Also compare the lines
of a second pair at `DON_TRACE_STATE=2` and `=10`.

## The relink at 14:01 (loose end of 014)

The explanation (task T-206: the game thread's rounding mode) is confirmed. Binary 14:20 knows `DON_FP_MODE`
(`strings`). `work/det/quit70.txt`, `DON_TRACE_STATE=60`:

| `DON_FP_MODE` | iopCycles | iopInstructions | ramHash |
|---|---|---|---|
| `host` | 40645063 | 35924193 | 2820717a217e95cf (the old hash of 014) |
| `console` | 40645063 | 35924193 | 5846af9aaaf75faf (the new hash) |

The same with `DON_TRACE_STATE=1`: `host` 40651185 / 36027780 / 2ae218445b1589cf, `console` 40651185 / 36027780 /
b8accb615b71cdaf (the latter is today's n=1 hash of the table above). So the counters do not depend on the
floating-point mode, the RAM hash does, and the default is `console`.

## Left alone

- Alarm host deadlines are `now + x` (`setAlarm`, ~line 1203) and the timer's host deadline in `waitForEvent` is
  `now + duration`: they now only decide how long the thread sleeps.
- `processDueDeadlines`' `if (now < pacingDeadline)` wait and `wait_until` in `waitForEvent`: host sleeping only.
- 014's other candidates (`sceCdReadClock`, `rand`, file times) are untouched (not part of this task).

## Not verified

- That the game's n=1/n>=3 divergence is this mechanism (needs the integrated build).
- The change of `processDueDeadlines` (all due events together) has no test of its own.
- The test is a timing test: it needs the 20 ms run to really be late; it asserts that it took longer than the
  paced one.

## Temporary diagnostics

None added. Scratch trees: `work/det/PS2Recomp-0038` (patched), `-0038-base`, `-0038-check` (git worktrees of
`third_party/PS2Recomp`); build folders `build/rt-tests-0038` and `build/rt-tests-0038-base`; logs in
`work/det/logs`.

## With the game: first runs (2026-10-13, T-202 verification)

**Binary.** Not the shared tree and not `build/linux-dev/don`: a side build, `build/linux-3d/don`, against the
runtime tree `work/3d/PS2Recomp` = `third_party/PS2Recomp` HEAD (2c5fbb9) + the committed series 0001-0034 +
`0037-pexew-lanes.patch`, then (relinked, 11 s, only `EeScheduler.cpp` recompiled) + `0038`. Both patches
applied without a conflict on a tree that has no 0035 and 0036. `DON_VU1=microvu`, `--headless --renderer
software`, the machine loaded by other agents. The check of this entry, with `work/3d/state_check.sh`
(`work/det/quit70.txt`, `DON_TRACE_STATE` = 1, 60, 2, 10; lines of blanks 1, 2, 10 and 60):

| binary | `DON_TRACE_STATE` | blank | eeCycle | iopCycles | iopInstructions | ramHash |
|---|---|---|---|---|---|---|
| 0037 only | 1 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| 0037 only | 60 | 60 | 334387800 | 40645063 | 35924193 | a5f5ba7d5609ca76 |
| 0037 only | 2 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| 0037 only | 10 | 60 | 334387800 | 40645063 | 35924193 | a5f5ba7d5609ca76 |
| 0037 only | 10 | 10 | 88622848 | 7532167 | 2811297 | 602e11130ba6d37d |
| 0037 only | 1 and 2 | 10 | 88622864 | 7539062 | 2915657 | 53be1af75e4cdf2a |
| **0037 + 0038** | 1 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| **0037 + 0038** | 60 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| **0037 + 0038** | 2 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| **0037 + 0038** | 10 | 60 | 334387800 | 40651185 | 36027780 | 9c6e2042907a2c76 |
| **0037 + 0038** | 10 | 10 | 88622864 | 7539062 | 2915657 | 53be1af75e4cdf2a |

With 0037 alone the lines differ exactly as in this entry (40651185/36027780 for n = 1 and 2, 40645063/35924193
for n = 60 and 10; the sampling interval changes the result). **With 0038 the `state blank=60` lines of
n = 1, 2, 10 and 60 are byte-for-byte the same, and equal to the lines of the 0037-only runs with n = 1 and 2;
blanks 2 and 10 are equal for n = 1, 2, 10** (blank 10 of n = 10 was different before: `eeCycle=88622848
iopCycles=7532167`; now `88622864 / 7539062`, the value every other run has). So the divergence of 014 went
through the path this patch fixes. Four runs each of 70 blanks, wall about 15 s each.

**The match.** 0038 does not change the game's pictures: with 0037 alone and with 0037 + 0038, the route of
`tools/run/scripts/to_match_1v1.txt` run to blank 8640 produces **144 of 144 automatic shots byte-identical**,
and the title/menu route 7 named and 55 automatic shots byte-identical. In both, the match leaves "GET READY"
(blank 6540) for "FIGHT" (6600) and the fight goes on to a KO and a second round (one run each). The freeze
of journal 010 did not appear in any of the four runs of this task (two with each binary), which is not
evidence that it is gone: it was seen in three of four runs before the fix of 0037, and a run is not
a sample. That the run that got past it was "the slowest" fits the host-speed story of this entry, but this
task did not test the slow-host case on the game (all runs here were at 0.6 of real time).

Not verified: a deliberately slowed run of the game; the game with 0038 and without 0037.
