# 052 · Windows zip under Wine: first run, link fix, second zip

- **Date:** 2026-10-28
- **Task:** T-743, T-746, T-748
- **Status:** resolved (the full prepare under Wine of the second zip is left to the real PC)

## Symptom
T-743 built the first Windows zip (sha256 `4a68155e…62e9`, 85793516 bytes) and ran it under Wine:
- `Battle Stadium DON.exe --headless` with an empty `DON_USER_DIR` exited 2 after the "game is not prepared" line (as expected).
- `--prepare` failed with E-BUILD-2 at the module link: `could not open '...\obj\batc'`.
- The link error text named `obj\.o.err`, not the stderr file of the link (`don_game.dll.err`).

## Classification
Build: Windows command-line limit. Not in the catalog of `docs/knowledge/08-external-references.md`.

## Investigation
- 1874 object paths on one `lld-link` command line exceed the 32767-character `CreateProcessW` limit, so the line was cut.
  A temporary response file made the prepare reach 100 % and exit 0 (module `3bfc5d11cdb626bf`).
- The screens resume, preparing and ready appeared (3 PNGs) with a copied user folder; the game ran until the 1800 s limit.
- `build_tools_windows.sh` failed with a PS2Recomp tree without patch 0092 (`/Qspectre-`); with patch 0092 applied it builds.
- `DON_USER_DIR` without a drive letter gives E-SYS-1 on Windows; a `Z:\...` path works.
- The module compile under Wine takes about 45 min.
- The dependency DLLs' licence texts were not installed by `build_win_deps.sh`.

## Cause
The link argv grew past the Windows limit; the err-file name of the link job was built from the object name pattern.
No licence text was installed next to the dependency DLLs.

## Fix
- T-746 (`f02ffae`): the Windows link reads its objects from a response file; a failed link names its own err file.
- T-748: `tools/pipeline/package_windows.py` reads the licence texts from the source archives in `third_party/win-deps-src/`
  (`collect_dependency_licenses`, from T-743's change); pytest on one fake zip and one fake tar.gz.

## Verification
Second zip, built from `main` plus the T-748 commit (`38ecd3f`) with `package_windows.sh --jobs 4`, after
`build_tools_windows.sh` (`DON_TOOLS_WIN_BUILD=build/player-build-win/recomp`) and `fetch_toolchain.py --platform windows`:
- sha256 `9dedef395f2b5d9a5ecdb30a09b7fb2318cb1dc8fa9e2bede383c7571605d034`, 85795295 bytes; 21 files in `licenses/`.
- Under Wine, unzipped: `Battle Stadium DON.exe --headless` with an empty `DON_USER_DIR` (`Z:\` path) prints
  `[don] the game is not prepared: run  don --prepare <your disc image>` and exits 2;
  `lld-link.exe --version` prints `LLD 17.0.6`; `clang++.exe --version` prints `clang version 17.0.6`.
- Not run: `--prepare` under Wine (45 min).

## Temporary diagnostics
None left. T-743's response-file edit was a diagnostic and was replaced by the T-746 fix.
