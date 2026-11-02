# 039 · Determinism of the game before the network: the same route, five times, four comparisons

- **Date:** 2026-10-15
- **Task:** T-500 (M5, wave 1; measurement only, no code)
- **Status:** resolved (measured on the binary of 14:27; all four comparisons identical)

## Question

Before the network is allowed to decide anything, the guest state must be a pure function of the pad input: lockstep
with input delay (knowledge 22 section 1) means every copy must produce the same `[don] state` line from the same
pads. This measures the four host-side inputs a *lockstep peer selection* could vary between players, on the route
that the netplay tests use (`multiplayer_match`, four players): two runs of the same command, the renderer
(Vulkan/software), the host time zone, and (below) the screenshot writer.

## The instrument and the binary

- Binary: `build/player/don-linux/don`, **not rebuilt** for this entry (other agents were building and editing
  `src/host/` at the same time). `BUILT_FROM.txt`: commit **a66dc2ffacd419270c2ab488aeb9800209fe335a** (T-307,
  patch 0073 in the series), built 2026-10-15T14:27:01+02:00, sha256 `b5d3d469f9b16580eed6000405e651cd…` (first 32 hex digits). It has no
  `--net-*` option yet; none is needed here.
- `DON_TRACE_STATE=60` prints one line every 60th vertical blank:
  `[don] state blank=<n> eeCycle=<ee> iopCycles=<iop> iopInstructions=<iopi> ramHash=<64-bit FNV-1a over the 32 MiB>`
  (`src/game/don_overrides.cpp`). `eeCycle` is non-zero here, so this build has `PS2X_EE_SCHEDULER_STATS` (patch 0034).
- The route's own environment (`tests/regression/harness.py`): `DON_TRACE_FRAMES=1 DON_PLAY_MOVIES=1
  DON_MOVIE_SKIP_AFTER=240`; the script taps Start itself to skip the movie. `--players 4` (multitap: the script
  drives P2, P3 and P4), `--sound on` (the SPU2 is emulated; `--headless` does **not** open an audio device unless
  `DON_AUDIO_DEVICE=1`, `host.cpp:380-382`, and prints nothing to the owner's speakers), `--headless`, no owner's
  display and no memory card of the owner: **each run had its own new empty `--mc-dir` folder** under `work/t500/`.
  The machine's own `TZ` is `CEST`, so `TZ=UTC` is set explicitly.
- Exit codes: `0` for every run; each ended on the script's own `quit` at blank 16700 (278 state lines, blanks 60 to
  16680), `[don] frame 16080` / `[don] vsync 16680`; wall time 278-279 s each (16700 blanks, 59.9 blanks/s: the run
  is paced at real time).
- Every command of the four runs, driven by `work/t500/cmds.sh` (see also `work/t500/driver.log`):

```bash
set -a; . ./local.env; set +a
env DON_TRACE_STATE=60 DON_TRACE_FRAMES=1 DON_PLAY_MOVIES=1 DON_MOVIE_SKIP_AFTER=240 TZ=<TZ> timeout 900 \
  build/player/don-linux/don --players 4 --headless --sound on --mc-dir work/t500/mc-<X> <RENDERER> \
  --input-script tools/run/scripts/multiplayer_match.txt --seconds 3600 en_patch/jpvoice/disc/SLPS_256.75 \
  > work/t500/run-<X>.log 2>&1
# <X>=A: TZ=UTC (no --renderer: Vulkan, the default)   <X>=B: TZ=UTC
# <X>=C: TZ=UTC --renderer software                    <X>=D: TZ=Asia/Tokyo (Vulkan)
grep -o 'state blank=[0-9]* eeCycle=[0-9]* iopCycles=[0-9]* iopInstructions=[0-9]* ramHash=[0-9a-f]*' \
  work/t500/run-<X>.log | sed 's/^state //' > work/t500/states-<X>.txt
```

## Result: the four comparisons

Extraction: `grep -o 'state blank=…ramHash=…' | sed 's/^state //'`; comparison `diff` over **all** lines (278 each
run), not over the first and last only. The five files `states-{A,B,C,D,E}.txt` have the same md5,
`0373ef934bf9fe34f9590bfdd4f34d36`.

| # | Comparison | Difference |
|---|---|---|
| 1 | A (Vulkan, `TZ=UTC`) against B (same command again) | **identical**, 278 of 278 lines |
| 2 | C (`--renderer software`, `TZ=UTC`) against A (Vulkan) | **identical**, 278 of 278 lines |
| 3 | D (`TZ=Asia/Tokyo`, Vulkan) against A (`TZ=UTC`) | **identical**, 278 of 278 lines |
| 4 | E (2nd run with `--renderer software`, with `--shots`, 286 PNGs) against A | **identical**, 278 of 278 lines |

`diff -q states-A.txt states-X.txt` printed nothing for X = B, C, D, E. **There is no first blank that differs.**

| Run | Options | First state line (blank 60) | Last state line (blank 16680) |
|---|---|---|---|
| A | Vulkan, `TZ=UTC`, mc-A | `blank=60 eeCycle=334387819 iopCycles=38189287 iopInstructions=1328171 ramHash=4354d2166f2a692d` | `blank=16680 eeCycle=82026657200 iopCycles=10253183443 iopInstructions=1237192003 ramHash=b7e4e2db1dfee6a8` |
| B | Vulkan, `TZ=UTC`, mc-B | same as A | same as A |
| C | software, `TZ=UTC`, mc-C | same as A | same as A |
| D | Vulkan, `TZ=Asia/Tokyo`, mc-D | same as A | same as A |
| E | software, `TZ=UTC`, `--shots`, mc-E | same as A | same as A |

Run E was added after the four: it is the auxiliary progress check of the card's route (see below) and a second
software run. It changes nothing in the four comparisons above and is reported separately.

## What the route actually did (auxiliary, run E)

`--shots work/t500/shotsE` (two more runs would have been needed for the recorded `md5`s to be comparable: the
route's landmarks in `tests/regression/routes.json` were recorded with **2** players and the card's run has 4, so the
`md5` check is not applicable). What was checked instead: the run produced all eight named shots of the script, and
two of them were looked at. `gaara_card.png` is Gaara's "Please push START button" name card, `winner_screen.png` is
WINNER (Frieza) — the same screens the landmarks of `routes.json` describe (`gaara_card`, `winner_screen`). So with
4 players the route does run the whole match to the WINNER screen and ends on its timer; the identical state lines
in the table above are the match's, not a stuck menu's.

## Notes for the next person

- **No host-visible input moved the guest state** on this route: 5 runs, 16700 blanks each, 4 configurations, the
  same 278 lines including the 32 MiB RAM hash. That is the strongest baseline T-508/T-509/T-510 can build on: with
  fixed pads the copy is reproducible, so a `DESYNC` in the loopback tests is the network's fault and not the build's.
- The comparison is only valid **within one build** (journal 014: the same script after a relink gave another
  `ramHash` with identical counters). The state lines above belong to commit a66dc2ff; T-513 appends to this entry
  with patch 0074 (`DON_SESSION_EPOCH`) and must use its own build's numbers.
- The memory card was **empty in all five runs** (`--mc-dir` folders hold only the empty `mc0`, `mc1` the port
  creates): the route never saves, so the card's *contents* cannot be an input here. The one card input left is the
  folder's own creation/modification times, which is T-513's comparison, not this one.
- The screenshot writer did not change the guest (`--shots` in run E, 286 PNGs, identical lines) — consistent with
  the GS never being read back by the guest; it is also the host-side crash source of journals 011/014, which did
  not fire in these five runs.
- One `timeout 900` per run bounds the wall time: the four runs take 4 x 279 s ≈ 19 min; the machine was loaded by
  other agents' work during part of them and the lines still agree, which is what patch 0038 was for.

## Verification

- `diff -q` and `md5sum` over the extracted state lines of all five runs: identical (no line differs).
- Logs, the extracted line sets and the command script: `work/t500/` (`run-{A..E}.log`, `states-{A..E}.txt`,
  `cmds.sh`, `driver.log`); run E's PNGs in `work/t500/shotsE`. Guest memory and logs stay under `work/`
  (gitignored); nothing from the game was added to the repository.

## Temporary diagnostics

None added; `DON_TRACE_STATE` (journal 014) was already there and is off by default. No file of the port, of the
tests or of the runtime was touched by this entry.

---

## Follow-up 2026-10-15 (T-513): the session clock on the save_and_load route

T-513 asked to repeat the T-500 comparisons on the route where the card's data matters (the memory card is read at
start), with patch 0074 (`DON_SESSION_EPOCH`), and once without it to show whether the session clock is needed.

**Binary.** Own build folder, `build/t513/don` (the shared `build/linux-dev` is rebuilt by other agents), built from
the shared runtime tree (0074 applied) at 17:06, HEAD 82dcbf1 (plus the other agents' uncommitted `src/host` work,
none of which this route touches), sha256 `4b7f982b7680b6263476834ceff3b3ec2e3b6b339182abb3be6d37515bfe9fd1`;
`DON_SESSION_EPOCH` is in the binary. Libs resolve through its `$ORIGIN:$ORIGIN/../gs` run path (build/gs), no copy
needed.

**Method.** The `save_and_load` route of `tests/regression/routes.json`, both steps, `--sound off` (the route's
setting), `--headless --renderer software`, `DON_TRACE_STATE=60 DON_TRACE_FRAMES=1 DON_PLAY_MOVIES=1
DON_MOVIE_SKIP_AFTER=240`, each step on its own card folder under `work/t513/` (never the owner's). Four
configurations with `DON_SESSION_EPOCH=1700000000` — `TZ=UTC` and `TZ=Asia/Tokyo`, each with a card folder created
now and one whose folders and files were touched an hour back (`touch -d '1 hour ago'`; the card's
`BISLPS-25675game` file mtime was 16:15 vs 17:11 for "now", verified with `stat`) — and three without the epoch
(`UTC` now, `Tokyo` now, `UTC` one hour old). 14 runs in all, every one exit 0; the save step ends at blank 4020
(67 state lines), the load step at blank 3420 (57 state lines).

**Result.** The 7 save runs are byte-identical (md5 `069d3ae5adb3aed8d9220d563a46be18`) and the 7 load runs are
byte-identical (md5 `0569d2d7ec638acf26a35d6a4c9f0768`), **across all four epoch configurations and all three
no-epoch configurations**. First/last state line: save
`blank=60 eeCycle=334387819 iopCycles=38303233 iopInstructions=6836243 ramHash=9079737375231a1d` →
`blank=4020 eeCycle=19798971853 iopCycles=2498084630 iopInstructions=895421680 ramHash=6529edc3f5df23c1`; load
`blank=60 …ramHash=9079737375231a1d` → `blank=3420 …ramHash=f28001fa9d30bf37`. There is no first blank that differs.

**What this means.** On this route the session clock is **not needed** for determinism: neither the host time zone
nor the card folder's file dates change the guest state hash, and the epoch runs are identical to the no-epoch runs.
The guest never stores a date in RAM on the save/load/options path (the save/load dialogs show no date, and the OSD
time-zone value is not read into RAM here), consistent with T-500's finding that `multiplayer_match` is TZ-insensitive
without 0074. The clock remains necessary in principle wherever the guest *does* render a date (a save browser, the
date on a card entry, the main menu), but that is not on the netplay test routes. Logs, extracted state lines and the
command script are under `work/t513/` (`run.sh`, `epoch.out`, `noepoch.out`, `states-*.txt`, `run-*.log`); nothing
from the game was added to the repository.
