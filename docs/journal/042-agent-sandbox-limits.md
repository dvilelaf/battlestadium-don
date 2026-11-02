# 042 · Can an agent sandbox run the game?

- **Date:** 2026-10-24
- **Task:** P-run
- **Status:** resolved

## Symptom
It was not known whether the Gate 2 cards, which compare state hashes and frame times, could run in an agent
sandbox: a first probe's two game runs failed (exit 2, the program said the ELF `.../en_patch/` does not exist). The
cause was a card error: the command passed the folder `en_patch/` where the program expects the ELF file
`disc/SLPS_256.75`. (Correction by the orchestrator, 2026-10-24: the first version of this entry said three cards
could not start because of these runs; they could not start because of an unrelated agent-binding defect.)

## Classification
`tools/run/classify_log.py`: `OK_VSYNC_LOOP`.

## Investigation
Measured by the earlier probe (not re-measured): Docker works (`bash tools/container/run.sh true` exits 0); no
`clang`, `clang++`, `clang-17` or `ld.lld` on `PATH`; `/dev/dri` has `card0` and no render node; `libvulkan_dzn.so`
fails to load; 16 cores; ~25 GB free; `cmake` and a C++ compiler work (setup tests 2/2); the main checkout's
`generated/`, `third_party/`, `disc/`, `en_patch/` and `build/linux-dev/don` are readable.

This run, in the git worktree `agent/p-run`:
- The clone had none of `disc/`, `third_party/`, `generated/`, `en_patch/`, `build/linux-dev`. I made symlinks to the
  main checkout (AGENTS.md "Isolation"): the four folders at the root and `build/linux-dev`. They are not committed.
- Tools: `g++`, `cmake`, `ninja`, `uv`, `xvfb-run` and `docker` are all present.
- Run with the project's own script (no hand command needed, `uv` and `DON_ISO` did not get in the way):
  `DON_ISO=/nonexistent DON_USER_DIR=$PWD/work/prun-user bash tools/run/smoke.sh 20`. Exit code 0.
- The log started with `[don] cmd build/linux-dev/don --headless --sound off --seconds 20 disc/SLPS_256.75`,
  `[don] cpu AMD Ryzen 7 7700X 8-Core Processor (16 logical cores)`, `[don] ram 31195 MiB`.
- The log ended with `[don] exit: --seconds elapsed, code 0 (20.5s, 1200 frames, worst 10s window 60.0 fps)`.
- Verdict of `classify_log.py`: `RESULT: OK_VSYNC_LOOP 1020 1140`, `frames=1020 vsyncs=1140`. Milestones seen: ELF
  loaded, overrides applied, `sceCdSearchFile` ok (x11), modules loaded by the game (x14), `sceDmaSend`, frame loop
  counter, vsync counter. Not seen in that short run: IOP reset, `sceCdRead`, thread created, RPC bind, GS vsync
  callback, game text output. No warning or error lines.
- Steps 3 (`--renderer software` / `vulkan`) were not needed because the run reached the frame loop. The run was
  `--headless`, so no GPU renderer was exercised; whether `--renderer vulkan` works here is not measured.

## Cause
The earlier failure was the wrong argument (`en_patch/` instead of `disc/SLPS_256.75`). The sandbox itself does not stop
the game from running headless.

## Fix
None in the code. Cards must run `tools/run/smoke.sh` or `build/linux-dev/don --headless ... disc/SLPS_256.75`, with
`disc/`, `third_party/`, `generated/` and `build/linux-dev` linked from the main checkout.

## Verification
```sh
DON_ISO=/nonexistent DON_USER_DIR=$PWD/work/prun-user bash tools/run/smoke.sh 20
```
Output: exit code 0, `RESULT: OK_VSYNC_LOOP 1020 1140`, `[don] exit: --seconds elapsed, code 0 (20.5s, 1200 frames,
worst 10s window 60.0 fps)`.

This shows that the game boots and runs its frame loop in a sandbox, headless. State-hash and frame-time comparisons
that need only the headless run can be done in sandboxes. Anything that needs a GPU renderer is not shown by this run.

## Temporary diagnostics
None added.
