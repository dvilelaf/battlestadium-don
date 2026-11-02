# 066 · Windows release zip from main f537322, with the app-local VC++ runtime

- **Date:** 2026-10-29
- **Task:** T-774
- **Status:** resolved, with one finding about the player README (below); speed check on the Windows PC is the orchestrator's

## Symptom
Need the Windows download from current main (module `-O2`, patch 0093, exit-line fix, VC++ DLLs app-local), with FFmpeg.

## Investigation
- Packaged commit `f537322` (main plus HANDOFF). Commands as journals 063 and 064: `build_tools_windows.sh` (`DON_TOOLS_WIN_BUILD=build/player-build-win/recomp`),
  `fetch_toolchain.py --platform windows`, `package_windows.sh --jobs 4` (3 min 26 s; the libraries were rebuilt by the packaging).
- PS2Recomp reset: the clone's `third_party/` is a symlink to the shared tree, which must not be touched, so a clone of the shared tree's objects
  (`git clone --no-hardlinks`, origin reset to the upstream URL) was made under `work/t774/PS2Recomp` and `fetch_ps2recomp.sh --reset` was run there
  (temporarily as `third_party/PS2Recomp`; the script works on `third_party/PS2Recomp` only). All 76 patches in `series` (0001..0093) applied, none already applied, none failed.
  `build_tools_windows.sh` then took that tree through `DON_PS2RECOMP_SRC` (a nested symlinked `third_party/` is not mounted in the container: the
  first try failed with "could not open kernel32.lib"). The packaging builds its own PS2Recomp tree from the series.
- `git repack -a -d` in the clone first (journal 063: the clone's alternates are not mounted in the container); the build id came out clean.

## Verification
- Zip `don-windows-f537322.zip`: 86101876 bytes, sha256 `924746533d4a4663943dde24625878c17b43bc97ad817bf8ac238fd567a2be36`.
- (a) Wine 6.0.3, fresh prefix, `WINEDLLOVERRIDES=msvcp140,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n`, empty `DON_USER_DIR` (`Z:\` path),
  `--headless`: `[don] the game is not prepared: run  don --prepare <your disc image>`, exit 2.
- (b) `strings -a "Battle Stadium DON.exe" | grep -E '^[0-9a-f]{12}-'` prints `f537322b1a36-ffmpeg`.
- (c) `strings -n 3 ... | grep -n -e '-O[0-9]'` prints `-O2`, `-O1`, `-O2`, `-O1` (two recipes, no `-O3`).
- (d) `unzip -l` lists `msvcp140.dll`, `msvcp140_atomic_wait.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` and `licenses/Microsoft-VCRedist-license.rtf`.
- Finding: the `README.txt` in the zip comes from `packaging/windows/README.txt`, not from `tools/package_windows/README.txt` (the file T-771 edited, which nothing
  packages). The shipped README does not mention the redistributable at all (it neither asks to install it nor says it is not needed). Not changed here (outside the owned file).
- Not run: `--prepare` under Wine. `pgrep -ax wine` empty at the end.
