# 053 · The Linux archive after the Windows packaging changes

- **Date:** 2026-10-28
- **Task:** T-750
- **Status:** resolved (the archive differs from the reference list only by one file, explained below; the two checks passed)

## Symptom

Nothing to diagnose. T-742 made `tools/module/stage_install.py` platform-aware (shared with the Windows packaging) and
made the licence match case-insensitive. This entry checks that the Linux download did not change because of it.

## Archive

- **Packaged commit:** `5c5e083e27e14d24c390742990578a4fcf85efc8` (`origin/main`, already merged in the T-750 clone).
- **Archive:** `build/player/don-linux-5c5e083.tar.gz` in the T-750 clone. Size: 118908747 bytes.
- **SHA-256:** `3436990ccac9e568eefeaf6e82c3145cbcc13146585fe3645e30bb91e53eacac`
- **Build:** `DON_JOBS=4 uv run --no-project python -I tools/pipeline/package_linux.py`, 4m27s, from a clean clone
  (no build folders reused).

## List compared with the reference (ffce1e9)

`tar tzf` with the top folder removed, sorted, against `work/m7-inputs/linux-ffce1e9.list` (465 entries; the new list
has 466).

The one difference is `sdk/game_module/win_runtime.cpp` (1889 bytes), added. Its cause:

- It was added by `4919d89` (T-730, the game module as a Windows DLL). That commit is after ffce1e9.
- `tools/module/stage_install.py` line 112 copies the whole `src/game_module` folder into `sdk/game_module`, so every
  source file of that folder ships. That line was already there at ffce1e9 (line 82 then); T-742 did not change it.
- The file is Windows-only (`make_ninja.py` adds it only for the Windows target). On Linux it is inert source.

Not a defect of the T-742 change. It is an expected consequence of T-730 under the current copy rule. Whether the Linux
download should ship Windows-only source is a question for the owner; no change was made here.

The licence set is the same as the reference: the case-insensitive match changed no name in the list.

## Checks (unpacked into a scratch folder of the clone)

- Build id: `strings don | grep -E '^[0-9a-f]{12}-(dirty-)?(ffmpeg|noffmpeg)$'` gives `5c5e083e27e1-ffmpeg`;
  `grep -c dirty` on the strings gives 0.
- `SOURCE.txt` names the commit `5c5e083e27e14d24c390742990578a4fcf85efc8`.
- `DON_USER_DIR=<empty temporary folder> timeout 60 ./don --headless --renderer software` prints
  `[don] the game is not prepared: run  don --prepare <your disc image>` and exits with code 2.

## Correction to journal 051

Journal 051 says the unpacked folder has `BUILT_FROM.txt`. It does not, and it did not at ffce1e9 either (the reference
list has no such entry). The cause: `package_linux.py` writes `BUILT_FROM.txt` into the player folder, and
`stage_install.py` skips that name when it lays out the download (line 99, `("don", "BUILT_FROM.txt")` before T-742,
`(plat.built_program, "BUILT_FROM.txt")` after). The commit is kept in `SOURCE.txt` instead. The exclusion predates
T-742. The write in `package_linux.py` is therefore unused in the archive. Not changed here: a decision for the owner or
a later task (remove the write, or ship the file).

## Verification

- `tar tzf` list diff: one added line, explained above.
- Build id without `-dirty`, and the exit-2 line with code 2: see Checks.
