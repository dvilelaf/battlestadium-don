# 058 · Windows zip with the -O2 module recipe

- **Date:** 2026-10-28
- **Task:** T-760
- **Status:** resolved (Wine run check blocked by a missing runtime DLL, see below)

## Symptom
The module at `-O1` cost about 4 fps in fights on the Windows PC (journal 055). T-752 moved game units to `-O2`
(registration unit `-O1`); a zip with that recipe was needed for the PC speed check.

## Classification
Build and packaging task, no failure.

## Investigation
Built from commit `9dc800e` (main), as in journal 052: `DON_TOOLS_WIN_BUILD=build/player-build-win/recomp bash
tools/pipeline/build_tools_windows.sh` (rc 0), `fetch_toolchain.py --platform windows`, then
`DON_TOOLS_WIN_BUILD=build/player-build-win/recomp bash tools/pipeline/package_windows.sh --jobs 4` (3 min 52 s).
- Zip: `don-windows-9dc800e.zip`, 85795232 bytes, sha256 `3039cc50c299f9a41da34de43e3dbbade36eff7dbad70e956176b5347b5d4c33`.
- Recipe in the exe: `strings -n 3 "Battle Stadium DON.exe" | grep -n -e '-O[0-9]'` prints `-O2` and `-O1` (two
  pairs, in the same data region as `opt=`), and no `-O3`. The strings are short and stored apart, so this shows the
  two levels are in the program, not which unit gets which; the unit mapping is pinned by the `tests/setup/` test of T-752.
- Wine 6.0.3, empty `DON_USER_DIR` (`Z:\` path), `Battle Stadium DON.exe --headless`: the exe does not start.
  `err:module:import_dll Library MSVCP140_ATOMIC_WAIT.dll ... not found` (status c0000135, exit code 53 from
  `timeout`/wine). The zip omits that redistributable by design (journal 056) and this Wine prefix has none; the
  machine has no copy. So the "game is not prepared" exit 2 was not reproduced here. Nothing was stubbed.

## PC result
On the owner's PC (i5-9600K), zip from the investigation above:
- `--prepare` exit 0 in 825 s (723 s at `-O1`); module id `7ece7c3f94bb2f1c`.
- `tools/run/windows/fight_compare.ps1`, ougi fight, worst 10 s window, 3 runs: **57.0 / 57.5 / 57.9** fps against the
  old monolithic 63fc1b6 at **59.0 / 59.1 / 59.1** fps. Same state hash in all runs (`0cd1035476044e29`).
- Result: **FAIL** by about 1.6 fps. The `-O2` recipe recovers most of the `-O1` loss (journal 055) but not all of it.
  The residual is open (journal 059).

## Cause
Check (a) is blocked by the environment (no VC++ runtime DLL in a fresh Wine prefix), not by the build. T-748's
earlier run of the same check printed the exit-2 line; what differed in its prefix was not recorded.

## Fix
None needed in the repository.

## Verification
sha256 and size above; `pgrep -ax wine` empty after `wineserver -k`.

## Temporary diagnostics
None.
