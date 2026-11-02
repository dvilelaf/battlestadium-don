# Stage 1 tasks

Work list split so that each piece can be done by an agent with little context.
The general plan is in [PLAN.md](PLAN.md); the status of each task, in [STATUS.md](STATUS.md).

## How it is used

1. The orchestrator picks a task whose dependencies are done (see [STATUS.md](STATUS.md)).
2. It launches an agent of the indicated model and gives it **only**: the task card, `AGENTS.md` and
   the documents the card cites under "Read first".
3. The agent works, runs the verification and returns the report (format in `AGENTS.md`).
4. The orchestrator reviews the report, repeats the verification if the task touches code that
   others will use, and updates `STATUS.md`.

Agents do not edit `PLAN.md`, `TASKS.md`, `STATUS.md` or `DECISIONS.md`.

## Task cards by phase

| File | Tasks | Milestone |
|---|---|---|
| [tasks/F0-foundations.md](tasks/F0-foundations.md) | T-001 … T-028 | Phase 0: environment, function map, clean recompilation, first boot |
| [tasks/M1-menu.md](tasks/M1-menu.md) | T-101 … T-121 | Menu visible: IOP modules, PCSX2 GS, host program |
| [tasks/M2-match.md](tasks/M2-match.md) | T-201 … T-209 | Match at 60 fps: VU1, performance, coverage |
| [tasks/M3-playable.md](tasks/M3-playable.md) | T-301 … T-321 | Sound, videos, memory card, 4 players, English |
| [tasks/M4-delivery.md](tasks/M4-delivery.md) | T-400 … T-408 | Windows, one-command build, documentation |

## Task card format

```
### T-NNN · Title
Model · Depends on · Time box
Read first:    documents and files that give the necessary context
Input:         what must exist before starting
Output:        files the task creates or modifies
Steps:         what to do, with exact commands when they are known
Done when:     verification command and expected output
Limits:        what not to touch and when to stop
```

**Time box** is the agent's working time after which it must stop and return
a blocker report instead of continuing to insist.

## Which model for what

| Label | When |
|---|---|
| **Haiku** | The card gives the complete specification: inputs, outputs, behavior and verification. No third-party code needs to be read |
| **Sonnet** | Third-party code must be read, integrated into CMake, a build debugged, or a known patch ported |
| **Sonnet + review** | Like Sonnet, but many tasks use the result; the orchestrator reviews it before accepting it |
| **Orchestrator** | Why the game behaves a certain way must be diagnosed, or alternatives decided between |
| **Human** | Requires the user: their BIOS, their gamepads, a Windows machine or a judgment on how it looks or sounds |

## Level of detail

The phase 0 cards are specified down to the command: what they describe was tried
on this machine or follows directly from what was tried. The ones from M1 onward fix scope,
inputs, outputs and verification criterion, but their internal steps depend on what comes out
of the earlier phases. **When each milestone closes, the orchestrator reviews and makes concrete the cards of the
next one.** Where a card says "investigate", it is a research task with a written
deliverable, not a gap.

## Order of attack

```
Wave 1 (no dependencies, in parallel):
  T-001  T-002  T-004  T-005  T-007  T-019  T-022  T-101  T-103  T-109  T-115  T-024

Wave 2:
  T-003 ◀ T-002        T-006 ◀ T-005, T-002        T-104 ◀ T-103        T-017 ◀ T-024

Wave 3:
  T-008 ◀ T-006        T-023 ◀ T-006        T-020, T-021 ◀ T-003        T-105 ◀ T-104        T-018 ◀ T-017

Wave 4:
  T-009 ◀ T-008        T-010 ◀ T-008        T-106 ◀ T-105

Wave 5:
  T-025 ◀ T-009, T-010 (added on 2026-10-11)        T-107 ◀ T-106, T-017
  T-011 ◀ T-025, T-020, T-021        T-026 ◀ T-025 (added on 2026-10-11)

Wave 6:
  T-012 ◀ T-011, T-026  ─▶  T-013      T-014 ◀ T-012  ─▶  T-015  ─▶  T-016 (boot loop)

Phase 0 gate  ─▶  M1: T-102, T-108 … T-121  ─▶  T-114 (loop up to the menu)
```

## Unblocking loops

Four tasks (T-016, T-114, T-202, T-304) are loops: the cycle is repeated until the
milestone criterion is reached. Each iteration:

1. **Run** (Haiku): launch the smoke command and save the log.
2. **Classify** (Haiku): `tools/run/classify_log.py` gives the blocker category.
3. **Look for precedent** (Haiku): check the catalog in
   [knowledge/08-external-references.md](knowledge/08-external-references.md).
4. **Diagnose and decide** (orchestrator): cause and fix. An entry is written in
   `docs/journal/`.
5. **Apply** (Sonnet): the fix as a patch or as a setting in `src/game/`, with its test.
6. **Go back to 1.**

If the same blocker uses up more than three iterations, the orchestrator escalates it to the user with the
journal entry.
