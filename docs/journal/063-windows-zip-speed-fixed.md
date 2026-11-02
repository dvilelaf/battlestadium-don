# 063 · Windows zip from main with both speed fixes (module -O2, patch 0093)

- **Date:** 2026-10-28
- **Task:** T-769
- **Status:** resolved (speed check on the owner's PC is the orchestrator's)

## Symptom
Need a release zip of main with module `-O2` (T-752) and PS2Recomp patch 0093 (T-767), with FFmpeg as the real package.

## Investigation
- Packaged commit `06ad518` (main plus HANDOFF). Patch series from clean: `fetch_ps2recomp.sh --reset` run in a scratch root
  (`tools/` and `patches/` symlinked, its own `third_party/PS2Recomp` clone with `--no-hardlinks`): all 0001..0093 applied; the shared tree was not touched.
- Commands: `DON_JOBS=4 DON_TOOLS_WIN_BUILD=build/player-build-win/recomp bash tools/pipeline/build_tools_windows.sh`;
  `fetch_toolchain.py --platform windows` (it lives in `tools/pipeline/`); `package_windows.sh --jobs 4`.
- First zip had build id `unknown-ffmpeg`: the clone borrows objects through `.git/objects/info/alternates` pointing to the main
  checkout, which the container does not mount, so `git diff` in the container exits 128 (CMake warning "build id commit is unknown").
  Fix in the clone only: `git repack -a -d` (objects now local; the read-only sandbox kept the alternates file, harmless: `git diff --quiet HEAD`
  in the container then exits 0), deleted the host `CMakeCache.txt` to force a reconfigure, repackaged. Not a repo change.

## Verification
- Zip `don-windows-06ad518.zip`: 85800366 bytes, sha256 `6da4f492626d24211f2ab89446226dd7db7bd218696212df833624892d76938b`.
- `strings -a "Battle Stadium DON.exe" | grep -E '^[0-9a-f]{12}-'` prints `06ad51820ce4-ffmpeg` (no `-dirty`).
- `strings -n 3 ... | grep -n -e '-O[0-9]'` prints `-O2` and `-O1` (two pairs), no `-O3`.
- Wine 6.0.3, empty `DON_USER_DIR` (`Z:\` path), `--headless`: exit 2 with `[don] the game is not prepared: run  don --prepare <your disc image>`.
  Needed `msvcp140_atomic_wait.dll` (omitted from the zip by design, journal 056) copied into the scratch Wine prefix only.
- Not run: `--prepare` under Wine. `pgrep -ax wine` empty at the end.
