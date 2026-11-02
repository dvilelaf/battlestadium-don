# 033 · Past the second match of Battle Stadium Mode; save and load

- **Date:** 2026-10-14
- **Task:** T-320 / T-307
- **Status:** partial (the mode explored to ~39000 blanks; save and load checked, Auto save and the saved option values not)

Binary for every run here: `be40828685d4e6fbcf12c5f36b6cfc95` (build/linux-dev/don). English variant with Japanese
voices, headless, software renderer. Scripts in `work/soak/` (not versioned); the longest route is `r8.txt`
(`gen4.py 40000`, `run2.sh r8 800 on`).

## Part 1: the mode after match 2

Run r7 (`--sound off`, 20000 blanks, 9 min: slower than real time on a shared machine) and r8 (sound on,
40000 blanks planned): r8 exit 0, `frame=39240 vsync=39960 ... largest gap 180 vsyncs (limit 300): ok`; wav 666.8 s,
rms 0.104825, silent 0.350925, saturated 0.000177. Sound on is the faster way to explore (60 blanks/s).

The pattern now: double tap toward the opponent, hold, Square x3, Forward+Square, Circle (ultra), Triangle, R2 held
25 blanks, alternating the side every 240 blanks. It beat the first match.

Screens seen:
1. Match 1: Naruto vs one COM, Sakura, on a ship deck. Matches are rounds (best of several): the ultra
   cut-in (red lines, Naruto) at ~7200 and ~13200, a Sakura close-up ~10200, Naruto's winner close-up ~19200, then a new
   match loads. It was won (r7, r8).
2. Next, with Piccolo as the 1P fighter (the player's character differs from the match before; not chosen by the
   script): Piccolo vs two COM, Zoro and Frieza, temple courtyard stage, items (a chest, bombs).
3. A "MISSION" box over the stage before the fight: "Don't use guard!" (so matches carry missions; the box is in
   English). After the fight a RESULT screen over a Piccolo close-up: "Count 0x" (a count of the mission thing) and a
   coin panel: "Coins earned 0x", "Coins you have 15x", a "SLOT" coin strip.
4. In r8 that match was lost (Frieza WINNER at ~22200) and a match with the same fighters started again at ~23400
   (by the CONTINUE? prompt or by the game; my script presses did not check which); then "Coins earned / Coins you
   have", then **Piccolo vs Usopp on a Capsule Corporation city stage**, timer running to ~39600 (017 left).
5. Not reached: an ending, credits, a boss, tickets, a name entry. The mode has coins and missions: the Tickets
   Mode presumably spends them (not checked).

First thing that looks wrong: nothing froze, and no picture was garbled. The player's character changing between
matches is probably the mode's design (missions with fixed fighters), but I did not prove it.

## Part 2: save and load (`--mc-dir work/soak/mc`, an empty folder of my own)

- **Save (run sv2):** OPTIONS (Up from Battle Stadium Mode, Circle), Down to SAVE (the cursor starts on AUDIO MODE; BGM,
  SE, AUTO SAVE, SAVE, LOAD below), Circle: "The memory card (PS2) in MEMORY CARD slot 1 is inserted. Do you want to
  save the game?" with the cursor on **NO**; Left, Circle = YES: "Save from memory card (PS2) in MEMORY CARD slot 1
  is completed." (OK). Files in `mc/mc0/BISLPS-25675game/`: `BISLPS-25675game` 992 bytes, `icon.sys` 964, `battle_logo.ico`
  60840. (The first save attempt answered NO: "Canceling the save of the game data ... Is this OK?".)
- **Second run, same folder (run ld):** the first dialog is no longer "There is no game data": it is
  **"Load from memory card (PS2) in MEMORY CARD slot 1 is completed."** with OK, so the game loads at start.
  After it the health warning etc. goes on as before.
- **LOAD from OPTIONS:** my script pressed Down four times and landed on SAVE, not LOAD (one row off: the cursor
  starts on the first row, and the Audio rows count), so the prompt was "Do you want to **overwrite** the save
  data?"; YES overwrote the save (file changed from byte 1; modification time moved). LOAD itself was
  **not** exercised: not verified.
- **Option values:** my Right taps on BGM and SE (4 and 10 blanks) showed no visible change in the panels (the rows
  stay at OFF in every shot I looked at, only the cursor moves), so the "BGM 2, SE 1" check could not be done. Whether
  the panel really ignores the taps, or the shots hide the highlight, is not known. The score of Survival
  was not made (not played in this session).
- **Auto save (c):** not tried (time box).
- Log: no errors in any of the three runs; liveness ok in all (largest gap 120 vsyncs).

## Not verified
Match 3+ and the ending; Survival's score; option values surviving a save; LOAD from OPTIONS; Auto save; a third run.

## Follow-up 2026-10-14: the save checks, finished

Binary for all runs: `be40828685d4e6fbcf12c5f36b6cfc95`. Card folders of my own (`work/soak/mcB`, `mcX`, `mcRO`).
Scripts `work/soak/gen6.py` (steps with a shot 8 and 16 blanks after every press), runs a1, b1, c1, d1, e1, f1, f2
(all liveness ok, largest gap 120 vsyncs).

1. **Rows and cursor.** The options screen opens on AUDIO MODE; each Down is one row: Audio mode, BGM, SE, Auto
   save, Save, Load (shots k01..k07 of a1). The selected value is the white one; BGM and SE both start at 3. Left
   lowers: two Lefts on BGM = 1 white, one Left on SE = 2 white. Saved (card empty, cursor on NO, Left, Circle):
   "Save from memory card (PS2) in MEMORY CARD slot 1 is completed." File `BISLPS-25675game` md5 a61f42d5..., 02:16:51.
   (Earlier note in this entry that the taps showed no change: they were 4-blank taps and I could not tell the white
   value; with 6-blank taps and zoomed crops the changes are plain.)
2. **Second run.** The start dialog is "Load from memory card (PS2) in MEMORY CARD slot 1 is completed." (OK).
   The options show BGM 1, SE 2: the saved values are back. Changed to BGM 3, SE OFF without saving; Down x3 to LOAD (the
   cursor on the LOAD row in the shot), Circle: "Load game data from the memory card (PS2) in MEMORY CARD slot 1?"
   YES/NO (cursor on NO), Left, Circle: "Load ... completed." Back to BGM 1, SE 2. The save file's md5 and time
   before and after the load: identical (a61f42d5..., 02:16:51).
3. **Auto save.** Down x3 to AUTO SAVE, Left = ON (ON is the left value). SAVE again: "Do you want to overwrite the save data?",
   YES: completed; file md5 b45c62a5..., 02:19:56 (the setting is on the card). Next run: the options open with Auto
   save ON; changed SE, left the screen with Cross: a "Save from memory card ... is completed." box appeared by
   itself and the file changed (md5 45d1b32c..., 02:21:10, during the run that began 02:20:09). So the game writes on
   leaving the options after a change. Third run: BGM 1, SE 3 there (the SE change made after the last manual save).
   Not found: any other moment worth an auto save (no Survival fight was finished here).
4. **Errors.** (a) `--mc-dir` on a **missing folder**: the port creates `mc0` and `mc1` there, and a save works
   (three files, 992 / 964 / 60840 bytes); no message. (b) A **read-only** card folder (`mc0` mode r-x): SAVE, Left,
   Circle (YES): the game shows **no error**: the same question "Do you want to save the game?" comes back with the
   cursor on NO; nothing is written; the log has no line about it; answering NO gives "Canceling the save ... Is this
   OK?". The game goes on. The first dialog at the start with an empty card is "There is no game data on the memory
   card (PS2) in MEMORY CARD slot 1. (you need 85KB or more of free space) Do you wanna continue the game?". A silent
   failure on a read-only card is a gap: the console would say that the save failed (not checked what text).
5. **Dates.** No date appears in any dialog (start, load, save, overwrite). The card files carry the host's times as
   written (02:16:51 etc.); whether the game's own data shows a date was not seen. Not verified further.

Not verified: auto save at a Survival fight's end; the file's date as the game would show it; the read-only
failure on a console.

## Follow-up 2026-10-15: the read-only card (T-307, patch 0073)

- **Why it was silent.** In the save script (`mcard/2000.seq`) every failing step (Mkdir, Chdir, Open, Write, Close) maps
  the codes 0 / -2 / -3 / -4 / -5 / -11 to the same two branches: 0 goes on, any other only sets a flag; with a flag set
  the later steps are skipped and the script asks again. The run on a read-only `mc0` shows it: `Mkdir` answers -5, then
  only the final `GetDir` (-4) follows, no Chdir/Open/Write. So **no libmc code returned by those calls can make the game
  show a message**; changing the codes would change nothing.
- **What does show one.** The free-space check at the start: the script's "There is not enough space on the memory card
  (PS2) in MEMORY CARD slot 1 (you need 65KB or more of free space)" comes from the free blocks of `sceMcGetInfo`.
  Patch 0073: `sceMcGetInfo` probes the card folder (create + remove a small file, once a second at most) and reports 0
  free blocks, result 0, when it cannot write: what a full card says.
- **Checked.** `tests/runtime/mc_read_only_test.cpp`: fails without the patch (8192 blocks on a read-only folder), passes
  with it; `mc_file_numbers_test` passes. Game, `mc0` mode r-x (folder of my own, `work/t307ro/card`): the start dialog
  and OPTIONS > SAVE (Circle, YES) show the not-enough-space text with OK; nothing is written; after it the script's own
  taps hit "Canceling the save ... Is this OK?" (the timings of `save_and_load_save.txt` assume the normal dialogs).
  On a writable folder `pytest tests/regression -m slow -k save_and_load` passes (1 passed).
- **Limit.** A card that becomes read-only after a save also refuses an overwrite (a console card with a save can
  overwrite). Not checked: the text a real console shows for a card that fails mid-write.
