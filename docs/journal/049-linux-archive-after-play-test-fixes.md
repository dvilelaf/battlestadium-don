# 049 · The Linux archive after the play-test fixes, proven headless

- **Date:** 2026-10-27
- **Task:** T-719
- **Status:** resolved (the four checks pass; two notes below)

## Symptom

Not a failure: the archive had to be rebuilt from `main` once T-715 (wrapped title), T-716 (headless fails loudly),
T-717 (no false `-dirty`) and T-718 (time left from the build's own rate) were in, and proven without a desktop, as
journal 048 did.

## Investigation

Commit packaged: `e2c165732fa8b7fd12848184f3af6ccf2cdb8429`. Everything ran from `work/t719/` of the clone; Xvfb on the
fixed display `:96` (it was found gone between steps and restarted; no Xvfb or `don` is left running).

**Archive.** `DON_JOBS=4 uv run --no-project python -I tools/pipeline/package_linux.py > work/t719/package.log`, 4m07s.
Last line: `[package_linux] built .../build/player/don-linux-e2c1657.tar.gz (113 MB) from commit e2c1657... in 4m07s`.
Archive `build/player/don-linux-e2c1657.tar.gz`, 118908292 bytes, SHA-256
`811b74486bb81cc60bf5f86807f7240f1c2046ce082c2dc37b93a717c764f797`. Unpacked in `work/t719/pkg/don-linux-e2c1657/`.

**Check 1, build id.** `strings <unpacked>/don | grep -E '^[0-9a-f]{12}-(dirty-)?(ffmpeg|noffmpeg)$'` gives
`e2c165732fa8-ffmpeg`: no `-dirty`.

**Check 2, headless with an empty `DON_USER_DIR`.**
`DISPLAY=:96 DON_USER_DIR=work/t719/e-user HOME=work/t719/e-home SDL_AUDIODRIVER=dummy timeout 60 don --headless --renderer software`
printed `[don] the game is not prepared: run  don --prepare <your disc image>` and exited 2 (not 124).

**Check 3, prepare in the real setup screens.** `tools/run/setup_check.py` prepares through the screens only in its
`--full` path, which uses the ASan build, so a scratch driver (`work/t719/prep.py`, not committed) reused its helpers
`make_home` and `run_don` with the packaged `don`: fake HOME with `Downloads/disc.iso` (the `en_patch/jpvoice` image),
`DON_SETUP_SCRIPT=confirm,confirm`, `DON_SETUP_SHOTS`, a grab of the screen every 15 s, no `--headless` (see note 1).
Screens: welcome, welcome-offer, checking, preparing, ready. Preparing began 08:44:50, `ready` at 08:51:12: 6 min 22 s.
- Title: whole and wrapped on two lines ("English, Japanese voices (Lord Izen patch, / 2023-09-07) · checked") in every
  grab.
- Time text: "Estimating time left…" at 15 % (checking the translated code, 08:45:04), 15 % and 16 % (build 0 and 19 of
  1,876, 08:45:34 and 08:45:50). Then "About 6 min left" at 22 % (08:46:05, build 53/1,876; real time to 100 %
  from then: 5 min 07 s) and "About 5 min left" at 27 % (08:46:20; real: 4 min 52 s). Last build grab: 93 %
  "Less than a minute left".
- Not seen: the copy and translate stages themselves; they finished within the first 15 s, before the second grab. The
  "Estimating" text is therefore confirmed from the end of translate and the start of the build, not inside copy.
- PNGs (all in `<swarm>/t-719/work/t719/prep/shots/`): `04-preparing.png`
  (screen-change shot), grabs `frame-02.png` to `frame-24.png`; the ones quoted: `frame-02.png`, `frame-03.png`,
  `frame-04.png` (16 %), `frame-05.png` (22 %), `frame-06.png` (27 %).

**Route with the prepared folder** (`work/t719/route.log`): the 048 command with the packaged `don`, absolute
`DON_USER_DIR=work/t719/prep/user`, `HOME` of the prepare: 129 `[don] state blank` lines, last line
`[don] exit: the game ended, code 0 (132.8s, 7800 frames, worst 10s window 57.7 fps)`. 129, as in 048, not the 32 of
the card (that route is not in the repository).

**Check 4, `tools/run/setup_check.py`** (default mode, `timeout 1200`, `work/t719/check.log`):
`setup_check: 27 passed, 0 failed (636 s)`.

## Cause

Nothing to fix. Notes:

1. With T-716, `don --headless` and no module exits 2, so the first prepare cannot use `--headless`; the check's
   `--full` path `full-prepare-and-play` passes `--headless` with an empty user folder (`tools/run/setup_check_paths.py:103`)
   and would, by the code of `main.cpp:129-133`, now exit 2. It was not run here (default mode); it needs a look.
2. The route with a relative `DON_USER_DIR` (`work/t719/prep/user`) answered "not prepared" (exit 2) on the prepared
   folder; the same command with an absolute path found the module. Not investigated further.

## Resolution

Archive and its four checks recorded above; the route ends with code 0.
