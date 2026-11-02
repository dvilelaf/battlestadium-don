# 028 · The main thread at the fight stop is in the normal per-blank frame-end wait; the wait is not the stop

- **Date:** 2026-10-13
- **Task:** T-304
- **Status:** partial (the earlier reading "the stop is a wait that never ends" is not supported)

## What is established about the stop (journals 010, 011, 023, 024, 026)

- Stops seen: character select with sound, the idle title with and without sound, the title with sound, the fight after blank 7080 without sound. The game stays alive: blanks count, handlers run, the picture is static.
- 011: with sound the DMA end-of-transfer interrupt and the VBlank handler are delivered in the stop (measured under gdb), so no interrupt is lost there.
- 026: reproduction without sound, `ctl` route, stops after blank 7080 every run.
- Runtime candidates found by reading and not observed: `dispatchIrq` drops a cause whose mask bit is clear; the scheduler ignores the guest's DI (journal 027).
- This entry: the wait on 0x4C9F08/0x4C9F0C is the frame-end flip wait and it behaves the same in the stop and in the healthy run.

## Measured (gdb, `ctl` route and `base` route, blanks 7200 to 7203, `work/integ/wait.py`, `wait2.py`)

- Call chain of the main thread at the stop (return addresses verified as after a `jal`/`jalr`): `FUN_002D46B8` (wait, ra 0x2D47C8) <- 0x2D4D0C `FUN_002D4C18` <- 0x2E1D1C (frame-end function) <- 0x2D4D3C `FUN_002D48D0` <- 0x2E1D1C <- 0x2C9D00 (jalr) <- 0x2C4510 <- 0x13654C <- 0x135DF0 (jalr) <- 0x131EE0 <- 0x132400 (`FUN_00131CA0` region) <- 0x100210. sp 0x1F7FDC0 at every hit.
- `FUN_002D46B8` is entered once per blank (entries 7200..7203: 1 each) in both the stop and the baseline.
- Passes through the spin callback 0x136560 per blank: stop about 86,350; baseline about 75,900. Same shape.
- Per blank, a new entry is written to 0x4C9F0C (watch hits at 7201, 7202, 7203 in both runs): the head alternates between two buffers, the pump (a0=1, end of transfer) moves it, the wait ends and the next blank starts again.
- Five readings in the stop (blank 7200 to 7202): f08/f0c = 5A9F08/5A9F10 (or 69CD88/69CD90) with busy=1 for the spin; they equal each other only after the end-of-transfer pass of the blank. f08 != f0c is the term that is true during the spin and becomes false once per blank. Healthy: 5A9F00 or 69CD80 against 5CF0A0 or 6C1F20: same cycle, larger packets.
- The scene ring index 0x535BE8 stays at 194 in the stop (406 in the sound stops) and the picture md5 is constant from 7080 to 7200.
- The counter 0x4D6900 is not an indicator: in the stop it is in the hundreds of thousands at the first hit of a blank, in the baseline small, because the callback is only reset by a different path; both rise during the spin.

## Paragraph

Measured: at the fight stop the main thread is inside the frame-end flip wait, called from the main frame loop, and that wait is entered and left once per blank exactly as in the healthy run; the pump, the VBlank handler and the end-of-transfer interrupt keep working. So the wait does not end late: the condition stays true only until the blank's transfer finishes. The values that earlier images showed (5A9F08/5A9F10 identical 60 blanks apart) are the same parity sample taken twice, not a stuck queue (journal 011 correction). What does not advance is the game's own logic above the frame loop: the scene ring and the picture do not change although the frame loop is still posting a packet per blank. Supposed, not verified: the game's per-frame update stops producing new content (a state word that the scene thread waits on, or a thread blocked on a semaphore that the main thread would signal), and the runtime's scheduler may be the one that does not wake it. The next measurement is the EE thread table and semaphore states in the stop against the baseline.

## Not verified

- No fix, no patch. Stop vs baseline difference is only: ring index frozen, picture frozen.
- The baseline run used the `base` script, not the `ctl` script, so the contrast is between different inputs.

## Follow-up 2026-10-13 (later): the chain read, and the frame loop is not iterating (loop not yet found)

Read (decompiled, `tools/re/decompile.sh`): `FUN_00131CA0` is the frame loop (`do { FUN_00138B00 (per-frame logic); ...;
FUN_00135C40 (frame end) } while`), called from `FUN_00132280`. `FUN_00135C40` calls the frame-end callback `(*[0x4D6958])()` (the
`jalr` at 0x135DF0) = `FUN_00136530` -> `FUN_002C44F8` -> `FUN_002C9CC0` (`jalr` through `[0x5406D8]`, 0x2C9D00) -> `FUN_002E1CD8`
(returns 1 always; calls `FUN_002F0570`, `FUN_002D4C18`, `FUN_002E0898`) -> `FUN_002D4C18` (writes the end-of-frame packet; if byte
gp-0x7560 is set it calls `FUN_002D46B8` at 0x2D4D04; then `FUN_002D48D0` (append packet; calls `FUN_002D46B8` when its buffer is
full); a DI..EI section; `FUN_002D3A60(-1)`). `FUN_002D46B8` is the buffer flip: spin until `[gp-0x7568] == [gp-0x7564]` and
`byte [gp-0x7570] == 0` (callback `[gp-0x7B98]`, the 0x136560 counter), then DI, store the new tail in `[gp-0x7564]` (0x2D47FC,
the writer seen by the watch in every blank, stop and baseline alike), swap the double buffer byte `gp-0x759C`, EI, return.

Measured (gdb, `ctl` route, stop, blanks 7200 to 7203, present binary with patch 0041):
- The frame counter `[0x4D78E4]` (incremented at the end of `FUN_00135C40`) is 6348 at every blank: the frame loop does **not**
  iterate. `FUN_00138B00`, `FUN_00135C40`, `FUN_00131CA0`, `FUN_00136530`, `FUN_002C9CC0`, `FUN_002C44F8` have 0 entries in the four blanks.
- Yet per blank, once each: `FUN_002F0570`, `FUN_002E0898`, `FUN_002D4240`, `FUN_002D46B8` (and its post-wait writer of `[gp-0x7564]`)
  are entered, and `FUN_002D4C18`/`FUN_002E1CD8` are entered twice (each with the entry `ra` inside itself: the dispatch's resume of the
  same call, return-address words on the stack are identical in every blank: 2E1CD8, 2D4D3C, 2E1D1C, 2C9D00, 2C4510, 13654C, 135DF0, 131EE0).
- So the body of `FUN_002E1CD8` (the packet write and the flip) runs once per blank, from a stack that still shows the callback chain
  above, and nothing above `FUN_002E1CD8` returns. The writer of the tail word in the stop is the game's own flip code at 0x2D47FC.

Not found: which instruction sends control back to the top of `FUN_002E1CD8`'s work each blank without returning through
`FUN_002C9CC0`. Candidates, none verified: (a) a re-entry by the runtime (the scheduler restarting a guest function from its saved
`pc` after a checkpoint, with a wrong resume point for a function whose frame has already been left), which fits "the first hit of
the three reads has `ra` inside the function itself"; (b) a loop in `FUN_002F0570`/`FUN_002E0898`, not read. The three-reading of "the
exit condition" the task asked for is therefore not made: the loop is not between these functions in guest code that I could find.
Next: read `FUN_002F0570` and `FUN_002E0898`, and compare the stack words of the stop with the baseline's at the same hit.

## Follow-up 2026-10-13 (plain runs with patch 0041): how far sound and the idle title get

Binary md5 and the PS2Recomp tree (45 modified paths in `git -C third_party/PS2Recomp status --short`: another agent's experiment on
top of the series) are as noted per run; the binary was rebuilt by someone else between runs. Headless, software renderer. Scratch under `work/p41/`.

- **Run 1: sound, the route to the match.** Binary `e6fb359c…`. `DON_PLAY_MOVIES=1 --sound on --wav work/p41/r1.wav --shot-every 60`,
  `tools/run/scripts/to_match_1v1.txt`, Japanese disc folder. It ran to the script's `quit` (blank 7800, exit by "Game thread returned").
  The picture changed up to blank 2520 and from then on is one picture, md5 `b3704727` (the washed title, as in runs A), in every shot to
  7800, including the script's named shots `main_menu`, `charselect`, `charselect_practice`, `stage_select`, `match_*` (all `b3704727`).
  So with sound the game still stops at the title's fade-in: patch 0041 did not change it. Wav: 48 kHz stereo, 130.005 s, rms 0.0628,
  silent fraction 0.314; by 10 s windows rms is 0.0000 up to 40 s and 0.07-0.085 from 40 s to the end: sound goes on to the end
  (the title music), not stopped.
- **Run 2: idle title, English with Japanese voices, sound off, defaults.** Binary `e1ab6c63…`. Script `work/p41/idle.txt`: dialog
  answered (LEFT 270, CIRCLE 330), Start tapped at blank 5700, a shot every 10 blanks 5710-6000, quit 6100. The picture changed up to
  blank 4440 (last change between 4380 and 4440); from 4440 to 6060 every shot is identical (md5 `c9cbae9a`), including the 30 shots after
  Start. The last picture is the clean English title ("PRESS START button", translation credits), **not garbled**, and not the menu.
  The menu never appeared. Start did nothing.
- **Run 3: Start early, same variant, sound off.** Binary `c38cdddf…`. Start at blank 2300 (this was during the title's fade-in: shots
  at +10 to +30 are washed out), quit 2700. Pictures keep changing to the last one (the title's spotlights move): the menu did not
  appear within 400 blanks of that Start either. Whether the Start came too early (during the fade-in) was not tested; the "known to
  open the menu" case (Start within 5 s of the title) was not reproduced with a later Start such as 2500. The first pictures after Start
  in runs 2 and 3 cannot be compared (different title phases), so where they part is not measured.
- **Not run:** run 2 with `--sound on` (time).

- 2026-10-13: the picture of how the port executes the game's code and of the stop, put together from this and the neighbouring entries, is in [knowledge/20-execution-model-and-the-stop.md](../knowledge/20-execution-model-and-the-stop.md).
