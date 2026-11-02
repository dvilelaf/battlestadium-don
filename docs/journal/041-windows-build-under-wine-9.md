# 041 · The Windows build runs with the GS and sound under Wine 9 (lavapipe); the friend's texture bug is not reproduced

- **Date:** 2026-10-16
- **Task:** T-524 follow-up (reproduce the Windows graphics/sound bugs)
- **Status:** resolved

## Symptom (the friend's video, Windows 10 22H2, RTX 3060)

The geometry is always right (buildings, characters keep their shapes). Right after Naruto's
special-move cutscene (the full-screen red-streak background with his close-up) in a 4-player fight on
the Konoha village stage, back on the stage the TEXTURES are wrong: the HUD bar becomes coloured
garbage/noise tiles, stage surfaces turn white/grey or take other textures, characters are scrambled;
before that cutscene the same stage drew correctly. During fights the game runs at about half speed
(the fight timer drops ~3 per 6 s of video) and the sound is broken.

## What was built

- `tools/container/Dockerfile.wine` → image `don-wine:1` (1.16 GB) from `ubuntu@sha256:534baea6…30eb55`;
  wine/wine64 9.0~repack-4build3, mesa-vulkan-drivers 25.2.8 (llvmpipe/lavapipe, Vulkan 1.4.318),
  vulkan-tools 1.3.275, xvfb 21.1.12. `tools/container/run_wine.sh` runs a command in it (mount, uid:gid,
  memory limit, `build/.wine.lock`). The Windows package (`build/windows-package/don-windows`, commit
  `cfcd786c`) is run headless on a private Xvfb, `--renderer vulkan`, `VK_ICD_FILENAMES=…/lvp_icd.json`,
  sound with `--wav`.
- `tools/run/scripts/multiplayer_ougi.txt`: reaches a 4-fighter match (`--players 4`, multitap;
  P1 = Naruto) and does the ougi combo (multiplayer_match.txt's menu + main_mode_match2.txt's
  "double-tap toward, hold, Square ×3, forward+Square, Circle = ultra"); the red-streak cut-in fires at
  ~6750 and ~7470, then it keeps fighting to blank 7800.

## Verification (three comparisons, Windows-under-Wine vs Linux, all Vulkan)

| Route | Result |
|---|---|
| `default_title_idle` (title/menu) | 4 landmarks byte-identical; WAV byte-identical (5 604 351 frames) |
| `fight_practice` (1v1 fight) | 6 landmarks byte-identical; WAV byte-identical (6 322 789 frames) |
| `multiplayer_ougi` (4-player + ougi), 41 shots every 30 blanks 6600–7800 | first diff at blank **6780**; 4/41 differ (6780, 6840, 7500, 7530), each **exactly 1 pixel, max delta 1** (numpy); the other 37 byte-identical. All four are inside the cutscene, none in the fight before/after. |
| `multiplayer_ougi` Linux RADV vs Linux lavapipe | all 41 differ from blank 6600 (expected: different Vulkan driver) |

Contact sheet of frames 6720–6900 (3 rows: WIN-wine, LIN-lavapipe, LIN-RADV):
`work/wine-check/contact_sheet_ougi.png`. Run walls: Wine 208.4 s / LIN-lavapipe 195.9 s / LIN-RADV
132.8 s (worst 10 s window 21.0 / 22.9 / 55.6 fps).

## Classification

The friend's texture corruption and the half speed are **not reproduced**. The only Windows-vs-Linux
difference is a 1-pixel, off-by-1 floating-point rounding artifact (clang-cl/MSVC vs clang/glibc in the
GS) in four cutscene frames — not the garbage/noise tiles of the video, and there is no persistent
corruption after the cutscene.

## Cause

Not determined here. What still differs on the friend's PC and is not exercised by Wine+lavapipe: the
NVIDIA Vulkan driver (vs lavapipe/RADV), real-Windows thread/timing (the GS bridge's handling of guest
memory pointers whose data may be reused before the GS thread reads them — a race that the half speed
would expose), and the ~0.5× speed itself.

## Wine 9 quirks (scratch workarounds, not game bugs)

- Wine 9.0 cannot create the PCSX2 data directory non-recursively: `don_gs_open` fails with
  `PCSX2 data directory setup failed` unless `gs/data/PCSX2/inis` already exists (real Windows and
  Linux create it fine). Pre-created in the run script.
- The `wine` process hangs at teardown after a clean `the game ended, code 0` (killed with SIGKILL;
  screenshots and WAV are written first).
- The **software** renderer is still unusable under Wine: it wants a D3D11 device, which headless Xvfb
  cannot provide (`Failed to create render device … (Software)`); only the Vulkan renderer runs headless.

## Temporary diagnostics

None left in the tree. The data-directory pre-create and the post-run SIGKILL live only in the scratch
`work/wine-check/wine_run.sh` (not versioned).
