# 004 · Stall in the ADXF directory open after the `MOVIE.AFS` search

- **Date:** 2026-10-11 (diagnosis and fix 2026-10-12)
- **Task:** T-016
- **Status:** resolved (2026-10-12: with patch
  `0031-changepriority-return.patch` the boot leaves the poll loop at `0x331C60` and goes on to the
  file packages; journal 005 and journal 006 for what stops it next)

## Symptom
`bash tools/run/smoke.sh 60` (build -O1, patches 0001-0004, journals 002 and 003 applied, a
real `MOVIE.AFS` in the host `disc/` folder) classifies as `NO_PROGRESS`: about 1,600 calls of
the vertical-blank handler `0x138EE0`, no frame-loop iteration, no log line after
`[sceCdSearchFile:ok] path="\\MOVIE.AFS;1"`. The log of the run: `work/logs/smoke-20261011-181920.log`
(in the worktree, ignored by git).

## Classification
`NO_PROGRESS`. No precedent in the catalog.

## Investigation
1. Before `MOVIE.AFS` was in the host folder the log ended with
   `sceCdSearchFile failed: \\MOVIE.AFS;1`. The runtime serves `sceCdSearchFile` from a host
   folder (`registerCdFile`, `Stubs/Helpers/Support.h`, pseudo LBNs from `0x100000`), not from
   the ISO, so the file has to exist there. Doc 01 says `MOVIE.AFS` is "not needed", which is
   true for analysis but not for the run. It was extracted from the ISO with `7z x` into
   `work/loop/movie/` and linked into the worktree's `disc/` (that folder is now a directory of
   symlinks, not one symlink). With it the search succeeds, and the stall does not change.
2. Host side at the time limit: one game thread in `FUN_00331c60` (`0x331C60`, the CRI set-up
   function of doc 16) → `FUN_00311468` → `PS2Runtime::dispatchGuestBranch` →
   `EeScheduler::checkpointDue` → `IopEmulator::runCycles`. It is spinning, not blocked.
3. `0x331C60` opens four files with `FUN_00310c90(n, name, 0, buf)` and then waits with
   `while (FUN_00311468(n) != 3) FUN_00315928();` for n = 0..3. `0x311468` is a lock/unlock
   (`0x313288`, `0x3132a0`) around the poll `0x3114A0`→`0x3114F0`; `0x315928` runs the CRI server
   step `0x3140A8`.
4. With `tools/run/gdb_trace.sh` (breakpoints on guest functions, expressions read from guest
   memory through `$rdi`): the state byte `[0x3CF078]` is **2** in every one of 7,794 polls of
   `0x311468`, `[0x3CF074]` = 0 (file index 0, the first of the four), handle `[0x3CF070]` =
   `0x3CE700`, and `[0x3D22D0]` = -1, so `0x3142E8` returns 2 and the server step is
   `0x3317E8` → `0x331610(5)`, which calls the callbacks registered in the table at
   `0x548BA0 + 5 * 0x48`. The read request `0x3125D8` → `0x3123F0` was made once (via
   `0x32E738` and `0x312140`); state 2 means "read in progress" and nothing ever completes it.
5. A breakpoint census of the guest functions in `0x300000-0x33FFFF` over 150 s (under gdb, so
   few events: 2,641) shows, besides the poll loop, `0x330D28`/`0x330C38`/`0x330C98`/`0x330D40`
   (94 each), `0x32DB70`/`0x32DB88` (37), `0x325FC0`, `0x324D38`, `0x326E08`, `0x314B80`,
   `0x314B18`, `0x330390`, `0x32F340`/`0x32F398`/`0x32F6A8`/`0x32FA30`/`0x32FA80`,
   `0x330AE8` and `0x325E58` (3 calls: the bind of service `0x90000200`). The full list is in
   `work/logs/b2-d.log`.
6. The runtime logs no `sceCdRead` and no `sceSifCallRpc` line in the whole run. The
   runtime's `sceCdSearchFile` line is the last one.
7. **Ruled out:** the `0x11A868` bind loop (journal 002); module loads (journal 003); the heap
   retry loop (finished long before); vblank delivery (the handler runs about 27 times per
   second of run time, so the vertical-blank interrupt is delivered).
8. (2026-10-12) **Who completes the read.** Measured by code reading of the four opens' path
   (`work/orchestrator/t016d/notes.md`): `0x312140` fills the *device* object and
   `0x31AF78` → `0x31AFC0` → `0x31AEC8` only sets `dev+1 = 2` ("read in progress"); **no I/O is
   issued there**. The transfer is issued by the device state machine `0x31B1F0`, reached only
   from `0x31B618` ← `0x31B820` (walk of the 40-entry device table at `0x3DC878`) ← `0x31B7F8`
   ← `0x3132E0` ← `0x315F78`, and `0x315F78` is called from the CRI thread bodies `0x314658`
   (at `0x3146E0`) and `0x314850` (at `0x314930`). The game's own vertical-blank callback
   `0x331F20` → `0x315910` → `0x3157E8` → `0x314A60` only calls `iReferThreadStatus` and
   `iWakeupThread` on the CRI thread ids; it does not step the device.
9. (2026-10-12) **The four CRI threads were never created.** `tools/run/gdb_trace.sh` with a
   breakpoint on `ps2_syscalls::CreateThread` and the expression
   `*(unsigned int*)($rdi+*(unsigned int*)($rsi+0x40)+0x14)` (the `initial_priority` field of the
   parameter block): the game calls `CreateThread` four times, from `CreateThread_0x10c0a0`, with
   `initial_priority = 0` for all four. `EeScheduler::createThread` rejects `priority < 1` with
   `KE_ILLEGAL_PRIORITY = -403` (`EeScheduler.cpp:426`), `EeScheduler::startThread` was never
   entered, and the game then used the returned `-403` (`0xFFFFFE6D`) as a thread id for
   `StartThread` (×4), `SuspendThread` and `ChangeThreadPriority`. The four thread entries
   (`0x314568`, `0x314658`, `0x314760`, `0x314850`) never ran.
10. (2026-10-12) **Where the 0 comes from.** `0x314FF0` (the CRI set-up inside `0x331C60`) reads
    the *calling* thread's status with `ReferThreadStatus(GetThreadId(), buf)` and copies
    `buf+0x18` (`current_priority`) into `DAT_003d9c18`; `0x314BE0`, `0x314D40`, `0x314DC8` and
    `0x314E50` store that value into the `initial_priority` field of the parameter block they
    pass to `CreateThread`. The runtime initialises the main thread with
    `main.currentPriority = 0` (`EeScheduler.cpp:144-145`), so the value copied is 0.
11. (2026-10-12) **What the console has.** The game's own `InitThread` (`0x10D078`) is libkernel's:
    it creates a semaphore and the `SceKerneltopThread`, starts it through `0x10D3E0`, and then
    ends with `GetThreadId()` followed by `ChangeThreadPriority(..., 1)`
    (`bash tools/re/decompile.sh 0x10D078`). The runtime replaces that whole function
    (`config/stub_policy.toml`, `InitThread = "stub"`), and the handler only returned the main
    thread's id, so the last step of the replaced function did not happen.
12. (2026-10-12) **After the fix** (`DON_TRACE_THREADS=1`, the thread table of the scheduler at
    each 60th vertical blank): `InitThread` is called once, from `0x116618` (the game's `_InitSys`);
    the four `CreateThread` calls carry `initial_priority = 1`; `StartThread` returns 2, 3, 4 and 5;
    the table shows t1 (main, running, priority 1) and t2-t5 with current priorities 8, 16, 18 and
    25. The priority-0 rejection is gone. The main thread is nevertheless still in the poll loop at
    `0x331C60` at the end of the run: see journal 005.

## Cause
The four CRI threads were never created. `InitThread` (`0x10D078`) is replaced as a whole by
`ps2_syscalls::InitThread`, which did not do what the replaced function ends with —
`ChangeThreadPriority(GetThreadId(), 1)` — so the main thread kept the priority 0 the scheduler
initialises it with; the CRI middleware read that 0 and passed it to `CreateThread`, which the
runtime rejects (`KE_ILLEGAL_PRIORITY`). With no CRI thread, nothing ever runs the device state
machine `0x31B1F0`, so the device byte `dev+1` and the file byte `handle+1` stay 2 and the wait at
`0x331C60` never ends. It is not a missing CD function and not a missing RPC: `sceCdRead` and its
`sceCdSync` polling work (the `DIR.DIR` read was served) and the bind of `0x90000200` succeeded.

## Fix
Patch `patches/ps2recomp/0030-initthread-main-priority.patch`: `ps2_syscalls::InitThread`
(`ps2xRuntime/src/lib/Kernel/Syscalls/Thread.cpp`) now sets the calling thread's priority to 1
through `EeScheduler::changePriority(id, 1, false, old)` — the same scheduler call the runtime's
own `ChangeThreadPriority` handler makes — and keeps returning `EeScheduler::kMainThreadId`.
The scheduler's own "priority 0 is illegal" rule is not touched, and no game code is changed: on
the console the main thread is at 1 when the CRI code reads it, because the game's own `InitThread`
raised it.

## Verification
`tests/runtime/init_thread_priority_test.cpp` (new): a host function as the guest on a real
PS2Runtime, on the game thread, calls `InitThread`, reads its own priority with
`GetThreadId` + `ReferThreadStatus`, builds the parameter block the game builds (priority = the
value read) and calls `CreateThread` + `StartThread`. Against the tree without patch 0030 it fails
(`priority read 0`, `CreateThread -403`, the entry never runs); with the patch it passes
(`priority read 1, thread id 2, entry runs 1`), and the other six executables of `tests/runtime`
pass with `PS2X_WITH_RAYLIB=OFF` and `DISPLAY` unset. `bash tools/setup/fetch_ps2recomp.sh --reset`
applies all 12 patches of the series and exits 0.

What the run shows (this is not the success the "Next step" below asked for):
`bash tools/run/smoke.sh 60` three times after the patch, with the game: `RESULT: NO_PROGRESS
[don] vsync 840`, `... vsync 1200`, `... vsync 1320`, `frames=0` in all three; the log still ends
with the poll loop and no `[don] frame` line. The GDB trace of item 12 shows the four CRI threads
created and started, so the fix removed the cause of *this* entry, and the same wait continued for
the second, independent cause of journal 005 (the main thread's priority is left at 1 by a rejected
critical-section restore, and it starves the CRI threads).

Resolved (2026-10-12, journal 005's fix). With `0031-changepriority-return.patch` the wait at
`0x331C60` is passed: `bash tools/run/smoke.sh 60` changes from `NO_PROGRESS` to
`MISSING_BRANCH_TARGET 0x0` and the log goes on past the CRI set-up — the ADXF device state machine
runs (`tools/run/gdb_trace.sh`: `sceCdSearchFile` called from the device path
`0x31b820 → 0x31b618 → 0x3223f8 → 0x3265a0 → 0x326188`), the 5 searches (DIR.DIR and the four AFS
archives from DIR.DIR) succeed, the 14th module (`sndfi`) loads, and the game starts opening its
file packages (`cmn/1000.txd`, `cmn/1000.dff`). The device step `0x31B1F0` that journal 004's
investigation was looking for is no longer needed to explain anything: the transfer completes and
the poll loop returns 3. What stops the boot there is journal 006.

## Next step
Journal 005 (resolved): the ADXF poll loop left the main thread at priority 1 because a critical
section's restore was rejected. Journal 006: the boot now stops in the RenderWare/CRI file packages.

## Temporary diagnostics
`tools/run/gdb_trace.sh` runs (external, reusable). `DON_TRACE_THREADS=1` in
`src/game/don_overrides.cpp` prints the scheduler's thread and semaphore table every 60 vertical
blanks (off by default, no cost when off); it is the evidence of items 12 and of journal 005 and
stays until the boot loop is over. The extracted `MOVIE.AFS` and the symlink folder are in `work/`
(ignored).
