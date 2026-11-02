# 051 · The Linux release archive from main, proven headless

- **Date:** 2026-10-27
- **Task:** T-726
- **Status:** resolved for the archive proof (checks 0 to 6 and the route were run on the packaged program; see
  "Open points" for what the proof does not cover)

## Symptom

Nothing to diagnose. This entry records the release candidate built from `main`: the Linux download the owner plays
from the dock, with the legal changes of 2026-10-27 (`SOURCE.txt` names the source URL, the LLVM trust note, the
`public_check` allow list) and the fixes of journal 050 (PLAY direct, `aligned_alloc`). The method is journal 048,
the commands are those of journal 050.

## Earlier attempt (blocked, kept)

The first attempt packaged `1b92fcb` (`build/player/don-linux-1b92fcb.tar.gz`, SHA-256
`3454012…40db9`). Checks 0 to 2 passed. Checks 3 to 6 and the route were blocked: `en_patch/{jpvoice,envoice}/disc.iso`
were symlinks to a folder outside this sandbox, so `tools/run/setup_check.py` stopped with `FileNotFoundError`. They
are now hard links inside `en_patch/` (same size, readable), so the blocker is gone.

## Archive

- **Packaged commit:** `ffce1e9dee17fb7a92120abc8f65f8226778de44` (the merge of `origin/main` into `agent/t-726`, which
  brought `c2200cc` and the legal changes). The journal commit of this task comes after it and is not in the archive.
- **Archive:** `build/player/don-linux-ffce1e9.tar.gz` in the T-726 clone
  (`<swarm>/t-726/build/player/don-linux-ffce1e9.tar.gz`)
- **Size:** 118908354 bytes
- **SHA-256:** `74ba8a9ab938e175cdba55f9c86f96b1a246b0cf5857b824c705b5218d946a1c`
- **Libraries:** the GS and VU libraries were rebuilt by the packaging. A first run reused the `build/player-build/gs`
  and `vu` folders of earlier today (the run took 48 s and its log shows only the `don_host` steps); both folders were
  deleted and the packaging run again (2m11s, 465 compile steps): `[716/716] Linking CXX shared library don-gs-bridge/libdon_gs.so` and
  `[29/29] Linking CXX shared library libdon_vu.so`. The archive SHA-256 is the same after the rebuild.

## Commands and their last line

Run from the T-726 clone unless said otherwise. Game commands under `timeout` (600 s; checks under 1200 s). The
archive was unpacked to `work/t726/unpack/don-linux-ffce1e9/`.

| Step | Command | Last line of output |
|---|---|---|
| Merge | `git fetch -q origin && git merge --no-edit origin/main` | `Merge made by the 'ort' strategy.` (HEAD `ffce1e9`) |
| Package | `DON_JOBS=4 uv run --no-project python -I tools/pipeline/package_linux.py > work/t726/package.log` | `[package_linux] built .../don-linux-ffce1e9.tar.gz (113 MB) from commit ffce1e9dee17... in 2m11s` |
| Check 0 | `tar -xzf` then `grep -E 'repository\|commit' <unpacked>/SOURCE.txt` | `  commit:     ffce1e9dee17fb7a92120abc8f65f8226778de44` (the repository line holds `https://github.com/dvilelaf/battlestadium-don`) |
| Check 1 | `strings <unpacked>/don \| grep -E '^[0-9a-f]{12}-(dirty-)?(ffmpeg\|noffmpeg)$'` | `ffce1e9dee17-ffmpeg`; `grep -c dirty` on the strings: 0 |
| Check 2 | `DON_USER_DIR=<empty mktemp dir> timeout 60 ./don --headless --renderer software` | `[don] the game is not prepared: run  don --prepare <your disc image>`, exit code 2 |
| Check 3 | `uv run --no-project python -I work/t726/plain_prepare.py <clone>` (see "Check 3") > `work/t726/plain_prepare.log` | `plain_prepare: 1 passed of 1 (690 s)` |
| Check 4 | `DON_JOBS=4 DON_CHECK_LIBS=<clone>/build/player/don-linux timeout 1200 uv run --no-project python -I tools/run/setup_check.py > work/t726/check.log` | `setup_check: 27 passed, 0 failed (575 s)` |
| Check 5 | `... tools/run/setup_check.py --full --only full-prepare-and-play > work/t726/full.log` (same env) | `setup_check: 1 passed, 0 failed (615 s)` |
| Check 6 | `grep -n 'skipped, setup ended with PLAY' work/t726/plain/full-prepare-and-play/run.log` | `385:[don] the start screen: skipped, setup ended with PLAY` |
| Route | `DON_TRACE_STATE=60 DON_USER_DIR=<prepared> SDL_AUDIODRIVER=dummy timeout 600 <unpacked>/don --headless --renderer software --sound off --players 4 --input-script tools/run/scripts/multiplayer_ougi.txt --seconds 33 en_patch/jpvoice/disc/SLPS_256.75 > work/t726/route.log` | `[don] exit: --seconds elapsed, code 0 (38.9s, 1980 frames, worst 10s window 60.0 fps)` |

`<prepared>` is the user folder of check 3: `/tmp/don-t726-plain-62/full-prepare-and-play/user`, a temporary folder
(not copied into the clone). The route log has 32 `[don] state blank` lines (`grep -c`), the first at blank 60.

## Check 3: the prepare in the setup screens

The fast check runs its full prepare path (`full-prepare-and-play`) with `args=["--headless"]`, and its log has no
skip line (the skip is not taken in headless). For the skip line, the same path was run without `--headless`, by
`work/t726/plain_prepare.py` (scratch, not committed): it imports `tools/run/setup_check.py`, copies the path with
`args=[]`, and runs it on a private Xvfb (display 98, killed by pid). Same keys, env and screenshots.

Preparing screenshots (absolute paths; the frame files are written about every 20 s, their modification times are the
grab times):

- `<swarm>/t-726/work/t726/plain/full-prepare-and-play/04-preparing.png`: 0 %,
  "Estimating time left…" (copy step). The title is whole, wrapped on two lines: "English, Japanese voices (Lord Izen
  patch, 2023-09-07) · checked".
- `.../frame-03.png` (17:39:50): 15 %, "Estimating time left…", build 0/1,876 (build start).
- `.../frame-04.png` (17:40:10): **18 %, "About 17 min left"**, build 34/1,876.
- `.../frame-05.png` (17:40:30): 23 %, "About 9 min left", build 56/1,876.
- `.../frame-06.png`, `frame-07.png`, `frame-08.png`, `frame-12.png`, `frame-13.png`: 31 %, 37 %, 43 %, 55 %, 57 %, with
  "About 6", "About 5", "About 4", "About 4", "About 4" min left.
- `.../frame-15.png`: 71 %, "About 3 min left", build 1,173/1,876. `.../frame-20.png`: 62 %, "About 3 min left",
  build 809/1,876 (see "Open points").
- `.../05-ready.png` (17:49:05): ready.

Measured: at about 18 % the screen showed "About 17 min left"; the real time from that frame (17:40:10) to the ready
screen (17:49:05) was about 8.9 min. At 23 % it showed "About 9 min left" (real remaining about 8.6 min). The estimate
is high at the start of the build and close later.

Translate-step text: no frame was grabbed during translate (5 % to 14 % in the log), so "Estimating time left…" in that
step is not confirmed by a screenshot; only the copy step (0 %) and the build start (15 %) are.

## Check 4 and check 5

- Check 4: the 27 paths of the fast check, `PASS` each, `FAIL` none (`grep -c '^PASS' work/t726/check.log` = 27).
- Check 5 (`--full`, sanitized, ASan and UBSan build of `build/check-asan`, `DON_CHECK_LIBS` = the packaging's
  `build/player/don-linux`): `PASS full-prepare-and-play screens welcome > welcome-offer > checking > preparing > ready`.
  Its log, `work/t726/full.log` (the check's own folder `work/check/full-prepare-and-play/run.log`), has
  `[don] state blank` once, the first game frame.

## Open points

- **Progress bar is not monotonic in the screenshots.** The log's build progress rises from 15 % to 100 % (line 34 to
  line 370). The frames seen are `frame-13` (57 %, 590/1,876), `frame-15` (71 %, 1,173/1,876) and `frame-20` (62 %,
  809/1,876), in grab-time order. Either the frame file times do not match the grab order, or the build count drops
  between them. Not explained by this run; to check against the run's own log before any change.
- **The route prints `E59`.** `[don] E59 the previous run of module d86e182144617e50 did not exit cleanly` is the
  first line of `work/t726/route.log`. The cause is the check's harness, not the game: `tools/run/setup_check.py`
  `run_don` kills the process with SIGKILL once `[don] state blank` appears (line 31 of that function), which leaves
  the module's crash marker. The route still ended with `code 0`.
- The fast check's build (`build/check-asan`) and the packaging were run with `DON_JOBS=4` (a previous parallel run
  froze the owner's PC, journal 048 / memory notes).

## Verification

- The archive's SHA-256 and size: see "Archive". The unpacked folder has `don`, `libdon_gs.so`, `libdon_vu.so`,
  `libdon_spu2.so`, `resources/`, `config/`, `SOURCE.txt`, `BUILT_FROM.txt`.
- Checks 0 to 6 and the route: the table above, each with its last line.
- `pgrep -ax Xvfb` and `pgrep -ax don` were empty at the end of the task.
