# 023 · Videos skipped and microVU by default

- **Date:** 2026-10-13
- **Task:** T-113, T-205
- **Status:** partial (the title freezes if it is left alone, see below)

## Symptom
In a window the owner got a white screen after the logos (the opening video, which the port cannot
decode: it waits for Start), and had to type `DON_VU1=microvu` to get a right picture (the runtime's
interpreter gives up on the menu microprograms, journal 008).

## Investigation
- `0x36AC70` (`if (index < 2) 0x36A820(table[index]); return 1;`) is the handler of script opcode `0x50`
  in `0x1BC420`; its only caller is that case and the player `0x36A820` has no other caller (checked
  with `tools/re/callers.py`). Index 0 = OP (the opening, also the attract loop), 1 = ED (the ending): both
  go through this one function. `0x1BC420` ignores the result: it takes the operands from the script,
  calls, and returns the new script pointer. So returning 1 at once runs the game's own code after a
  video unchanged. What the player would leave behind (GS reset to 640x224 and the VBLANK callback
  `0x331F20` installed by its last call) is not needed: the title came up correctly in the run below.
- Replaced `0x36AC70` in `src/game/don_overrides.cpp` (`donSkipMovie`). `DON_PLAY_MOVIES=1` keeps the
  original. One call is made per run up to the title (index 0, a1 = 1, at about blank 2000).
- `DON_VU1` unset now means microVU; library not found: one `[don-vu]` warning and the runtime's
  interpreter. `DON_VU1=runtime|interp` is the old default.

## Measured (English variant, no variable set, software renderer, blanks of the run)
- 240 memory-card dialog; Circle at 330; health warning ~600; logos ~900-1980; one white shot at 2040;
  the title fades in at ~2060-2100, the stadium behind it is drawn (3D works with no `DON_VU1`).
- Start tapped at blank 2400 (title 300 blanks old): the menu is up at about 2640-2700.
- Title left alone: the picture animates until about blank 2460, then is **byte-identical in every shot to
  the end of the run (4560)**: the game thread keeps polling the memory card (`GetInfo`/`Sync` with
  result 0, after loading one more IOP module and a `GetDir` of the save folder) and Start at blank 3960
  did nothing in the following 600 blanks. The opcode is not called again (checked with a temporary
  print), so this is not the attract video. With the game's own player (`DON_PLAY_MOVIES=1`, Start at
  2400 to leave the video) the title alone for 3300 blanks (to blank 6000) did NOT freeze. Cause: not determined.
- `DON_PLAY_MOVIES=1` with `tools/run/scripts/to_main_menu.txt`: `title` is the title, `main_menu` (blank
  3000) is a grey diagonal transition of the title logo, not yet the menu.

## Follow-up 2026-10-13: the first cut was wrong; the port now presses Start for the player

- **What was wrong.** Returning 1 from `0x36AC70` never ran the player, and journal 024 showed that the title then
  freezes. The cut is removed.
- **What replaces it.** The player runs unchanged. The host holds Start and Circle for the game while the player
  runs, from 10 blanks after it began (`src/host/video_skip.h`, the third source of player 1's pad next to the devices
  and the script, merged in `host.cpp`'s vblank hook; nothing is written into game memory). `DON_PLAY_MOVIES=1`
  installs no probe, so nothing is pressed.
- **How the port knows the player runs.** A wrapper that calls the original of `0x36A820` does not work: the generated
  functions return early at scheduler checkpoints and the dispatcher resumes them at a label, so the wrapper saw the
  call return at once (measured: enter and return within one blank, black screen afterwards). The probe reads the
  guest word `0x55A860` (the pool block the player takes before its loop and zeroes after it).
- **Measured (idle run, nothing pressed after the dialog):** player at ~1990, black at 2040, title fading in at 2100,
  full at 2160, no white wait. **The title still freezes**: its picture is byte-identical from blank ~2280 on (to 6000).
  Start at 2160 only reached a grey menu-transition frame at 2700; Start at 2700 did nothing. So the freeze is not
  caused by skipping the player; with `DON_PLAY_MOVIES=1` and a Start tap at 2400 the title did not freeze to 6000. Open.

## Follow-up 2: the delay before the port's press (`DON_MOVIE_SKIP_AFTER`) - the hypothesis did not hold

`DON_MOVIE_SKIP_AFTER=<blanks>` (default 240) is how long the player must have run before the port holds Start/Circle.
English variant, nothing pressed after the dialog (idle runs); the player starts at blank ~2032; the player's screen is
black, the title appears (white flash, then fade) about 20 blanks after the press. "Frozen at" = the last shot that
differs from the previous one (shots every 60 blanks).

| delay | player ends | title up | idle: frozen at (title + n) |
|---|---|---|---|
| 10 (first run) | ~2050 | ~2100 | ~2280 (+180) |
| 60 | ~2100 | ~2120 | 3120 (+1000) |
| 120 | ~2160 | ~2180 | 2880 (+700) |
| 240 | ~2280 | ~2300 | not frozen to 4260 in one run; frozen at 4440 (+2140) in another |
| 400 | 2433 | 2460 | 3120 (+660), twice (deterministic) |

With Start tapped 600+ blanks after the title: delays 240 and 120 opened the menu (taps at 2940 and 2820); delay 400 (tap at
3100, after its freeze) did not. The known-good timing (400, the script's Start at 2400 of the old routes) therefore does NOT
reproduce a working title when the port presses at 400 and nobody touches the title afterwards: the hypothesis "ending the
video early leaves the middleware in a bad state" is not supported. The freeze comes later or earlier but, in these
runs, always comes if the title is left alone; the old routes may only have worked because they pressed Start within
~300 blanks of the title. Japanese disc with delay 240, Start at 2940: the picture's last change is at 2340, no menu.
Cause open. No default works for an idle title; 240 is kept as the best seen.
