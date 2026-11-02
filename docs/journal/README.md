# Journal

One entry per investigated blocker or per technical decision made during an unblocking loop.
It exists so that nobody investigates the same thing twice.

## File name

`NNN-short-description.md`, with `NNN` a consecutive three-digit number: `001-iop-reset.md`.

## Template

```markdown
# NNN · Short title

- **Date:** YYYY-MM-DD
- **Task:** T-NNN
- **Status:** open | resolved | parked

## Symptom
What is observed, with the exact command that reproduces it and the relevant lines of the log.

## Classification
Category from `tools/run/classify_log.py` and, if there is a precedent, the row of the catalog
in `docs/knowledge/08-external-references.md`.

## Investigation
What was looked at, in order, and what was ruled out. Addresses, functions, short fragments.

## Cause
The real cause, or "not determined".

## Fix
What was changed (patch, file, lines) and why that fixes the cause and not just the symptom.

## Verification
Command and output that show it is fixed and that nothing earlier was broken.

## Temporary diagnostics
Which traces or shortcuts were added to investigate and whether they have been removed.
```

## Signal for "the game is in its frame loop"

Set in T-015. The runtime has no counter of presented frames that the game's loop drives, and
the game's loop (`0x131CA0`) has no blocking wait for the vertical blank (boot map, section 8),
so one counter is not enough: without a delivered vblank the loop spins at full speed. Two
counters are printed by `src/game/don_overrides.cpp`, installed only when `DON_TRACE_FRAMES=1`
(`tools/run/smoke.sh` sets it); each wraps the recompiled function and calls it:

| Log line | Counts | Wrapped function |
|---|---|---|
| `[don] frame <n>` | iterations of the frame loop (calls of the per-frame counter reader) | `0x138B00` |
| `[don] vsync <n>` | calls of the game's vertical-blank interrupt handler | `0x138EE0` |

Both print every 60 calls. `tools/run/classify_log.py` reads them:

| Result | Condition |
|---|---|
| `OK_VSYNC_LOOP <frames> <vsyncs>` | largest `frame` and largest `vsync` are both at least 300 |
| `FRAME_LOOP_UNPACED <frames>` | frames reach 300 but vsyncs do not: the loop runs and no vblank reaches the game |
| `NO_PROGRESS <detail>` | otherwise; the detail is the last line that looks like progress |

The categories that come before these (`MISSING_FUNCTION`, `MISSING_BRANCH_TARGET`,
`UNIMPLEMENTED_SYSCALL`, `IOP_UNHANDLED`, `IOP_ERROR`, `STARTUP_FAILED`, `CRASH`) are listed with
the runtime message each one matches at the top of `classify_log.py`. Only `OK_VSYNC_LOOP`
exits with 0. `smoke.sh` appends `[smoke] exit status <n>` to the log (124 is the timeout and is
normal; a status of 128 or more other than 137, 141 and 143 is a `CRASH`).

## Rules

- Whole decompiled functions or dumps are not pasted here (see `AGENTS.md`, rule 1).
- An entry is not deleted. If the cause turned out to be another, a "Correction" section is added.
