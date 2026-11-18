# 070 · Fast loading: loading screens run as fast as the PC allows (small gain)

- **Date:** 2026-11-18
- **Task:** owner request: loads between fights (story mode) feel long
- **Status:** done, experimental (measured gain is small)

## What the loads are

The port serves disc reads at once (no drive model), so a loading screen is mostly the game's own work and waits,
paced at 60 blanks per second like the console. Measured on `fight_short.txt` (`DON_FAST_LOAD_LOG=1`, a temporary
per-blank count of the bytes read): the game reads in bursts of hundreds of KB within a few blanks at each load
(menu transitions, NOW LOADING), while menu music streams far less.

## What it does

- Runtime (patch 0095): `EeScheduler::setTurbo(bool)`. On, the scheduler skips its waits for host deadlines; guest
  events keep their emulated cycles and order. Off, the pending host deadlines are put back at their emulated
  distance from now, so pacing resumes without a stall. Counters `g_donReadBytes` (readCdSectors, fioRead) and
  `g_donMpegActive` (bumped per picture by the MPEG stubs) in `runtime/don_read_counter.h`.
- Host: a per-blank hook (`HostGs::set_blank_hook`, called from the fight detection of `update_aspect`, which now
  runs when widescreen is on or the hook is set; `don_widescreen::fight_tracking()` lets the camera override count
  fight camera updates with widescreen off). Turbo is on while: not in a fight, past blank 600 (boot and intro),
  256 KiB or more read within the last 8 blanks less than 60 blanks ago, and no MPEG call for 30 blanks.
  `DON_FAST_LOAD=0` turns it off, `DON_FAST_LOAD_LOG=1` logs each change. Never in online play.

## Measured

`fight_short.txt` (power-on, menus, character select, NOW LOADING, 4-fighter fight), headless, Windows:
117 s before, ~109 s with fast loading on the development tree; on this branch (main + 0095), a second run gave
138 s with `DON_FAST_LOAD=0` and 121 s with it on (the PC was busier). So 8-17 s on a 2-2.5-minute route, no
turbo stretch inside the fight (`konoha_ougi.txt`: none after blank 6150). The gain stays modest: most of a loading screen is the game
waiting on its own (fades, animations), which cannot be told from a menu, so it is not sped up.
A shorter hold (30 blanks) gave ~112 s; a longer one (90) ~107 s but menus right after a load (the character
select) ran visibly fast for ~1.5 s. 60 is the compromise.

## Limits

- The gain is small; a story-mode load (the owner's case) was not measured (no route reaches it).
- Right after a bulk read in a menu (e.g. the character select loading its portraits), the menu can run fast for
  up to one second.
- Sound produced during a turbo stretch is produced faster than it plays; loading screens are mostly quiet, but a
  load with music may sound rushed for a moment.
