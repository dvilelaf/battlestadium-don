# 026 · Can the fighter be controlled? Partly: Left and Right yes; the rest not established; a fight freeze at blank 7080

- **Date:** 2026-10-13
- **Task:** T-202
- **Status:** partial

## Runs

Route: `DON_PLAY_MOVIES=1`, `tools/run/scripts/to_match_1v1.txt` up to its `match_load_2` (6540), Japanese disc folder,
sound off, Naruto (1P) against Sasuke (Com) on the ship. Scripts and shots are under `work/control/` (not versioned).
A shot every 10 blanks from 6680 to 7510.

- `base`: no input after "FIGHT". 
- `ctl`: Right held 6700-6759; Left held 6820-6879; Cross tap 6960; Triangle tap 7020; Square taps 7100/7120/7140;
  Circle taps 7200/7220/7240; Right held 7340-7399 with a Square tap at 7345; quit 7520. Run twice (`ctl`, `ctl2`).

## What the shots show

| Input | Blanks | Player 1 | Baseline at the same blanks |
|---|---|---|---|
| Right (held) | 6700-6760 | Naruto dashes toward Sasuke and past him (the camera turns), then runs to the right of the deck at 6740-6770 | stands in his guard pose |
| Left (held) | 6820-6880 | Naruto, standing until 6820, jumps/runs left at 6830-6850 and collides with Sasuke at 6860-6870 (**the opponent was also attacking**) | stands still |
| Cross tap | 6960 | no distinct jump seen; Naruto is in a melee exchange with Sasuke (as the baseline also is at 6940-6970) | Sasuke's super cut-in at 6980-7050; the fights diverge here |
| Triangle tap | 7020 | not distinguishable from the exchange | |
| Square, Circle taps, Right + Square | 7100-7400 | **not observable: the picture is frozen from 7090** | |

Controlled: yes for Left and Right (the pad reaches the fighter and he moves the right way at once, no sign of a
stuck direction after the release; the exact frame of the release was not checked). Not established: which button is
the jump and which are the attacks (the exchange starting at 6940 is the opponent's as much as player 1's).

## The freeze

In `ctl` (twice, identical) the picture is byte-identical from blank 7090 to the end (7510), a hit spark on both
fighters; `base` keeps changing to 7510. The vertical blanks keep counting (`DON_TRACE_FRAMES=1`, run `ctl2`: vsync 7380,
7440, 7500). So it is a stop like the title's (journals 011, 024), here during a fight, and it is deterministic for this
input script. The shots after 7090 therefore say nothing about Square, Circle or the combined input.

## Next

Run the inputs one at a time from "FIGHT" (e.g. only Square taps, 40 blanks apart) to see an attack animation before
the stop; find what is common between this stop and the title's (journal 011: the main thread waits for its display-list
packet queue).

## Follow-up 2026-10-13 (T-304): are the stops the same wait? Yes, the same reading in the fight and in the title

Reading of journal 011's wait, from RAM images (`DON_STATE_DUMP`, needs `DON_TRACE_STATE=<same blank>`; the dump is
taken at the game's own tick, which runs a few blanks behind the vsync count, so the script must not quit before it:
quit 300 blanks later). Words are little-endian; the byte is 0x4C9F00.

| run (image at blank) | word 0x4C9F08 | word 0x4C9F0C | byte 0x4C9F00 | spin 0x4D6900 |
|---|---|---|---|---|
| fight, control script, stopped (7200) | 0x005A9F08 | 0x005A9F10 | 0 | 1,474,540 |
| fight, control script, stopped (7260) | 0x005A9F08 | 0x005A9F10 | 0 | 6,662,735 |
| fight, baseline, healthy (7200) | 0x005A9F00 | 0x005CF0A0 | 0 | 75,739 |
| fight, baseline, healthy (7260) | 0x005A9F00 | 0x005CEB58 | 0 | 77,048 |
| title idle, no sound (4700) | 0x005A9F08 | 0x005A9F10 | 0 | 7,917,985 |
| title idle, no sound (4760) | 0x005A9F08 | 0x005A9F10 | 0 | 3,105,310 |

- Fight stop: the two words differ, are identical in both images, and the counter is 20 to 90 times the healthy
  value and different between the images (it moves, and may wrap): the same wait as journal 011's stopped state.
- Title stop (no sound): the same two values as the fight stop, the counter huge and moving. In these runs the
  picture had stopped (last change at 4440 in both idle runs, shots 60 apart), so the images were taken in the stop.
- Healthy baseline: the second word changes between the images and the counter stays in the tens of thousands (the first
  word is also 0x5A9F00 here, so a stop is "first word 0x5A9F08 and second 0x5A9F10", not just "words differ").

Not verified: that the pending entry is the same one in all three (only the values of the two words match), what the
interrupt that should pump the queue is, and that the healthy counter is always small (two samples).

### The shortest reproduction of the stop (fight, no sound, deterministic: the picture is byte-identical from 7090)

Script `work/control/ctl.txt` (not versioned): the route of `tools/run/scripts/to_match_1v1.txt` up to and including
`+300 shot match_load_2` (6540), then these lines (absolute frames):

```
6700 press RIGHT      6760 release RIGHT    6820 press LEFT       6880 release LEFT
6960 tap CROSS 6      7020 tap TRIANGLE 6   7100/7120/7140 tap SQUARE 6
7200/7220/7240 tap CIRCLE 6                 7340 press RIGHT      7345 tap SQUARE 6     7400 release RIGHT
7520 quit
```

Command (about 3 minutes; the picture stops after blank 7080):
`DON_PLAY_MOVIES=1 build/linux-dev/don --headless --renderer software --shots work/control/ctl --shot-every 60 --input-script work/control/ctl.txt --seconds 1200 disc/SLPS_256.75`

## Follow-up 2026-10-13 (T-304): the scheduler's old path (`DON_IOP_BATCH_EE_CYCLES=8`) does not avoid the stop

Suspicion: patch 0034's fast path leaves a completed transfer's interrupt unseen. Comparison: the same runs with
`DON_IOP_BATCH_EE_CYCLES=8` in front (the slow path on every dispatch). Both runs had `DON_TRACE_STATE` and
`DON_STATE_DUMP` for a RAM image; the run speed was about 13 blanks per second instead of 45.

| run | variables / script | picture kept changing until | wall time | queue words 0x4C9F08 / 0x4C9F0C, spin (image) |
|---|---|---|---|---|
| fight route (`work/control/ctl.txt`), Japanese folder | `DON_IOP_BATCH_EE_CYCLES=8 DON_PLAY_MOVIES=1` | **blank ~4020** (the main menu, with the script's `menu_again` at 4140 still to come: the route never reached the fight; all 84 shots from 6680 to 7510 are the same menu picture) | 589 s (to 7520) | 0x0069CD88 / 0x0069CD90, 6,887,698 (blank 7400) |
| idle title (dialog answered, then nothing), English variant | `DON_IOP_BATCH_EE_CYCLES=8` | **blank 4440**, to the end of the run (4980) | 398 s (to 5000) | 0x005A9F08 / 0x005A9F10, 7,917,985 (blank 4700) |

- Fight stop: **not gone**. With the old feed the route stops earlier (at the menu, ~4020) than the 7080 of the fast
  path, so the first look at the jump and attack inputs did not happen. The two queue words differ by 8 as in the other
  stops but sit at 0x69CD88 / 0x69CD90 (the pending entry is elsewhere), and the spin counter is huge: the same wait.
- Title stop: **not gone**; same blank (4440) and the same words as the fast-path run.
- So the end-of-transfer interrupt is not lost only because of the fast path's flag. The old path moves WHERE the stop
  happens in the fight route (earlier, at a screen change) but not whether it happens.
- Not verified: that the batch variable disables the fast path completely (read from the task text, not the patch); runs
  of the fight route with the old path from a different starting point; whether the stop at ~4020 is the same blank in
  a second run.

- 2026-10-13: the picture of how the port executes the game's code and of the stop, put together from this and the neighbouring entries, is in [knowledge/20-execution-model-and-the-stop.md](../knowledge/20-execution-model-and-the-stop.md).

## Follow-up 2026-10-13 (T-202): what each button does in a fight

Binary `build/linux-dev/don` md5 `b9a0350b...` (before patch 0054). English variant (`en_patch/jpvoice/disc/SLPS_256.75`),
no variable (videos skipped, delay 240), software renderer, sound off. The route is `to_match_1v1.txt` with Start at
2640 instead of the video tap and the title tap (the match then goes live about 6480; scripts in `work/t202/`, not
versioned). No run froze.

### Screens found

- Main menu (Start at 2640; ready at ~2940): Battle Stadium Mode, Survival Challenge, Practice, Multiplayer, Tickets
  Mode, Options (wraps: Up from the first item reaches Options).
- Options (Up, Circle): Audio mode (Stereo/Mono), BGM, SE (Off, 1-4), Auto save (On/Off), Save, Load. **No button
  configuration.**
- Pause menu in a fight (Start): Resume, Com state (Stand/Auto), Skill list, Reset, Reselect.
- Skill list (pause menu, Down twice, Circle): Naruto's and Sasuke's moves with their buttons; the footer says
  "*for key configuration type A". Scroll with Up/Down, L1/R1 switch page (R1 changed nothing visible), Start returns.
  Naruto: basic combo Square x3; Upper Up+Square; Hard punch Forward+Square; Sweep Down+Square; Slam (mid-air) Square;
  Dash punch (mid-dash) Square; Giant windmill Triangle (holdable); Shadow clone jutsu hold Triangle; Uzumaki rising
  dragon Up+Triangle; Uzumaki barrage Forward+Triangle; Summoning Down+Triangle; Giant windmill (air) Triangle; Rising
  assault (air) Up+Triangle; Exploding dive (air) Down+Triangle; Rasengan Circle (holdable). Sasuke: basic combo Square
  x3; Double kick Up+Square; Vertical fist Forward+Square; Back kick Down+Square; Roundhouse kick (air) Square; Forward
  roll (mid-dash) Square. Jump, guard and dash are not on the list.

### Runs (one kind of input each, compared with the no-input baseline `rD` at the same blanks)

Sasuke's super cut-in at 6740-6830 is the same in all runs and knocks player 1 down until ~6870, so inputs sent in
that window do nothing visible (the picture is byte-identical to the baseline until the first listed difference).

| Run | Input | First picture differing from the baseline | What player 1 did |
|---|---|---|---|
| rA | Cross taps 6700, 6780, 6860; Up taps 6960, 7020 | 6870 (after the Cross at 6860) | Cross at 6860: Naruto comes out of the knock-down in the air (his marker circle is shown, he stays up) instead of lying on the deck; one run, cause not certain. Up at 6960 and 7020: Naruto is high in the air at 6980 and 7040 (baseline: standing): a jump |
| rB | R1 held 6700-6820, L1 held 6900-7020 | none to 7060 | nothing visible (R1 was held in the knock-down window, so it says nothing; L1 held while standing changed nothing) |
| rC | Square taps 6700-6820 (4), Circle 6900, Triangle 6960 | 6930 | Squares in the knock-down window: nothing. Circle at 6900 (Naruto on the deck): no clear effect. Triangle at 6960: at 6980 Naruto is inside a spinning move with a large shuriken shape (the Giant windmill) |

Not checked: Square attacks while standing (the knock-down window covered all of them), Circle's Rasengan, R1, L2, R2,
Cross while standing, guard and dash, and the second player. The Cross effect and the jump rest on one run each.

## Follow-up 2026-10-13 (T-202, second): the buttons still unknown, and a long fight

Binary `build/linux-dev/don` md5 `b18c6ebe6193...` (with patch 0054) for every run below; it did not change. English
variant, software renderer, sound off, scripts in `work/t202/` (not versioned). No run froze: all ran to the end of
their scripts (the long one to blank 14940).

- **Com state.** Pause at 6500 (Start), Down, Right, Start: the menu shows `STAND` changing to `AUTO`; the setting does
  what it says: with Stand, Sasuke stays at the far right, no kick at 6690, no super at 6740, and Naruto is free.
  Set on Stand for runs qa/qb/qc/long; on Auto (no change) for the guard runs.
- **qa (Stand).** Square at 6760: an arm punch, first seen at 6775 (15 blanks). Three taps at 6820/6832/6844: punches,
  ending in a kick at ~6875. Up+Square at 6903: a different pose from 6910 (7 blanks), arm raised at 6940. Forward+Square
  at 6963: a lunging kick, impact spark at 7005. Down+Square at 7023: Naruto crouches from 7030. (Identification of the
  upper and sweep is by the pose only.)
- **qb (Stand).** Circle tap at 6760 and Circle held 6820-6880: nothing visible (the stance does not change). Triangle held
  6940-7000: sparks around Naruto from 6955 (15 blanks), a dash with shurikens after the release at 7000.
- **qc (Stand).** Cross at 6760: airborne at 6770 (10 blanks), lands ~6790: a jump. Right held + Cross at 6815/6820: a
  forward jump, airborne 6835-6860. Right tapped twice (3 blanks each, 8 apart) at 6880: Naruto runs (running pose from
  6885). Down held 6940-7000: crouch from 6950, stands again ~7010.
- **guard (Auto, one button held 6560-6700, baseline `ga` with no input).** L1 and R1: pictures identical to the baseline
  (R1: differs from 6600 by a few pixels only); Sasuke's kick still hits at 6690 and the super knocks him down. L2 and R2:
  Naruto glows blue from 6600, yellow by 6660 (chakra charge); the kick did not hit, there is no super, and Sasuke
  walks about: it is a charge, not a block. No guard button was found; Cross and Down were not tried against an attack.
- **Long fight (Stand, 6760-15000: run right, three Squares, Triangle, every 240 blanks; a shot every 60).** The
  fight never ended in 8400 blanks: no knock-out, no round end, no result screen. It is the practice mode: the
  health bar of Sasuke refills (full at 14940), and the picture kept changing to the end of the script. So the end of a
  fight (versus mode) is still not seen. Naruto was thrown into the air and off-screen several times; the camera
  follows, and the scenery (ship) moves with it. Nothing looked wrong in speed (the same 45 blanks/s as before).
- Not checked: guard, Rasengan (needs another condition?), Sasuke, player 2, the versus-mode end.

## Follow-up 2026-10-13 (T-203): two players and the Multiplayer mode

Binaries: runs 1-2 (practice, P2) `b18c6ebe6193`; everything from the Multiplayer menu on `72a4f6c0b70f`; the guard runs
`6c03c4c49c11` (the binary was rebuilt twice during the work). English variant, software renderer, scripts in
`work/t203/` (not versioned). A `P2:` command anywhere in the script plugs the pad in.

- **P2 in practice.** With Com state on Stand and P2 connected but idle, Sasuke stays at the far right, no super, no
  kick (run p2c). With `P2:press LEFT` 6760-6840 Sasuke walked toward Naruto and attacked (`P2:tap SQUARE` at 6900 gave a
  hit spark at 6920): the second pad exists and drives Sasuke. The pause menu's Com state is only Stand/Auto; no way to
  make the 2nd fighter player-controlled was found there (the Multiplayer entry screen is the way).
- **Multiplayer screens** (main menu: Down x3 from the first item, Circle): the screen is "ENTRY EDITING!!" with four
  slots, 1P PLAYER, 2P PLAYER (when the P2 pad is connected), COM, COM; footer: scroll, Circle select/edit, Start
  finish editing, Cross back. Circle on a slot toggled the 2P slot PLAYER <-> COM (done with P2's Circle). P3 and P4 were
  connected by script (a tap each) and the slots 3 and 4 still read COM: no four-player offer was seen, and no text
  about a multitap (not tried: moving the cursor to slots 3 and 4 and pressing Circle). Start finishes editing and
  opens the character select (Naruto, Sasuke, Luffy, Zoro as the four fighters; its footer has a "Triangle: key
  configure" entry, "L1 R1 team/color switch", "Square random"), then a stage list (GOING MERRY and others), a
  loading screen ("Please push START button" shown once), and the match: four fighters, timer 180 and "GET READY".
- **A match to the end** (P1 and P2 scripted, Coms on Auto; to 18100): the timer counted from 180 at blank ~4700 to 000
  at ~15860 (about 62 blanks per timer second); then "K.O." (15980), "WINNER" (Luffy, a Com fighter, 16100), a victory
  pose (16220-16340), then the ENTRY EDITING screen again (16460), then the menu. My script went on pressing buttons,
  so the later screens (the Multiplayer menu at 16940, the title at 17160) are my inputs backing out with Cross.
  Both human fighters' portraits were red (knocked out) at the end. Liveness: `liveness.py` on the log of this run:
  `frame=17580 vsync=18060 ... largest gap 120 vsyncs (limit 300): ok`, exit 0.
- **P2 only / P1 only** (runs onlyP1, onlyP2, 4820-5200): with only P2 pressing, Sasuke ran to the far left edge and
  Naruto stayed near his start; with only P1 pressing, Naruto went off to the lower left and Sasuke stood and was
  knocked down by the Coms. This reads as separate pads, but the four-fighter fight is crowded and the shots small,
  so a leak of one frame or a late input is not excluded. Not measured: delays.
- **Guard (Auto, one input held 6560-6700, binary 6c03c4c49c11):** Left (back): Naruto walked off-screen, never hit;
  Down: hit by the kick at 6690, then the super; Cross: Naruto jumped and avoided the kick. No block animation seen.

## Follow-up 2026-10-14 (T-202): the key configuration screen and the guard

Binaries: the key config runs `7d0ccd442999`; the guard run `7d0ccd442999`, its baseline `780d0d3919c3` (rebuilt between
them). English variant, software renderer, scripts in `work/t203/` (k1, k2, bL2, bnone; not versioned).

- **Where.** On the Multiplayer character select (route: Down x3 and Circle in the main menu, Circle, Start) the footer
  reads "Triangle: KEY CONFIGURE, L1 R1: TEAM/COLOR SWITCH, Square: RANDOM". Triangle opens a "KEY CONFIG" panel at the
  left; Down/Up scroll the layouts (arrows at the panel's top and bottom), Left/Right toggle "Vibration ON/OFF",
  Circle closes it (checked). P2's Triangle opened the panel for 2P (the badge reads 2P), and P2's Down changed only
  2P's layout, so the choice is per player. Whether the choice is kept for the match is not checked. The main
  mode's and Practice's character selects were not looked at in this piece (Practice's skill list says "type A").
- **Layouts as printed** (A opens first; the letter is the big letter behind the panel): A: Square Normal move / Pick
  up/use item, Triangle Special move, Circle Ultra move, Cross Jump, R1 Grab/Throw item, L2 R2 Guard. B: Square Normal
  move / use item, Triangle Special, Circle Ultra, Cross Jump, R1 R2 Guard, right stick Pick up item / Grab/Throw
  item. C: Square Normal / use item, Triangle Special, Circle Pick up item / Grab/Throw item, Cross Jump, R1 Ultra move,
  L2 R2 Guard. D: Square Normal / use item, Triangle Jump, Circle Ultra, Cross Special, R1 Pick up item / Grab/Throw
  item, L2 R2 Guard. A fourth Down was not tried.
- **So the guard is L2/R2 (type A).** The 2026-10-13 shoulder-button runs had shown the aura and the missing hit, and
  I had called it a "chakra charge": it was the guard. Guard run (practice, Auto, L2 held 6560-6900, shots every 5
  from 6670): Sasuke's kick arrives at about 6685-6700; with L2 Naruto stays standing in a yellow aura, with no hit
  spark and no knock-down, and nothing happens afterwards (Sasuke walks away, no super in these frames); the same
  run with no input (`bnone`) shows spark at 6670, 6690, 6710 and Naruto knocked back. The bars look unchanged in
  the guard run; the pixel values were not compared.
- **Circle is "Ultra move"**: that is the Rasengan on the skill list. A tap and a hold did nothing visible, standing; it
  probably needs a full gauge (not checked).
- Not checked: layouts after D, whether the choice persists into the match, the other modes' footers, a held
  guard against a throw, item use.

## Follow-up 2026-10-14 (T-306): four players

Binary `c88db5e57788` for the runs (the regression suite ran on `ce723b9aed84`, rebuilt by another task; both with patch 0061), English variant, software renderer, sound off, `--players 4`; scripts in
`work/t306/` (not versioned).

- **How the game reads pads.** The poll `0x138FB0` asks the connection query (`0x255EA0`, replaced) for port 0. With
  `*a != 0` it closes ports 0 and 1 and opens port 0 slots 0 to 3 (`0x1189E8`) for each slot whose state query
  (`0x118EB0`) says 99, then reads (port 0, slot i) with `0x139390`. State 99 is the SDK's "not open"; the runtime
  stub answered 0, so nothing was reopened and no pad was seen. Patch 0061 fixes that; the replacement writes `*a = 1`
  for port 0 under `--players 4`. Pad states of slots 1 to 3 come from host pads 2 to 4.
- **Entry screen.** With P2, P3, P4 plugged in (Select taps) the slots 2P, 3P and 4P show PLAYER; with `--players 2`
  the same script leaves 3P and 4P as COM. Circle on a slot cycles it by the pad that owns it.
- **Match with four humans.** Character select with Circle from each of the four pads (four distinct fighters), a
  match of four fighters, each driven by its own script lines, ran to the result screen; `tools/verify/liveness.py`:
  frame=17700 vsync=18360, largest gap 240 (limit 300), ok.
- **Only P3 pressing.** Against an idle baseline, the first shots are identical; with P3 holding Right, only the third
  fighter (Luffy) walks off to the right and the camera follows; the other three stay where they were.
- Regression suite (`-m slow`): 4 passed, 4 skipped. Not checked: real gamepads, a keyboard-only player 1 with
  three gamepads.
