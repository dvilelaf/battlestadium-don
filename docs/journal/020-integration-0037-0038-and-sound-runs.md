# 020 · Patches 0037 and 0038 integrated; the port with microVU and sound, measured

- **Date:** 2026-10-13
- **Task:** T-202 / T-207 / T-304
- **Status:** in progress (written as the work goes; sections are dated and labelled)

## Part 0 · three small repairs

1. `tests/runtime/mmi_lanes_test.cpp`: the reference read the lanes of a 128-bit value through
   `reinterpret_cast<uint16_t *>` of a `uint32_t` array (undefined behaviour; the host compiler folded it to
   zeros in some builds). The reference now copies the lanes out and in with `std::memcpy`; no check was
   weakened. Against a tree **without** 0037 (base 0001-0035 + the rewritten 0036, scratch worktree
   `work/integ/PS2Recomp-0036`, build folder `work/integ/rt-base`): `26 checks, 6 failed`, and the six are
   PEXEW, PEXEH and the three `FUN_0025b8f0` checks plus "the port's constant block is `a a a a`" (the
   PADDH/PSUBH/PMAXH/PMINH/PCGTH checks pass). With 0037 applied: `26 checks, 0 failed`.
2. Patch 0036 rewritten: its comments no longer attribute the crash at rip 0x66F80 to the overwrite (that was
   the build script rewriting a mapped library: journal 018), the removed code is removed, not commented out,
   and the header says how it was found (journal 015). Code unchanged: the two patched `MPEG.cpp` files are
   identical once `//` comment lines are removed (`diff` of the stripped sources printed nothing; the file has
   no `__LINE__`/`__FILE__`). The new patch applies on the base tree.
3. `journal 016` -> `journal 018` in the four build scripts that refer to the mapped-library rule.

## Part 1 · 0037 and 0038 in the series (before 0039)

- `series` + `0037-pexew-lanes`, `0038-pacing-does-not-change-guest-state`; `fetch_ps2recomp.sh --reset`:
  all 21 applied. `DON_JOBS=8 build.sh linux-dev`: `[build] linux-dev done in 0h 07m 05s`, peak memory
  6,853,844,992 bytes (6.4 GiB).
- Tests (before 0039): runtime `17 passed` (the two that were waiting included), host+input `20 passed`,
  audio+vuglue `7 passed`.
- Journal 017's check: `DON_TRACE_STATE` = 1, 2, 10, 60 on `work/det/quit70.txt`; the four `state blank=60`
  lines are identical: `eeCycle=334387800 iopCycles=40651185 iopInstructions=36027780 ramHash=b8accb615b71cdaf`.
- Against the side build (patches up to 0034 + 0037 + 0038): `DON_VU1=microvu`, `work/3d/menu_extra.txt`
  (the script that build's menu run used; 63 shots every 60 blanks) and `work/3d/match_long.txt` (156 shots):
  every file is byte-identical (`md5sum` lists compared). So patches 0035 and 0036 change no picture on these
  routes.
- Pictures: title crisp and complete (logo, three heroes, "PRESS START button", the crowd at the bottom, the
  light beams), SSIM 0.959 / MAD 5.3 against the console's replay of the same frame (the side build's figure,
  to the digit); the main menu has the tilted list with ひとりでバトスタ highlighted; the match frames show
  Naruto and Sasuke on the ship's deck with the sail, barrels and sea, health bars, one frame with the
  camera tilted looking down at the figurehead (that is the roll of journal 019, not yet fixed here).

## Part 2 (before 0039): measurements on this build, sound and no sound

Labelled **before 0039**; to be repeated on the build with it (below).

- The route of `work/audio/to_main_menu_sound.txt`, `DON_VU1=microvu --sound on`, three runs: exit 0, 81.1 s
  each (4860 blanks: 60.0 per second, the sound clock paces a run with sound), shots every 300 identical in
  the three, WAVs identical (same md5). **But the game is stopped at the title in every run**: the picture is
  the same from blank 2700 to 4860 (the title at the start of its fade-in; the Start tap at 3300 did nothing).
- The same script without sound: the title is complete at 2700 and animates to ~3120, then the picture is
  static, and START at 3300 (and again at 3420 for 20 blanks and 3600 for 60) does nothing: the game is
  stopped at the title without sound too, a few hundred blanks later. The routes of `to_main_menu.txt` and
  `menu_extra.txt` are not affected because their tap is at 2760, inside the animation. The older binary
  (14:46, patches to 0036) has the same behaviour with this script.
- `DON_TRACE_FRAMES`/`DON_TRACE_THREADS`: the frame loop's counter (`[don] frame n`, 0x138B00 per iteration)
  stops at **2280** with sound and at **2880** without, while `[don] vsync` goes on; the thread table at the
  end is the same in both: t1 ready at 0x31587C, t3/t4 waiting, t5 waiting suspended, one semaphore with count
  1 and no waiters. The batch experiment (`DON_IOP_BATCH_EE_CYCLES` = 8, 64, 512, unset, sound, route to the
  title and two taps): in all four the counter stops at 2280 and the picture at 2700, 3000, 3300 and 3600 is
  the same (the stop does not move with the batch size on this build).
- The route to the match with sound, three runs: exit 0, 144.2 s each (8640 blanks, 60.0 per second), 156
  shots, 114 of them the same stopped title; the three runs identical to each other and the WAVs identical.
- The route to the match **without** sound (`work/3d/match_long.txt`, shots every 60), three runs: exit 0,
  213 to 219 s (8640 blanks, ~40 per second under a load of about 10), and the 156 shots of each run are
  **byte-identical** to the side build's and to each other: all three pass "GET READY" and fight two rounds.

## Patch 0039: the code regenerated

- `0039-sign-branches-test-64-bits.patch` appended to `series`; `fetch_ps2recomp.sh --reset` applied all 22;
  `taskset -c 0-3 bash tools/pipeline/build_tools.sh` rebuilt the recompiler (16 s; `ps2_recomp` dated
  2026-10-13 16:01); `tests/runtime/test_branch_sign.py`: `1 passed` with it.
- `bash tools/pipeline/generate.sh` (39 s): audit exit code 0, `unhandled_total 0`, `unhandled_in_text 0`,
  3617 control-flow warnings (as the audit reports them); `[generate] sync: 0 added, 4786 changed, 0 removed,
  52938 left alone`. `grep -rlE 'GPR_S32\(ctx, [0-9]+\) *(<|>|<=|>=) *0' generated/` now finds **0** files;
  7,538 sign tests against zero are emitted as `GPR_S64`.
- `DON_JOBS=8 bash tools/pipeline/build.sh linux-dev`: `[build] linux-dev done in 0h 07m 08s`, peak memory
  6,831,710,208 bytes (6.4 GiB).
- `pytest tests/runtime -q` (rt-tests rebuilt before, series up to 0038; test_branch_sign.py included): `18 passed`.
- Journal 017's check on this build (`work/det/quit70.txt`, `DON_TRACE_STATE` = 1, 2, 10, 60): the four
  `state blank=60` lines are identical: `eeCycle=334387800 iopCycles=40651185 iopInstructions=36027780
  ramHash=b752953ed2040cc3` (the hash differs from the build without 0039, `b8accb615b71cdaf`: the guest
  computes differently by that blank, as expected; the cycle counts are unchanged).
- The route to the match (`DON_VU1=microvu`, no sound, headless software renderer; `work/integ/roll_route.txt`
  = `match_long.txt` up to the stage confirmation, shots every 60 blanks to 6540, then every 5 blanks from 6600
  to 7400; exit 0; shots in `work/shots/int-roll0039/`): `work/roll/classify.py` over the 161 shots
  (`work/integ/cls-0039.txt`) gives one value of 0.85 or more, blank 6600 (0.87), and that shot is the "FIGHT"
  banner (the same value, 0.87, was in the build without the fix: not a roll); before the fix 32 of the 161
  were at 0.85 or more. The lowest values (7050-7055: 0.16-0.20) are a cut-in with horizontal speed lines.
  A contact sheet of twelve shots (6600, 6685, 6720, 6745, 6770, 6800, 7070, 7180, 7260, 7290, 6900, 7400;
  `work/integ/roll_sheet.png`) shows the view upright in every one, including the blanks that were rolled
  before (6685, 6720, 6745, 7070, 7180, 7260, 7290): the fighters stand on the deck, the sail and horizon are
  level, and the match goes from "GET READY" to "FIGHT" (banner at 6600) into a fight with a special move
  (lightning, blank 7070) and a knock-back onto the figurehead.
- Not done on this build (cut by the orchestrator's instruction): the comparison of the pictures with the
  console's, the runs with sound and their speed tables, the repeated runs, Part 3.
