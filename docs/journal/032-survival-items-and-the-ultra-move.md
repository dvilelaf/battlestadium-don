# 032 · Survival, items and the ultra move, with sound

- **Date:** 2026-10-14
- **Task:** T-320
- **Status:** resolved (exploration; the unchecked points are listed)

## Runs
English variant, software renderer, `--sound on --wav`, `DON_TRACE_FRAMES=1`; with sound a run goes at real speed.
Scripts, shots, wavs in `work/t320/` (not versioned). Binaries: s1, s2 and the first look `46c8bc858837`; u1 (practice
ultra) and t1 (Tickets) `48225ee838b3` (a first u1 run was spoilt by my own script: the head's relative lines were
re-sorted and the memory-card dialog never got its answer; not counted).

| Run | What | Liveness | WAV |
|---|---|---|---|
| s1 | main menu to the first Survival fight, to blank 5100 | frame=4560 vsync=5040, largest gap 180: ok | 85 s, rms 0.078, silent 0.51 |
| s2 | Survival with a fixed attack loop, to 11700 | frame=10320 vsync=11640, largest gap 180: ok | 195 s, rms 0.082, silent 0.55 |
| u1 | practice, Stand, hits then Circle, to 8350 | frame=7680 vsync=8340, largest gap 180: ok | 139 s, rms 0.073, silent 0.63 |
| t1 | Tickets Mode and back | frame=4020 vsync=4440, largest gap 180: ok | rms 0.055, silent 0.73 |

(The "silent" fractions include the ~30 s of the start with the videos skipped and the memory-card dialog.)

## Survival Challenge (second entry of the main menu)
- The menu's subtitle: "BATTLE WITHOUT HEALTH RECOVERY". Circle goes through the DON logo screen to the character
  select with a "SURVIVAL SCORE" panel (Challenger, Character best score N stage, Best score N stage; both 0 at first).
  Circle on the highlighted fighter (Naruto here) goes straight to loading ("NOW LOADING", then "Please push START
  button"), the fighter's name card (NARUTO), "GET READY" and the first fight: no stage choice, no opponent choice.
- Fight 1: Naruto against Gaara (a Com), a canyon with a waterfall. About 2000 blanks of fight; Naruto's bar never
  fell; Gaara's emptied; "K.O.", "WINNER" with Naruto's pose (blank ~6000-6240), then loading again.
- Fight 2: Going Merry against Nami. Naruto's health was whatever remained (no recovery between fights). "K.O." at 7560
  with Nami the WINNER (a loss), her pose.
- Result screen: "RESULT: Time score 1 Stage, Character best score 0 -> 1 Stage, Best score 0 -> 1 Stage" (the figures
  count up). Then the Survival entry of the main menu again (not the character select). Fights chain until a loss;
  health is not restored in between.
- The fight loop was Square x3, Triangle, L2 held, R1, Circle, Cross; nothing was timed per fight.

## The ultra move (Circle)
In practice (opponent Stand), after hits given, Circle gave a full-screen red cut-in with Naruto's face and a blue
swirl hit, 10 blanks after the tap (blank 7290 -> 7300; again 8190 -> 8200). Earlier taps without hits did nothing. What
fills the gauge (hits given, hits taken, guard) was not isolated; the bar under each portrait showed a partly filled
bar, not compared pixel by pixel.

## Items and throws
- Capsule-like red items drift on the stages in every fight; a yellow box with an arrow appeared near Naruto at 8120-8180
  in u1. Nobody walked onto one on purpose and no pickup or use was seen: **not checked**.
- R1 (grab/throw) tapped at close range: nothing recognisable as a throw in the shots; not established.

## Tickets Mode (fifth entry)
Subtitle "FIGHT USING TICKETS!!". Circle: logo transition, then "Please select tickets you want to use": page 1/2 of a list
with counts (Gravity double! x4-5, Half gravity x5, Speed UP x5, Speed Down x5, infinite special moves x0, Blow away! x0),
a description pane, and "Challenge level" stars (1 star with a ticket chosen, none with none). Circle toggled the first
ticket (count 4, one star; again: 5, no stars). L1/R1 turn the page (not tried), Start finishes (not tried), Cross backs
out to the menu (checked). The game does not ask for anything it lacks: the list is stocked already.

## Sound
By the wav (5 s windows, rms): silence in the first 30 s (the skipped videos), 0.05-0.11 in the menus of Survival, 0.11-0.16
in fights, 0 in the result screen (blanks ~8100, one window) and again at some loading screens; no stop in the middle of a fight
was seen (windows during fights never fell to 0). Not listened to; only levels.

## Anything wrong
Nothing visibly wrong in these runs: no frozen or garbled picture, text English, no crash. The wavs' window positions
are wav seconds, which drift from blank counts by the loading time.

## Follow-up 2026-10-15 (measurement only, no code): items and throws are still **not reached**

Player folder `build/player/don-linux/don` (commit a66dc2f), English variant `en-jpvoice`, headless,
software renderer, sound off, own cards (`--mc-dir work/t-item/mc-*`), `DON_PLAY_MOVIES=1`. Scripts, shots and the
small analysis tools in `work/t-item/` (not versioned). Result: the three points of "Items and throws" above stay
**unchecked**; nothing here contradicts them.

- **The game's own list.** Pause menu (Start) then Down, Down, Circle: the SKILL LIST (shots `skill_1`..`skill_3`).
  It lists only attacks (Naruto: basic combo, upper, hard punch, sweep, slam, dash punch, giant windmill, shadow
  clone, Uzumaki rising dragon / barrage / summoning, rising assault, exploding dive, Rasengan; Sasuke: six moves),
  two columns (1P, Com) and two empty ones; footer `L1 R1 SWITCH PAGE  START RETURN  *FOR KEY CONFIGURATION TYPE A`.
  **No item, grab or throw entry**: for those the only source remains the key configuration (journal 026: layout A,
  R1 "Grab/Throw item", Square "Pick up/use item").
- **Where the route lands is not robust.** The same script (the first 6500 blanks of `tools/run/scripts/fight_practice.txt`,
  i.e. `to_match_1v1.txt` up to `match_load_2`) gave the 1v1 practice (Naruto vs Com Sasuke on GOING MERRY; stage
  select md5 `509a2bd9`, the same as `work/t320/u1`) in two runs and the four-fighter match the fight_practice header
  describes (NARUTO, PICCOLO, GAARA, VEGETA on BARATIE RESTAURANT, timer 176; stage select md5 `a7afa375`, the one
  recorded in `tests/regression/routes.json` for `fight_practice`) in a third. The third run was the one with
  `--shot-every 10` and without `DON_TRACE_FRAMES`; in it the shot at blank 3060 is still the title, so one press of
  the route was consumed elsewhere and the menu cursor travelled further down. Do not assume which mode a script
  reaches: shoot the stage select and the first fight frame and compare their md5 with `routes.json`.
- **Items.** Red balls exist and move: on the GOING MERRY deck one drifts left through the air at about y 120-148
  (u1 frames 7500-8160, next to the yellow box with an arrow at 8120-8180), and on BARATIE they appear at the
  fighters' feet and roll left along the floor (R3: two balls beside Naruto at 7240-7260, at x 281 then x 192 twenty
  blanks later). In the R3 run they were also seen near the Coms (Piccolo, at 8000-8180). So the objects and their
  motion are alive in the port; who moved them (a Com, or the ball rolling) was **not** isolated.
- **What was tried on purpose** (R3, shots every 10 blanks): `R1` taps at 7080 and 7160 while walking right into
  Piccolo; `R1` held 7300-7360; `R1` taps at 7440 and 7520; `Square` taps at 7600 and 7660; then nine
  jump (`Cross`) + `R1` pairs (7940, 8000, 8080, 8300, 8380, 8600, 8680, 8920, 9000) with left/right walking
  between them, across the middle of the deck.
- **What the pictures show.** No frame shows a fighter carrying anything above his head and none shows an object
  leaving a fighter's hands; no opponent is lifted or held. The two times the button met an opponent, Naruto was
  already in Piccolo's attack: at 7240 the white hit effect is on him and at 7260 he lies on the floor (the
  `R1` of 7300-7360 came in that knock-down, the same trap as the `rB` run of journal 026), and the nearest balls
  rolled away to the left. At 8060-8180 Naruto is beside the drifting ball but in the air (his pink 1P marker ring
  is drawn, journal 026) while `Cross`+`R1` were being sent.
- **Verdict.** Pick up an item: **could not tell**. Throw an item: **could not tell** (nothing was ever held).
  Throw an opponent: **could not tell** (never pressed with an opponent adjacent and Naruto free). Liveness of the
  run: exit code 0, the fight played to blank 9200 (timer running), no stop of the kind journal 026 describes.
- **Next.** A clean attempt needs a moment with Naruto standing, the Com at Stand (pause menu) and either an item
  on the floor next to him or an opponent right beside him: on GOING MERRY `Com state STAND` leaves Sasuke at his
  start, so walk into him with a short `Right` hold (a 30-blank hold covers about half the deck) and press `R1`
  only after the walk has stopped; for the ball on the floor, wait for it to roll under Naruto and press `Square`
  and `R1` while he stands (not while the marker ring is up).
