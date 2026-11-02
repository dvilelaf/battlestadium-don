# 050 · The Linux archive with PLAY direct and the aligned_alloc fix, proven headless

- **Date:** 2026-10-27
- **Task:** T-722
- **Status:** resolved for the archive proof; the checks below were run on the packaged program

## Symptom

Nothing to diagnose here. This entry records the archive built from `main` after T-720 (setup PLAY enters the game
with no start menu) and T-721 (PCSX2 patch 0007, `_aligned_malloc` rounds the size to a multiple of the alignment),
and the checks that the packaged program behaves as those changes say. The method is journal 048.

## Archive

- **Packaged commit:** `4643d4de5db5e14cf3fce6bd58f31cd736f8862e` (`main` after the merge of `origin/main`, which was
  already up to date). The GS, VU and SPU2 libraries were built by the packaging (`build/player-build/`), not reused.
- **Archive:** `build/player/don-linux-4643d4d.tar.gz` in the T-722 clone
  (`<swarm>/t-722/build/player/don-linux-4643d4d.tar.gz`)
- **Size:** 118908503 bytes
- **SHA-256:** `27fe7cecd6df8d22ded8aef04efade44055fefa86b2b8a69746bab9048366c54`

## Commands and their last line

Run from the T-722 clone. Each game command ran under `timeout` (600 s; the checks under 1200 s). The archive was
unpacked to `work/t722/unpack/don-linux-4643d4d/` for checks 1 and 2 and the route.

| Step | Command | Last line of output |
|---|---|---|
| merge | `git fetch -q origin && git merge --no-edit origin/main` | `Ya está actualizado.` (already up to date) |
| package | `DON_JOBS=4 uv run --no-project python -I tools/pipeline/package_linux.py > work/t722/package.log 2>&1` | `[package_linux] built .../build/player/don-linux-4643d4d.tar.gz (113 MB) from commit 4643d4de5db5e14cf3fce6bd58f31cd736f8862e in 3m59s` |
| check 1 | `strings don \| grep -E '^[0-9a-f]{12}-(dirty-)?(ffmpeg\|noffmpeg)$'` | `4643d4de5db5-ffmpeg` (no `-dirty`) |
| check 2 | `DISPLAY=:96 DON_USER_DIR=<empty mktemp -d> HOME=<empty> SDL_AUDIODRIVER=dummy timeout 60 ./don --headless --renderer software` | `[don] the game is not prepared: run  don --prepare <your disc image>`, exit 2 (not 124) |
| check 3 | `uv run --no-project python -I work/t722/prepare_play.py` (see below) | `setup_check: 1 passed, 0 failed (0 s)` |
| check 4 | `DON_CHECK_LIBS=<clone>/build/player/don-linux DON_JOBS=4 timeout 1200 uv run --no-project python -I tools/run/setup_check.py > work/t722/check.log 2>&1` | `setup_check: 27 passed, 0 failed (597 s)` |
| check 5 | `DON_CHECK_LIBS=<clone>/build/player/don-linux ... timeout 1200 uv run --no-project python -I tools/run/setup_check.py --full --only full-prepare-and-play --keep > work/t722/full.log 2>&1` | `setup_check: 1 passed, 0 failed (458 s)` |
| route | `DISPLAY=:96 DON_TRACE_STATE=60 DON_USER_DIR=<prepared> SDL_AUDIODRIVER=dummy timeout 600 ./don --headless --renderer software --sound off --players 4 --input-script tools/run/scripts/multiplayer_ougi.txt --seconds 33 en_patch/jpvoice/disc/SLPS_256.75` | `[don] exit: --seconds elapsed, code 0 (33.2s, 1980 frames, worst 10s window 60.0 fps)` |

Xvfb was started on fixed displays (`:96` by hand for checks 2 and the route, killed by pid after each run; the check
runs start their own on `:97`) and `pgrep -ax Xvfb` and `pgrep -ax don` were empty at the end.

## The four checks

**1. Build id.** No `-dirty`: `4643d4de5db5-ffmpeg`.

**2. Headless, empty user folder.** Exit 2 with the "game is not prepared" line, as journal 048 Resolution.

**3. Prepare in the real setup screens.** The check's own `full-prepare-and-play` path (check 5) prepares through the
screens, but it passes `--headless` to the game, and the start-menu skip needs no `--headless` (`src/host/host.cpp:464-466`).
So check 3 was run by `work/t722/prepare_play.py`: the same `run_path` of `tools/run/setup_check.py` and the same screen
script (`confirm,confirm`), against the packaged `don`, with `--headless` removed and the arguments empty. It ran on
`:98` with `DON_SETUP_SHOTS`. The screens: welcome, welcome-offer, checking, preparing, ready (PASS). Log:
`work/t722/plain.log`, game log `work/t722/shots/full-prepare-and-play/run.log`.

- Title: whole, wrapped on two lines: "English, Japanese voices (Lord Izen patch, 2023-09-07) · checked".
- Time text: "Estimating time left…" at 0 % (copy) and at 15 % (translate and the first build units). "About N min left"
  first appears at 23 % (build 57/1,876).
- Preparing PNGs: `work/t722/shots/full-prepare-and-play/04-preparing.png` (0 %, "Estimating time left…"),
  `05-ready.png`, and `frame-02.png` (15 %, Estimating), `frame-04.png` (23 %, "About 4 min left"). Absolute paths:
  `<swarm>/t-722/work/t722/shots/full-prepare-and-play/`.
- Minutes shown against the real time: at 23 % (grab at 12:57:31) the screen said "About 4 min left"; the ready screen
  appeared at 13:02:35, so the real time to 100 % was about 5 min 04 s. Preparing started at about 12:56:12; the whole
  preparation took about 6 min 23 s. The estimate was low by about a minute in the middle of the build.
- Measured from file times of the grabs (each grab is written when taken), not from a clock in the program.

**4. Fast check.** `setup_check: 27 passed, 0 failed (597 s)`, `work/t722/check.log`.

**5. Full check (sanitized, ASan + UBSan).** `setup_check: 1 passed, 0 failed (458 s)`, `work/t722/full.log`. The
run log has no `AddressSanitizer` or `runtime error` line; the game reached `[don] state blank=60`. This is the run
that T-721's `_aligned_malloc` patch fixes.

**6. The skip line.** `[don] the start screen: skipped, setup ended with PLAY` is in
`work/t722/shots/full-prepare-and-play/run.log` (check 3, plain run, line 394), before the first `state blank` line.
It is not in the check 5 log, because that path runs with `--headless` and the skip is not taken there.

## Route with the prepared folder

`work/t722/route.log`: 32 `[don] state blank` lines (blank 60 to 1920), last line `code 0`.

The route was run with `--seconds 33`. With `--seconds 32` the same route gave 31 lines (blank 60 to 1860, last
line `code 0`, 1920 frames), so the count depends on the run length: a state line is printed only before the
blank that ends the run. The route is the fight route of `tools/run/perf_fight.py`, with the arguments of journal 048.

## Notes

- The user folder used for the route was the prepared folder of check 3, in a temporary directory
  (`/tmp/t722-plain-hhgyw0t5/`), not copied into the clone (2.1 GB).
- Check 3's "0 s" in the summary line is the script's own placeholder: it does not time the run.
