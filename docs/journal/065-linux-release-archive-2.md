# 065 · The Linux release archive, candidate 2 (main 5d2b93e), proven headless

- **Date:** 2026-10-29
- **Task:** T-773
- **Status:** resolved for the archive proof (checks 0 to 7 and the route were run on the packaged program); one
  observation about the time estimate is open (see "Open points")

## Symptom

Nothing to diagnose. Same method as journal 051 (T-726) and journal 053 (T-750), on current `main`, which since 051
gained the Windows work (M7), `src/setup/path_text.h`, the module recipe `-O2` with the recipe in the module id (T-752),
PS2Recomp patch 0093 (T-767) and the exit-line fix (T-772).

## Archive

- **Packaged commit:** `5d2b93eb2402309e90b9eeadb9347e2e22ac0e19` (`main`; the journal commit comes after it).
- **Archive:** `build/player/don-linux-5d2b93e.tar.gz` of the T-773 clone, copied to `work/t773/`.
- **Size:** 118914264 bytes. **SHA-256:** `cc14361584db68d11b6685df025e3ee591a4a1f0743f2f8c01ece7b53da347d3`.
- **Build:** `DON_JOBS=4 uv run --no-project python -I tools/pipeline/package_linux.py`, 7m08s, clean clone (`build/`
  did not exist), so every library (GS, VU, SPU2, host) was built by the packaging.

## Checks

Game commands under `timeout` (600 s; the check 1200 s); `DON_CHECK_LIBS` = this packaging's `build/player/don-linux`.

| Check | Result |
|---|---|
| 0 `SOURCE.txt` | `repository: https://github.com/dvilelaf/battlestadium-don`, `commit: 5d2b93eb2402309e90b9eeadb9347e2e22ac0e19` |
| 1 build id | `strings don`: `5d2b93eb2402-ffmpeg`; `grep -c dirty`: 0 |
| 2 empty user folder, headless | `[don] the game is not prepared: run  don --prepare <your disc image>`, exit code 2 |
| 3 prepare in the setup screens (no `--headless`, private Xvfb :98) | `plain_prepare: 1 passed of 1 (1084 s)`; `[don] the start screen: skipped, setup ended with PLAY` (line 420 of its `run.log`) |
| 4 `tools/run/setup_check.py` | `setup_check: 27 passed, 0 failed (655 s)` |
| 5 `--full --only full-prepare-and-play` | `PASS full-prepare-and-play screens welcome > welcome-offer > checking > preparing > ready`; `setup_check: 1 passed, 0 failed (1085 s)` |
| 6 route, prepared folder | 32 `[don] state blank` lines; `[don] exit: --seconds elapsed, code 0 (37.0s, 1980 frames, worst 10s window 60.0 fps)` |
| 7 file list vs ffce1e9 | 466 entries against 465; one added: `sdk/game_module/win_runtime.cpp`; none missing |

Check 6's route: `DON_TRACE_STATE=60 DON_USER_DIR=<copy of check 3's prepared folder> SDL_AUDIODRIVER=dummy timeout 600
<unpacked>/don --headless --renderer software --sound off --players 4 --input-script tools/run/scripts/multiplayer_ougi.txt
--seconds 33 en_patch/jpvoice/disc/SLPS_256.75`. The exit line now shows a figure that counts only windows after the
first frame (T-772).

## Check 7: the list

Same single difference as journal 053: `sdk/game_module/win_runtime.cpp`, added by `4919d89` (T-730), copied by the rule
of `tools/module/stage_install.py` that ships the whole `src/game_module` folder. Windows-only source, inert on Linux.
No path of the reference is missing. Whether the Linux download should ship it is still the question left in 053.

## Check 3: the Preparing screens

The scratch script `work/t773/plain_prepare.py` (not committed) copies the `full-prepare-and-play` path with no
`--headless`, as in 051. Screenshots, in `work/t773/plain/full-prepare-and-play/` of the clone (grab time = file mtime):

- `04-preparing.png` (00:30:05): 0 %, "Estimating time left…", copy step. The title is whole on two lines: "English,
  Japanese voices (Lord Izen patch, 2023-09-07) · checked".
- `frame-05.png` (00:31:47): 11 %, "Estimating time left…", translating the game's code.
- `frame-20.png` (00:37:09): **57 %, "About 4 min left"**, build 581/1,876.
- `frame-35.png` (00:44:12): 77 %, "About 4 min left", build 1,399/1,876.
- `05-ready.png` (00:47:33): ready.

## Open points

- At frame-20 the screen said "About 4 min left" and the real time to the ready screen was about 10.4 min (00:37:09 to
  00:47:33). At frame-35 it said "About 4 min" and the real remaining time was 3.4 min. In 051 the estimate was high at
  the start of the build; here it is low in the middle. The build runs at about 117 steps per minute between those two
  frames, so 1,295 steps left at frame-20 need about 11 min. Not diagnosed here; this run had no other load. A gap of
  81 s without a frame grab (00:38:50 to 00:40:11) may mean a stall, which was not looked at.
- The whole check 5 took 1,085 s of its 1,200 s limit; on a slower machine the check would time out.

## Verification

The commands and last lines are in the table. `pgrep -ax Xvfb` and `pgrep -ax don` were empty at the end.
