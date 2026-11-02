# 002 · First run stalls in an endless SifBindRpc retry loop

- **Date:** 2026-10-11
- **Task:** T-016
- **Status:** resolved

## Symptom
`bash tools/run/smoke.sh 60` ends with `NO_PROGRESS`: no frame-loop counters move, no error.
The runtime's own logs (`[SifInitRpc] Initialized`...) are missing from the captured log.

## Classification
`NO_PROGRESS` (`tools/run/classify_log.py`). Not listed in section 9 of
`docs/knowledge/16-boot-sequence.md`; it is a new failure on step 9 (`modules/0000.fpk`).

## Investigation
1. **Missing runtime logs.** There is no run-time log switch (`getenv` appears nowhere in
   `ps2xRuntime`/`ps2xIOP`). `RUNTIME_LOG` (`include/ps2_log.h:119-137`) is compile-time
   (`PS2_RUNTIME_LOGS`) and writes to `std::cout`, block-buffered when piped, so `timeout`
   kills the process before the lines are flushed. With `stdbuf -o0 -e0` they appear.
2. **Guest sequence** (traced with `tools/run/gdb_trace.sh`): `entry` → `_InitSys 0x116618`
   → `main` → `0x1365C0` (2015 `EndOfHeap` calls, terminates, as predicted in boot map
   section 9 item 1) → `0x136770` (Dev/Gs/Dma/Sif/Cd init, `sceSifRebootIop`, `sceGsSyncV`,
   which blocks and wakes: vblank delivery works) → `0x133070` (`sceCdMmode`, `sceSifInitIopHeap`)
   → `0x1345B0("modules/0000.fpk")` → `0x11A4D0` (4 semaphores) → `0x11A7A8`.
3. **Where it stops.** `0x11A7A8` (named `sceCdLayerSearchFile` by the analyzer; reached via the
   unnamed wrapper `0x11AAA8`) loops
   `while (sceSifBindRpc(0x4d2640, 0x80000597, 0) < 0 || client->server == 0) { ~1M-iteration delay }`.
   Measured: 514+ `SifBindRpc(client 0x4d2640, sid 0x80000597)` from return address `0x11a87c`
   (loop address `0x11A868`) within the time limit. No `CreateThread`, `StartThread` or
   `SifRegisterRpc` is ever called by the game.
4. **Not "all threads blocked".** There is one guest thread and it is spinning, not blocked.
   The scheduler idle state seen in a backtrace (`processDueDeadlines` waiting on
   `m_eventCv` for the next vblank deadline, `EeScheduler.cpp:1776-1815`) is the host side
   pacing vblank, not a guest wait. Native CPU use of `GameThread` was **not measured**
   (the sampling run was rejected); the evidence is the bind-call count.
5. **Why the bind never succeeds.** `SifBindRpc` (`Kernel/Syscalls/RPC.cpp:254-340`) publishes a
   server only if one is registered or `PS2IopTransport::canBindRpc` is true
   (`ps2_iop_transport.h:31`, `iop_subsystem.cpp:188-194`). Otherwise `client->server = 0`.
   No IOP module serves the cdvdman SIDs (BIOS cdvdman; `config/stub_policy.toml` CD/DVD
   comment), so `0x80000597` is never bindable.
6. **Why it was not stubbed.** All other `sceCd*` functions are stubbed. This one was missed
   because the analyzer named `0x11A7A8` and never identified its wrapper `0x11AAA8`; the
   runtime has no handler named `sceCdLayerSearchFile`. The existing handler is
   `ps2_stubs::sceCdSearchFile` (`Kernel/Stubs/CD.cpp:545`: `a0` = file struct, `a1` = path,
   returns 1/0).

## Cause
The recompiled game code of `0x11A7A8` binds the cdvdman RPC service, which the emulated IOP
does not provide. The SDK's bind-retry loop has no timeout, so the game spins forever on
the first file search.

## Fix
`src/game/don_overrides.cpp`: a table `kHandlerBindings` (address, SDK function, handler,
journal entry) at the top of the file, applied in `applyDon()` through
`ps2_game_overrides::bindAddressHandler(runtime, 0x0011A7A8, "sceCdSearchFile")`.
Binding `0x11A7A8` covers both callers (`0x11AAA8` wrapper and `0x326188`). Argument fit
checked in doc 16 section 10.3: the handler reads `a0` (file struct) and `a1` (path) and
ignores `a2`; the second caller's layer variable `0x3DF0BC` stays 0.
`tools/run/smoke.sh` now runs the game with `stdbuf -o0 -e0` so the runtime's log (block-buffered
`std::cout`) survives the time limit, and keeps both streams in order in one file.

## Verification
`bash tools/run/smoke.sh 60` on the -O1 build with the fix: the log shows
`[sceCdSearchFile] pc=0x11a7a8 ra=0x11aab8 ... path="\MODULES\0000.FPK;1"` followed by
`[sceCdSearchFile:ok] ... lsn=0x100000 size=0x1c170 sectors=0x39`; 11 successful searches in
total (both packs, `DIR.DIR`, the four AFS) and the first module load follows. The loop at
`0x11A868` is left. See journal 003 for what comes after.

## Temporary diagnostics
Only `tools/run/gdb_trace.sh`, a reusable external tracer. Nothing was added to the code.
