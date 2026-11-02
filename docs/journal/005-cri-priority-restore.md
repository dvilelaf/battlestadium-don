# 005 · The ADXF poll loop keeps the main thread at priority 1, and starves the CRI threads

- **Date:** 2026-10-12
- **Task:** T-016
- **Status:** resolved (patch `0031-changepriority-return.patch`; journal 006 for what stops the
  boot after the wait at `0x331C60` is passed)

## Symptom
With patch 0030 applied (journal 004), `bash tools/run/smoke.sh 60` still classifies as
`NO_PROGRESS`: `RESULT: NO_PROGRESS [don] vsync 840`, `... vsync 1200`, `... vsync 1320` in three
runs, `frames=0` in all three; the log ends after `[sceCdSearchFile:ok] path="\\MOVIE.AFS;1"` with
no `[don] frame` line. At the time limit the game thread is in the poll loop
`FUN_00331c60` → `FUN_00311468` (`0x331C60` → `0x311468`), as in journal 004. Logs:
`work/logs/smoke-20261012-183436.log`, `work/logs/smoke-20261012-184014.log` (the second one with
`DON_TRACE_THREADS=1`) and the three runs of the final check.

## Classification
`NO_PROGRESS` (`tools/run/classify_log.py`). No precedent in the catalog of
`docs/knowledge/08-external-references.md`.

## Investigation
1. **The guest threads, measured** with `DON_TRACE_THREADS=1` (the table the runtime's own
   scheduler publishes, printed every 60 vertical blanks; the diagnostic is in
   `src/game/don_overrides.cpp`): from the first second of the run to the last,
   `t1 run prio 0/1` (main thread, running, current priority 1),
   `t2 susp prio 1/8` (suspended by the game), `t3 ready prio 1/16`, `t4 ready prio 1/18`,
   `t5 susp prio 1/25`, and only the semaphores 1 and 2 exist. t3 and t4 are **Ready** with
   `wait=0/0` (no wait reason) and their program counter frozen inside the SDK `SleepThread`
   wrapper (`pc=0x0010C1C8`): they are runnable and never run.
2. **Why Ready threads at 16 and 18 never run.** The scheduler preempts the running thread only for
   a thread of equal or *higher* priority (numerically lower):
   `checkpointDue` returns true only when `hasReadyAtOrAbovePriority(running->currentPriority)` is
   true (`EeScheduler.cpp:379-385`), and `hasReadyAtOrAbovePriority` looks at the queues
   `0..priority` (`:2089-2100`); `selectReady` takes the first non-empty queue, index 0 being the
   highest priority (`:1614-1630`). The main thread at priority 1 is the highest of the five and it
   spins in `while (FUN_00311468(n) != 3) FUN_00315928();` (the poll loop of journal 004), so the
   CRI threads never get the CPU. This is the same rule as the EE kernel's strict priority
   scheduling, so the main thread must not be at priority 1 while it polls.
3. **The main thread's priority is raised and never restored.** Trace with `tools/run/gdb_trace.sh`
   and a breakpoint on `ps2_syscalls::ChangeThreadPriority` printing `$a0` and `$a1`
   (`work/logs/t016e-d.log`):
   `ChangeThreadPriority(1, 0x18)` once, `ChangeThreadPriority(2, 8)`, `(3, 0x10)`, `(4, 0x12)`,
   `(5, 0x19)` once each, and then **18 pairs** of `ChangeThreadPriority(1, 1)` followed by
   `ChangeThreadPriority(1, 0)`, all of them from the game's SDK wrapper
   `ChangeThreadPriority_0x10c130`.
4. **Where the pairs come from.** They are a critical section of the CRI library: `0x314320`
   (registered as a callback by `0x314FF0` itself) does
   `DAT_003d9c10 = ChangeThreadPriority(GetThreadId(), 1)`, and `0x3143a8` (the other callback) does
   `ChangeThreadPriority(GetThreadId(), DAT_003d9c10)`
   (`bash tools/re/decompile.sh 0x314320 0x3143a8`). The exit restores the **return value** of the
   entry — in the SDK, `ChangeThreadPriority` returns the thread's previous priority (or a negative
   error). The same function's own `ChangeThreadPriority(GetThreadId(), DAT_003d2304)` after the
   four creators passes `0x18` (24): that is the priority the main thread is meant to have while it
   polls, below every CRI thread (8, 16, 18, 25).
5. **The defect: the return value.** `ps2_syscalls::ChangeThreadPriority` (and
   `iChangeThreadPriority`) answer `KE_OK` (0) always: `changePriorityImpl`
   (`Syscalls/Thread.cpp:105-117`) calls `EeScheduler::changePriority(id, priority,
   interruptSafe, oldPriority)` — which does produce the previous priority — and then calls
   `setReturnS32(ctx, result)` with the *result*, discarding `oldPriority`. So the game's exit
   callback restores `0`: `EeScheduler::changePriority` rejects `priority < 1` with
   `KE_ILLEGAL_PRIORITY = -403` (`EeScheduler.cpp:704`), the main thread keeps the 1 the entry
   callback set, and the value 24 from `0x314FF0` is lost. Measured in the two places: the 18
   restore calls carry 0 (item 3), and the thread table shows `t1 ... prio 0/1` (item 1) while the
   24 was written earlier.
6. **Ruled out.** The creation of the four CRI threads (fixed, journal 004: the trace of the final
   build shows `CreateThread` with `initial_priority = 1` four times and `StartThread` returning
   2, 3, 4, 5). The CD layer and the RPC (journal 004: `sceCdRead`/`sceCdSync` work and the bind of
   `0x90000200` succeeds). Vertical blanks (still about 60 per second of run time). The device state
   machine `0x31B1F0`: it is never entered, and the earlier trace (`work/logs/t016e-b.log`) shows
   the device walk `0x31B820` → `0x31B618` entered once from the CRI thread body `0x314658` before
   that thread fell asleep, with 0 hits of `0x31B1F0`; the only reason it is not entered again is
   item 2.

## Cause
The runtime's `ChangeThreadPriority`/`iChangeThreadPriority` return the success code 0 instead of
the thread's previous priority. The game's CRI library saves that return value in its critical
section (`0x314320`) and restores the main thread's priority from it (`0x3143a8`), so the restore
passes 0, which the runtime rejects (`KE_ILLEGAL_PRIORITY`), and the main thread is left at the
priority 1 the critical section set. At priority 1 it outranks the four CRI threads (8, 16, 18, 25)
and the scheduler never preempts it, so the threads that drive the ADXF device state machine
(`0x31B1F0`) never run, the file state byte `handle+1` stays 2, and the poll loop at `0x331C60`
never ends. The CD read is not the problem and no I/O call is missing.

## Fix
Patch `patches/ps2recomp/0031-changepriority-return.patch`:
in `ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp`, `changePriorityImpl` (both
`ChangeThreadPriority` and `iChangeThreadPriority`) answers `oldPriority` when
`EeScheduler::changePriority` returned `KE_OK`, and the error code otherwise — the value the
scheduler already computes in its `oldPriority` reference parameter. That is what the console's
kernel answers and what the game's own code assumes: it stores the value and passes it back
unchanged. Nothing else is changed; the scheduler's own rules are untouched.

## Verification
`tests/runtime/change_priority_return_test.cpp` (new) calls the handlers the way the generated code
does and runs the game's own sequence with the values measured in the run (item 3): the calling
thread at 24, the raise to 1 with its answer kept, the restore with that answer, then an illegal
priority (0) and an unknown thread id to show that errors are still errors, and the interrupt-safe
variant. Against the tree without patch 0031 it fails:

```
FAIL: the raise answers the priority the thread had (24), got 0
FAIL: the restore answers the priority the thread had (1), got -403
FAIL: the thread ends at the priority it had (24), got 1
FAIL: the rejected call changed nothing, got 1
FAIL: iChangeThreadPriority answers the previous priority (8), got 0
change_priority_return_test: 5 check(s) failed
```

with the patch it passes (`change_priority_return_test: all checks passed`) and the other seven
executables of `tests/runtime` pass with `PS2X_WITH_RAYLIB=OFF` and `DISPLAY` unset (8 passed).
`bash tools/setup/fetch_ps2recomp.sh --reset` applies all 13 patches of the series and exits 0.

The run that shows the boot passes the wait: `bash tools/run/smoke.sh 60` with the patch changes
from `RESULT: NO_PROGRESS` to `RESULT: MISSING_BRANCH_TARGET 0x0`, and the guest thread table
(`DON_TRACE_THREADS=1`) shows `t1 ... prio 0/24` (the restore worked) with t3/t4 no longer stuck
`ready` but sleeping/waiting and being woken as their work arrives; the log goes on past the CRI
set-up to the 14th module and the game's file packages. `[don] frame N` has not appeared yet: what
stops the boot there is journal 006.

## Temporary diagnostics
`DON_TRACE_THREADS=1` in `src/game/don_overrides.cpp`: prints the scheduler's thread and semaphore
table (id, state, initial/current priority, pc, ra, entry, wait reason and id, suspend count,
wakeup count, stack and gp) every 60 vertical blanks. Off by default, no cost when off; it is the
evidence of items 1 and 5 and it stays until the boot loop is over. `tools/run/gdb_trace.sh`
(external) for the rest.
