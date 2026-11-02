# 062 · Old vs new Windows profile, symbol by symbol: the extra 0.64 ms per blank

- **Date:** 2026-10-28
- **Task:** T-766
- **Status:** open (cause located to the EE scheduler hot path; fix proposed, not done)

## Symptom
Two exes built the same way (static, Release `/O2`, no LTO, noffmpeg, T-759 sampler; journal 060), A = 63fc1b6 and
B = main, profiled on the owner's PC over the same ougi fight (`DON_PROFILE`, `DON_PACING_LOG=6000`, 130 s):

```
A: work 14.73 ms/blank + renderer 0.01 + pacing wait 1.94   68654 samples   cpu_us 75156250
B: work 15.37 ms/blank + renderer 0.01 + pacing wait 1.30   68306 samples   cpu_us 80906250
```
B does +0.64 ms of work per blank (journal 059 saw +0.67).

## Classification
Performance, not a correctness failure (same state hash in journals 055-058).

## Investigation
1. `tools/verify/profile_diff.py` (new, with `tests/test_profile_diff.py`) symbolizes both profiles (exe via its link
   map, `$...` labels folded into the function before them; system DLLs via the export table of a copy of the DLL, nearest
   export below) and prints ms per blank per symbol: samples x (cpu_us / samples_taken) / blanks. Blanks = 7857
   (duration 131.07 s x 59.94 Hz; work + wait is 16.68 ms in both runs, which is one blank). Command:
   `uv run --no-project --with pefile python -I tools/verify/profile_diff.py A.profile A.map B.profile B.map --winsys DIR`.
   CPU total per blank 9.566 ms (A) vs 10.297 ms (B): **+0.732**. The ntdll wait symbol (`ZwWaitForAlertByThreadId`,
   3.4 ms) is the sampler's parking bias (journal 059) and went down by 0.11; ignore it.
2. Top deltas (ms per blank, B - A):

   | symbol | A | B | delta |
   |---|---|---|---|
   | `EeScheduler::run` (incl. its `$ehgcr_*` labels) | 1.312 | 1.617 | +0.306 |
   | `EeScheduler::checkpointDue` (out of line; absent in A) | 0.000 | 0.272 | +0.272 |
   | `EeScheduler::accountCycles` | 0.468 | 0.634 | +0.166 |
   | `entry_002d47b8` + `entry_002d4790` + `entry_002d47c8` (generated game code) | 0.241 | 0.368 | +0.127 |
   | `don_diag::dumpState` / `donVblankHandler` | 0.039 | 0.041 | +0.002 (a split: A inlined it) |
   | `PS2Memory::processVIF1Data` | 0.343 | 0.376 | +0.032 |
   | VCRUNTIME140 (nearest export `__telemetry_main_return_trigger` = the memmove helper, journal 061) | 0.392 | 0.425 | +0.033 |
   | `PS2Runtime::dispatchGuestBranch` | 0.337 | 0.006 | -0.332 |
   | `PS2Runtime::eeCheckpointDue` | 0.043 | 0.004 | -0.040 |

   Full table: `work/t766/diff.txt` (top 25 and 12 largest decreases; `diff_full.txt` has 400 rows; not versioned).
3. **The big memmove did not grow**: 2817 samples in A, 2820 in B (+3). It is not the delta; the journal 061 copy exists
   in both builds.
4. The scheduler hot path moved and grew. A's guest code called `PS2Runtime::dispatchGuestBranch` (a runtime function with
   the checkpoint inlined: 0.337 ms). B's guest code (`entry_002d47b8`, disassembled) has `dispatchGuestBranch` inlined
   and calls `EeScheduler::checkpointDue` directly (`call 0x14000b2a0`, a 0xf0-byte copy at the front of the exe), which
   calls `EeScheduler::accountCycles`. Group sum (dispatchGuestBranch + eeCheckpointDue + checkpointDue + accountCycles
   + run): A 2.160 ms, B 2.533 ms, **+0.373 ms**, about half of the gap. The rest is spread: generated entries +0.127,
   `processVIF1Data` +0.032, memmove +0.033, don_spu2.dll +0.05, small items.
5. `accountCycles` is the same size in both (0x150 bytes) but B has more instructions (85 vs 75) and a pointer chain:
   every field is reached through `m_hot` (`mov 0x15d0(%rsi),%rax`, reloaded about six times) and the memory timer through
   `m_runtime.memory().m_hot` (`mov 0x6e8(%rbx),%rax`, reloaded five times) because the stores through those pointers may
   alias them. A reads plain members of the scheduler. The samples inside the functions are flat (351-387 each on the
   top ten instructions of `accountCycles`), so there is no single slow instruction; it is the instruction count and
   dependent loads on a path run once per guest dispatch.
6. Source: patch `0087-don-hot-state.patch` (commit 17f2e0c, T-605) moved `eeCycle`, `nextFastPathCycle`,
   `iopPendingEeCycles`, `dispatchCount`, the EE timer fields and others from `EeScheduler`/`PS2Memory` members into the
   separate `DonHotState` reached by pointer. It is the only runtime patch touching `ee_scheduler.h` since 63fc1b6 besides
   the function-table patch 0088. Its header says "no behaviour change"; the cost was not measured in that card.

## Cause
Not proven by an experiment. Best-supported: the `DonHotState` indirection of patch 0087 (extra dependent loads and
reloads in `accountCycles`/`checkpointDue`/`run`) plus the different inlining (`checkpointDue` called out of line from
the generated code instead of an inlined `dispatchGuestBranch`). It explains about +0.37 of the +0.73 ms; the remainder
(generated-code entries, VIF1, memmove, SPU2) is spread thinner and was not attributed.

## Fix
Proposed, not done. In a new runtime patch (`patches/ps2recomp/`, file `ps2xRuntime/include/runtime/ee_scheduler.h`):
read `DonHotState *h = m_hot;` and the memory-timer state pointer once at the top of `accountCycles` and
`checkpointDue`, keep the fields in locals across the body and store them at the end (same statements, same order, no
behaviour change, so the RAM-hash lines of `fight_practice` must stay identical), and mark both `inline` + force-inline
at the call sites in `run` and in the generated code's path so `checkpointDue` is not a separate call. Expected saving:
up to the group's +0.37 ms per blank (about 3 fps at 57 fps), to be confirmed with `tools/run/windows/fight_compare.ps1`
against 63fc1b6; the other +0.36 ms is not covered.

## Verification
```
uv run --no-project --with pytest python -I -m pytest tests/test_profile_diff.py -q    # 4 passed
uv run --no-project --with pefile python -I tools/verify/profile_diff.py <A.profile> <A.map> <B.profile> <B.map> --winsys <dir> --top 25
```
Inputs (profiles, exes, maps, system DLL copies) stay under `work/m7-inputs/` and are not versioned.

## Temporary diagnostics
None in the product code. Scratch scripts in `work/t766/` only.
