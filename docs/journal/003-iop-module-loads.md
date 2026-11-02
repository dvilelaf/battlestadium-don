# 003 · IOP module loads from a buffer, seen in a real run (patch 0004)

- **Date:** 2026-10-11
- **Task:** T-016
- **Status:** resolved

## Symptom
Not a failure of a run: patch `0004-iop-load-module-buffer-memory.patch` was proven only by a
unit test (`tests/runtime/module_buffer_load_test.cpp`). Without it every
`sceSifLoadModuleBuffer` read the ELF from EE RAM instead of IOP RAM and returned -1
(docs/knowledge/14, section B), so the game's loop at `0x136708` would never end.

## Classification
Not a classifier category (the run did not fail here). Catalog precedent: none.

## Investigation
Run of `bash tools/run/smoke.sh 60` (build -O1, patches 0001-0004, journal 002 fix applied).
Log lines, in order (`work/logs/smoke-20261011-181920.log`):

| id | pack, position | `loaded IRX` line | start | Start output seen |
|---|---|---|---|---|
| 1 | 0000, 1 (`sio2man`) | entry=0x10634 base=0x10000 | 0 | none |
| 2 | 0000, 2 (`mtapman`) | entry=0x11b24 base=0x11000 | 0 | `[IOP Kprintf] yet sif hasn't been init` (printed by the module, after its load) |
| 3 | 0000, 3 (`mcman`) | entry=0x13078 base=0x12f00 | 2 | none |
| 4 | 0000, 4 (`mcserv`) | entry=0x3a340 base=0x3a300 | 2 | none |
| 5 | 0000, 5 (`padman`) | entry=0x40f04 base=0x3db00 | 0 | none |
| 6 | 0000, 6 (`cdvdstm`) | entry=0x48fdc base=0x47a00 | 2 | none |
| 7 | 0000, 7 (`libsd`) | entry=0x4c6b0 base=0x4c600 | 0 | none |
| 8 | 1000, 1 (`libsd` again) | entry=0x520b0 base=0x52000 | 0 | none |
| 9 | 1000, 2 (`modhsyn`) | entry=0x57a00 base=0x57a00 | 0 | none |
| 10 | 1000, 3 (`modmidi`) | entry=0x68000 base=0x68000 | 0 | none |
| 11 | 1000, 4 (`modmsin`) | entry=0x6bf00 base=0x6bf00 | 0 | none |
| 12 | 1000, 5 (`sdrdrv`) | entry=0x6c6a0 base=0x6c300 | 2 | `SDR driver version 4.0.1`, `Exit rsd_main` |
| 13 | 1000, 6 (`cri_adxi`) | entry=0x6e57c base=0x6e000 | 2 | `CRI ADX Driver Ver.9.31`, four thread priorities, SPU core number, `sceSdInit(SD_INIT_COLD) call.` |

(The module name per position comes from doc 14 section C and the load order of doc 16; the
log itself names modules only by `iopbuf:fnv64:<hash>`.) Every load logged
`[SIF module] load-buffer-emulated id=N ref=1`, so the handler returned a positive id for all
13. The `start` column is the value printed by the loader (`start=0` / `start=2`).

## Cause
Patch 0004 works in a real run: the ELF is read from IOP RAM, and all 13 loads of the two
module packs succeed. The loop at `0x136708` ended.

The documents speak of 14 loads. The 14th is `sndfi`, which the game loads later from
`0x37A460` (doc 16 section 10.4), not in the two packs; this run had not reached it.

## Fix
Patch 0004 (already in the repository, applied to the PS2Recomp worktree). Nothing new.

## Verification
The log above. Evidence that the start functions finished: the printed output of `sdrdrv` and
`cri_adxi` comes from their start functions and appears before their `loaded IRX` line;
the next module is loaded after each. For `libsd`, `modhsyn`, `modmidi`, `modmsin` the start
produced no output, so "finished within the 2M-instruction budget" is inferred only from the
loader logging success and the game going on, not measured (no budget-exhaustion message
appeared in the log).

## Temporary diagnostics
None.
