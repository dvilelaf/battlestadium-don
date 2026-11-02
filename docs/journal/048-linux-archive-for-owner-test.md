# 048 · The Linux archive for the owner's test: the headless route with no module

- **Date:** 2026-10-27
- **Task:** T-714
- **Status:** resolved for the archive proof; the headless-without-module finding is fixed by T-716 (see Resolution)

## Symptom

The first T-714 session built the archive `build/player/don-linux-f18d746.tar.gz` and prepared the module in
`DON_USER_DIR=work/t714/user`, then ran the headless route with a fresh user folder (`work/t714/route-user`). That run
had no output for 6.5 hours and had to be stopped.

## Investigation

**Point 1: the packaged `don` with an empty user folder.** Command, from the repository root (Xvfb on `:96`, a fixed
display, started by hand because the program needs a video device even with `--headless`):

```
DISPLAY=:96 DON_USER_DIR=work/t714/point1-user HOME=work/t714/point1-home SDL_AUDIODRIVER=dummy \
  timeout 60 build/player/don-linux/don --headless --renderer software
```

Result: exit code 124 (`timeout` killed it), `work/t714/point1.log` empty, `work/t714/point1-user/don.log` empty, and
`setup.lock` left in the user folder. Nothing was printed in 60 s: the program waits for a player on the first-run
setup screen.

Same command with no display (`DISPLAY` and `WAYLAND_DISPLAY` unset): the program exits at once with code 0 and the
line `[don] setup: SDL could not start: No available video device`. So a missing display is reported, but a missing
module is not.

**Point 2: the route with the prepared folder.** Command (the fight route of `tools/run/perf_fight.py`, the same
arguments, `DON_TRACE_STATE=60`, the user folder `work/t714/user`, timeout 600):

```
DISPLAY=:96 DON_TRACE_STATE=60 DON_USER_DIR=work/t714/user SDL_AUDIODRIVER=dummy timeout 600 \
  build/player/don-linux/don --headless --renderer software --sound off --players 4 \
  --input-script tools/run/scripts/multiplayer_ougi.txt --seconds 200 en_patch/jpvoice/disc/SLPS_256.75
```

Result: 129 `[don] state blank` lines (not 32), last line
`[don] exit: the game ended, code 0 (130.2s, 7800 frames, worst 10s window 60.0 fps)`. The card's figure of 32 does not
match this route; the T-699 command the card refers to is not in the repository or in the orchestration folder, so
this is the route that was run, not a confirmed copy of T-699.

A first attempt of this route was stopped by the tool's 120 s limit at blank 7380; it was rerun with a 600 s limit.

**Step 4: the fast check** (`tools/run/setup_check.py`, default ASan build in `build/check-asan`):
`DON_CHECK_DISPLAY=95` was needed because `/tmp/.X97-lock` was left by an earlier run (no Xvfb running on `:97`; not
removed). Result: `setup_check: 27 passed, 0 failed (457 s)`, exit 0. The check run was under `timeout 600`, not the
1200 s of the card, because of the rule "every game command under timeout 600".

**Archive:** SHA-256 `c0d239adbac80f78c70e9861ecf9f8aa7448cd1daf79e89042a86864568164c8`, 118906576 bytes
(`work/t714/package.log`, `build/player/don-linux-f18d746.tar.gz`).

## Finding

Headless without a prepared module waits forever instead of failing: `don --headless` with an empty `DON_USER_DIR`
stays on the first-run setup screen, prints nothing and exits only when killed (run 1, exit 124). Not fixed here; the
product code is not changed by this task.

## Resolution

The archive proof is complete: the module was prepared, the route ran to its end with code 0 and the fast check passed.

**Point-1 hang, fixed (T-716).** `src/host/main.cpp`: with `--headless` and no installed module, `don` no longer opens
the first-run setup screens. It prints `[don] the game is not prepared: run  don --prepare <your disc image>` and exits
with code 2. `--headless` was already parsed by `don_parse_options` (`src/host/options.cpp`), so `options.cpp` and
`options.h` are unchanged. Without `--headless` the behaviour is unchanged.

Run (after the change; `build/t716` configured with `--preset linux-dev -DDON_LINK_GENERATED=OFF` inside
`tools/container/run.sh`, target `don`, empty `DON_USER_DIR` from `mktemp -d`, `timeout 60`):

```
build/t716/don --headless --renderer software   # exit 2
[don] the game is not prepared: run  don --prepare <your disc image>
```

The point-1 run before the fix exited 124 with no output. The fix is verified for the headless run only.
