# Rules for working in this repository

For agents and for people. Read it all before the first task.

## What this is

A PC port of *Battle Stadium D.O.N* (PS2, `SLPS-25675`), "DON" for short, made by static
recompilation. The repository contains tools, patches, original code and documentation.
**It does not contain the game.**

Where everything is: [docs/PLAN.md](docs/PLAN.md) (section 2). What is known about the game:
[docs/knowledge/](docs/knowledge/README.md). What has to be done: [docs/TASKS.md](docs/TASKS.md).

## Rule 1: nothing from the game goes into git

Never add the following to the repository, and never paste it in a report, an issue or an
external service:

- disc images, the executable, `.IRX` modules, data files (`.afs`, `.fpk`, textures, models,
  sound, video);
- the C++ that the recompiler generates (`generated/`);
- memory dumps, savestates and GS dumps from an emulator;
- whole decompiled or disassembled functions.

These can be versioned: addresses, sizes, hashes, file names, descriptions of formats, and
short fragments of disassembly or decompiled C when they are needed to explain a diagnosis.

The folders `disc/`, `en_patch/`, `gc_rvz/`, `generated/`, `work/`, `build/` and `third_party/`
are in `.gitignore`. Before any `git add`, run `git status --short` and check that none of the
above appears. Do not use `git add -f`.

## Rule 2: nothing is done until its verification has been run

Each task has a "Done when" section with a command. The task is done when that command has
been run **after the last change** and its output is in the report. If the output is not the
expected one, the task is not done: report what came out.

Do not declare "it works" or "playable" because a frame that is not black was seen. Say
exactly what was run and what was observed.

## Rule 3: stop in time

Stop and return a blocker report, instead of continuing to try things, when:

- the time box of the task card is reached;
- the task card says so in "Limits";
- the solution that works requires going outside the scope of the task card (touching files it
  does not list, changing a decision in [docs/DECISIONS.md](docs/DECISIONS.md), installing
  something on the system);
- something in the task card turns out to be false (a file that does not exist, a command that
  does not do what it says).

A clear blocker report is worth more than a forced solution.

## Rule 4: do not fix a symptom by hiding it

Forbidden as a solution: making a function "return success" without knowing what it does,
nulling out instructions so that something stops failing, lowering the threshold of a
verification, disabling a test. They are allowed as a **temporary diagnostic**, noted in the
journal and removed before closing the task.

## How to run things

- Everything from the repository root.
- **Python always with `uv`**, never plain `python` or `python3` (a hook rejects it):

  ```bash
  uv run --no-project python -I path/script.py arguments
  uv run --no-project --with rabbitizer --with pytest --with pillow --with numpy --with scikit-image python -I -m pytest tests -q
  ```

  `-I` isolates the interpreter; that is why scripts that import others from their folder
  start with `sys.path.insert(0, str(Path(__file__).resolve().parent))`.
- No `sudo`. Nothing is installed on the system; dependencies go in `third_party/` or in the
  container in `tools/container/`.
- The game files are untrusted data: they are read, not executed; they are extracted into
  their own folder (`disc/`...), never mixed with scripts.
- Paths that belong to one machine (the disc image, the BIOS folder for the reference
  emulator) are never written in versioned files. They are environment variables
  (`DON_ISO`, `DON_PS2_BIOS_DIR`) set in `local.env` at the repository root, which git
  ignores; `local.env.example` lists them. Load them with `set -a; . ./local.env; set +a`.
- Frequently used constants (hashes, pinned commits, file names): in
  [docs/knowledge/01-game-and-discs.md](docs/knowledge/01-game-and-discs.md) and
  [docs/knowledge/09-environment-and-tools.md](docs/knowledge/09-environment-and-tools.md).

## Where each change goes

| I want to change... | It goes in... |
|---|---|
| The runtime, the recompiler or the IOP emulator of PS2Recomp | A patch in `patches/ps2recomp/` |
| Something in PCSX2 | A patch in `patches/pcsx2/` |
| The behavior of a specific game function | `src/game/don_overrides.cpp` |
| Which functions are recompiled or replaced | `config/` (never by editing `generated/`) |
| Window, gamepads, audio, options | `src/host/` |
| Glue with the GS, microVU or SPU2 of PCSX2 | `src/gsbridge/`, `src/vubridge/`, `src/spu2bridge/` |

`generated/` and `third_party/` are **not edited by hand** as a final result.

### How to create a patch

1. Edit the files inside `third_party/PS2Recomp` (or `third_party/pcsx2`).
2. Generate the patch with only the files of this change:
   `git -C third_party/PS2Recomp diff -- <files> > patches/ps2recomp/NNNN-short-description.patch`
   (`NNNN` = next free number, four digits).
3. Add its name at the end of `patches/ps2recomp/series`.
4. Check that everything applies from clean: `bash tools/setup/fetch_ps2recomp.sh --reset`.
5. One patch per topic. Comment header with: what it fixes, how it was verified and, if it is
   based on the work of another project, which one (repository, commit, file) and its license.

## Portability (Windows and Linux)

In `src/` and `tools/`:

- paths with `std::filesystem` (C++) or `pathlib` (Python); no absolute path written in the code;
- no operating system API outside a clearly separated per-platform file;
- fixed-size types (`uint32_t`...) for everything that represents game memory;
- the build scripts that the end user needs exist in Python or have a Windows equivalent.

## Build options that are not touched

`-ffp-contract=off`, no `-ffast-math` or equivalents, `-march=x86-64-v3`. The floating point
of the game depends on it (see the catalog in
[docs/knowledge/08-external-references.md](docs/knowledge/08-external-references.md)).

This applies to the game: the generated code, the PS2Recomp runtime, `src/game/` and
`src/host/`. The PCSX2 pieces behind the bridges (GS, microVU, SPU2) are built with PCSX2's own
flags, unchanged.

## Diagnostics

- Controlled by environment variables with the prefix `DON_` (`DON_TRACE_MODULES=1`...), off
  by default and with no cost when off.
- Messages with a prefix in square brackets: `[don]`, `[don-gs]`, `[don-vu]`, `[don-spu2]`.
- A diagnostic that is no longer needed is removed.

## Journal

Each investigated blocker leaves an entry in `docs/journal/` following
[docs/journal/README.md](docs/journal/README.md). If an investigation turns an "unverified"
fact in `docs/knowledge/` into a measured fact, that document is updated. Findings are not
deleted: if they turn out to be false, they are corrected and the correction is recorded.

## Isolation (owner's rules, 2026-10-23)

- **Never the owner's real user folder.** Every run of the game by an agent sets `DON_USER_DIR` to a temporary
  folder (or to a copy of the real one when a card asks for the owner's history), so the owner's saves, settings and
  log are never touched.
- **One git worktree per agent.** Each card runs in `work/wt/<card>` on branch `agent/<card>`, made from `main`.
  Cards of the same sequential chain share one worktree. In it:
  - `third_party/` and `generated/` are symlinks to the main checkout's;
  - the agent commits only on its own branch, never on `main`;
  - before reporting, it runs `git merge main` and resolves its own conflicts;
  - the orchestrator merges the branch into `main` after review, then removes the worktree.

## Token economy

Tokens are the budget. For every task:

- One new agent per task card, with a short prompt (the card's ID, its file, the deadline); a session is resumed only
  for one short follow-up right after its own report.
- Read only what the card lists, by line ranges; never print a file of `generated/` or a whole log (write long output
  to a file and read `tail` or `grep` of it).
- Reports of at most 25 lines; details in `work/<task>/NOTES.md`.
- Haiku for mechanical work, runs and docs; Sonnet for code and for reviews. Never Opus as a subagent (too expensive).
- No duplicated checks: whoever reviews re-runs only the "Done when" command; the full regression suite runs once per
  integration batch.

## Report when finishing a task

Plain text, with these sections and nothing else:

```
TASK: T-NNN · title
RESULT: done | blocked | partial
FILES: list of files created or modified
VERIFICATION: the "Done when" command and its literal output (trimmed if long)
NOTES: what the next person needs to know: surprises, measured data, deviations from the task card
BLOCKER: (only if not done) what prevents it, what was tried, what would be needed
```

Agents do not edit `docs/PLAN.md`, `docs/TASKS.md`, `docs/tasks/`, `docs/STATUS.md` or
`docs/DECISIONS.md`; they propose the changes in "Notes".

## Git

- Do not `commit` or `push` unless whoever assigns the task asks for it explicitly. In a per-agent worktree
  ("Isolation"), commits on the agent's own branch are expected; never on `main`.
- Since 2026-10-11 the work is versioned as it goes: the orchestrator commits each task after
  reviewing it (one commit per task, or one for several tasks that share files). Agents that
  execute a task still do not commit. Nothing is pushed until the owner asks for it.
- Commit messages in English, starting with the task identifier: `T-004: add disc verification`.
- Never rewrite published history.

## Language

Everything in the repository is in English: documentation, file names, code, comments, program messages and commit messages.

## License

The repository is GPL-3.0 (it links PS2Recomp and PCSX2). Code taken or adapted from other
projects keeps its notice and its origin is noted.
