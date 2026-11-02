# 030 · The main mode, first visit (Battle Stadium Mode), with sound

- **Date:** 2026-10-13
- **Task:** T-320 / T-304
- **Status:** resolved (a visit; one open observation about the sound, below)

## Symptom
Only the practice match had ever been played. This entry is the first look at the main mode, as a player
would see it, on the English variant with Japanese voices (`en_patch/jpvoice/disc/`), headless, software
renderer, `--sound on`, after patch 0054 (journal 029).

Form of every run (scripts in `work/soak/`, not versioned):
`DON_TRACE_FRAMES=1 DON_PLAY_MOVIES=1 build/linux-dev/don --headless --renderer software --sound on --wav W --shots D --shot-every 60 --seconds N --input-script S <disc>/SLPS_256.75 2> LOG`

| run | script | binary md5 | blanks | liveness |
|---|---|---|---|---|
| r1 | to the character select | b18c6ebe... | 3600 | ok |
| r2 | + difficulty, loading, intro, fight start | b18c6ebe... | 4600 | ok |
| r3 | + fight pattern | b18c6ebe... | 14000 | ok (largest gap 120) |
| r4 | + the whole of match 2, the options screen | b18c6ebe... | 30000 | ok |
| r5 | + continue prompt | b18c6ebe... | 34000 | ok |
| r6 (longest) | route to the second continue prompt and past it | 72a4f6c0b70f528e7c3b7a518784cf93 | 38000 (633 s) | `frame=37560 vsync=37980 ... largest gap 120 vsyncs (limit 300): ok` |

(the binary was rebuilt between r5 and r6; r1-r5 used b18c6ebe6193bc79e83687179c36590b.)

## The route, screen by screen (blank numbers are from r1-r4; they move with how long a match lasts)
1. ~240 memory-card dialog (cursor on NO: Left, Circle = YES). Then health warning, logos, opening video
   (Start at 2400), title (Start at 2760). Title shows an English subtitle block ("Translated by ...").
2. ~3000 main menu (a grey transition frame was caught); Circle on the first entry, **Battle Stadium Mode**.
3. ~3200 **character select**: grid of portraits (Naruto, Sasuke, Goku-like, Luffy, Sakura, Chopper, Piccolo-like,
   Sanji-like...), "1P NARUTO" highlighted by default, footer "CHOOSE / SELECT / RANDOM / KEY CONFIGURE / BACK".
   Circle = Naruto.
4. ~3300 **DIFFICULTY** box (EASY / NORMAL, EASY first). Circle = Easy.
5. ~3540-3600 "NOW LOADING ..." over the arena entrance, then "Please push START button" (the intro: it
   advances by itself, no key needed): Naruto close-up, LEE close-up, a darkening, the two sides in the arena.
6. ~4300 **match 1, Naruto (1P) against two COM fighters, Lee and Sakura**, on the waterfall stage: "FIGHT",
   timer from 180. Health bars, names, timer, all readable and English.
7. Match 1 ends with a "K.O." caption (checked in r4 at ~7400 with the first pattern), then "WINNER" (Naruto), the
   winner close-up over the arena, then loading.
8. **Match 2: Naruto against Luffy and Usopp** (two COM), a temple courtyard stage. Items appear (bombs,
   spiked balls, a chest, item icons), a special-move cut-in with a red background (r5, ~20460), the camera
   tilts and zooms (the roll noted in docs/RUNNING.md).
9. In r4 and r5 match 2 ran to "TIME UP", "WINNER" (Luffy), then a **CONTINUE? Yes / No** screen over a
   Naruto close-up. The cursor starts on No and stayed there for 4000+ blanks with no input (no countdown
   seen). r4's later random presses chose No: back to the main menu and, from there, the options screen
   (AUDIO MODE, BGM, SE, AUTO SAVE, SAVE, LOAD; footer "BACK TO MENU").
10. r6: Left + Circle on the prompt = **Yes**: a Naruto close-up, then match 2 again with a fresh timer (177).
    Played to the end of the run (timer 56 at the last shot).

## What worked
- Every screen above drew and answered. The frame loop never stopped in any of the six runs (the longest,
  r6, 633 s; the largest gap between frame advances is 120 vsyncs).
- Match to match transitions (loading, intro, the next stage) work without help.
- The fighter reacts: walking, jumping, combo, special moves, a hit stun, items.

## Sound (r6 per minute, rms of 16-bit, peak up to 20459)
`wav_stats.py` on r6: duration 633.35 s, rms 0.044646, silent 0.624844, saturated 0.000000. By minute: rms
1314, 2264, 2102, 2019, 1735, 388, 945, 1042, 876, 1157, then 0 in the last 33 s. Silent seconds: the first 40 s
(the console's own, before the title), then in the fights most seconds only carry short bursts (match 1:
seconds 115-143 mostly sound; match 2 from second ~145: runs of 10-20 s of exact zeros between bursts, the
bursts all with the same peak 12539 or 13042).

## Observation, not a diagnosis: no continuous music in the fights
Match 1 has sound most of the time; match 2 and the replay are silent for long stretches. The same peak value
repeating suggests sound effects or voices, not music. Journal 010 already recorded that the menu music stops at
the first BGM stop/start and does not come back. Not examined here (no code or traces were changed); it needs a
`DON_TRACE_SPU2=1` run through the stage change. The same run was not done without my random Circle/Triangle
presses, so a connection to the inputs is not excluded.

## First thing that does not work (or is unproven)
Nothing froze, crashed or was garbled in the 633 s. The first deviation is the sound above (the fights have no
music); the second is the CONTINUE? screen having no visible countdown (it may be the game's behaviour).

## Not verified
- Match 3 and later (never reached: I lost match 2 on time in every run, the pattern does not reach the
  opponents reliably), so no prize, ending, save prompt or name entry was seen.
- Survival Challenge was not entered (time box).
- Whether the length of a match varies by run: match 1 was over by ~7400 in r4 but still going at 7980 in r5/r6
  (different inputs, so not a determinism test).
- Gamepad, window and GPU renderer: not used.

## Temporary diagnostics
None added. Scripts: `work/soak/gen.py`, `gen2.py`, `gen3.py` (generators), `run.sh`; **the longest working route
is `work/soak/r6.txt`** (generated by `gen3.py 38000`), plus `sheet.py` (contact sheets) and `wavseg.py`.
