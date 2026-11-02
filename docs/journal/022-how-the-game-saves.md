# 022 · How the game saves (memory card investigation)

- **Date:** 2026-10-13
- **Task:** T-307
- **Status:** open (the first save attempt fails; the cause of the failing write is not determined, see "Cause")

## Symptom

The owner wants to save. At boot the game says "There is no game data on the memory card (PS2) in MEMORY CARD
slot 1. (you need 65KB or more of free space) Do you wanna continue the game?". Nothing was changed for this
entry; only the existing binary (`build/linux-3d/don`, English variant `en_patch/jpvoice/disc`, headless,
software renderer, `DON_VU1=microvu`) was run. Scratch (scripts, logs, extracted scripts) is in `work/saves/`,
shots in `work/shots/saves-1/`.

## Classification

Not a crash. A stub that is only partly right (`Stubs/MemoryCard.cpp`), found by running the save once. No row in
`docs/knowledge/08-external-references.md`; the "Sync must say running once" lesson of the Ultraman FE3 row is
already honoured by the handler.

## Investigation

### 1. What the runtime does for the memory card

- The game contains 24 libmc functions (`work/analyzer/config.toml:189-212`): `sceMcInit 0x11BD58`, `sceMcEnd`,
  `sceMcChangeThreadPriority`, `sceMcGetSlotMax`, `sceMcOpen 0x11C230`, `sceMcMkdir 0x11C358`, `sceMcClose`,
  `sceMcSeek`, `sceMcRead 0x11C5E0`, `sceMcWrite 0x11C6F8`, `sceMcSync 0x11C870`, `sceMcGetInfo 0x11C990`,
  `sceMcGetDir 0x11CB80`, `sceMcChdir`, `sceMcFormat 0x11CEA0`, `sceMcDelete 0x11CF70`, `sceMcFlush`,
  `sceMcSetFileInfo 0x11D150`, `sceMcRename`, `sceMcUnformat 0x11D478`, `sceMcGetEntSpace`, and the libmc
  internals `mceIntrReadFixAlign`, `mceGetInfoApdx`, `mceStorePwd`. All 24 are `stub` in
  `config/stub_policy.toml:234-259`, so the runtime's handlers run and the game's own libmc code never does.
  (`sceMcUdCheckNewCard 0x11CB10`, `sceMcReadFast`, `sceMcWriteFast` are in the analyzer list but not in the policy:
  nothing calls them as far as the callers tool shows.)
- Every handler is in `third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp` and uses the
  "immediate" model: the call does the work at once, returns 0 (command accepted), stores `(cmd, result)` and sets
  "pending"; the next `sceMcSync` returns 1 and writes `cmd` and `result` through its pointers (a1, a2); a `Sync` with
  nothing pending returns -1 and writes nothing.
- The card is a **host folder per port**: `IoPaths.mcRoot` (`ps2_runtime.cpp:341`: default `<folder of the ELF>/mc0`;
  `getMcRootPath`, `MemoryCard.cpp:119`): port 0 = `mc0`, port 1 = the sibling `mc1`. Guest paths are normalised
  (`mc0:` prefix dropped, `.`/`..` handled, per-port current directory) and mapped under that folder. Only slot 0 of
  ports 0 and 1 is valid. The game's "MEMORY CARD slot 1" is port 0.
- Really implemented (host files, `std::filesystem`/`FILE*`): `Open` (flags RDONLY/WRONLY/RDWR/APPEND/CREAT/TRUNC;
  directory = denied; missing without CREAT = -4; up to 32 open files; fd numbers start at **1**), `Read`, `Write`
  (flushes at once), `Seek`, `Flush`, `Close`, `Mkdir` (ok if it exists), `Chdir`, `GetDir` (wildcards, `.` and `..`
  first, then entries sorted case-insensitively, 64-byte `sceMcTblGetDir` rows with dates, size, attributes),
  `Delete` (not empty = -6), `Rename`, `Format` (wipes the folder, sets formatted), `Unformat` (wipes, clears
  formatted).
- Only answer constants: `GetEntSpace` = 1024; `SetFileInfo` = ok if the path exists (stores nothing); `GetSlotMax` = 1;
  `ChangeThreadPriority`, `mceIntrReadFixAlign`, `mceGetInfoApdx`, `mceStorePwd` = 0; `Init` marks both ports formatted
  and creates `mc0`/`mc1`; `End` resets state.
- `GetInfo`: card type always 2 (PS2), free blocks always 8192 (when formatted), format 1; result 0. With "unformatted"
  (only after the game calls `Unformat`): free 0, format 0, result -2. There is **no way to report a missing card**
  (both ports always exist), **no real free-space accounting** (8192 regardless of the folder's contents) and no
  "card changed" (-1) event. `GetDir` of a path whose directory does not exist returns -4 (not 0 entries).
- The file also holds about 30 `mc*` handlers (`mcGetFreeCapacitySize`, `mcNewCreateSaveFile`...) with constants named
  `kCv*`: they belong to another game (Castlevania-style helper names), are not in `stub_policy.toml` and the DON
  executable has no function of those names (its game code is `FUN_*`), so they are dead for this port.
- The I/O processor side: the game does load `mcman` (2.38) and `mcserv` (2.16) from `MODULES/0000.FPK`
  (`docs/knowledge/14-iop-modules.md` rows 3 and 4; the log shows seven `loaded IRX` lines), and they register
  `0x80000400`. They **take no part**: the EE only talks to them through `sceMcInit`/`sceMcOpen`... which are
  replaced, so no RPC to `0x80000400` is ever made, and the real `mcman` would need SIO2 hardware that does not
  exist. All card traffic goes through the replaced functions.

### 2. What the game asks at boot

`work/saves/saves_boot1.log` (20 s boot). The `[MC]` lines are on one long line without newlines; the useful part:

```
[MC] GetInfo port=0 type=2 free=8192 format=1 result=0   [MC] Sync cmd=1 result=0
[MC] GetDir port=0 '/BISLPS-25675game/*' maxent=18 -> result=-4   [MC] Sync cmd=13 result=-4
[MC] GetInfo ... (the same GetInfo + Sync, once per frame, for as long as the dialog is on screen)
```

- The game asks the card's state (GetInfo), lists `/BISLPS-25675game/*` (18 entries at most), gets -4 and keeps
  polling GetInfo (to see whether the card is pulled out or replaced). It never opens anything at boot.
- On a console the same screen appears for a formatted card that has no folder `BISLPS-25675game`. The runtime
  answers exactly that (formatted, free 8192, GetDir -4), and the screen is the script's "There is no game data on
  the memory card ... Do you wanna continue the game?" text, so **for an empty card the message is the right
  answer**, not an error of the port. (Whether -4 vs 0 is also what libmc returns for a missing directory on
  hardware is **unverified**; the game reacts to it with the correct text, which is the evidence available.)
- The dialog logic is not in the executable but in a script of the data: see 3.

### 3. When and how the game writes a save

- The game has a **script interpreter** whose commands include the card. The executable function is
  `FUN_001c7bc0` (0x1C7BC0-0x1C8F3F, the only caller of `sceMcGetDir`, `sceMcOpen`, `sceMcWrite`, `sceMcMkdir`,
  `sceMcRead`, `sceMcClose`, `sceMcFormat`, `sceMcDelete`, `sceMcGetInfo`; `callers.py` shows no other caller of
  `Write`/`Mkdir`/`GetDir`). Its opcode is byte 1 of each script word. The card opcodes (decompiled
  `work/saves/saves_c7bc0.txt`; not versioned): 0x00 `sceMcInit`; 0x01 `sceMcSync(mode)` (stores the Sync *result* in
  script variable +0x4C and the Sync *return value* in +0x54); 0x02/0x03/0x04 `GetInfo` variants; 0x05 Format;
  0x06 Unformat; 0x07 Mkdir; 0x08 Chdir; 0x09 Open (stores the *return of sceMcOpen* in +0x4C and +0x54); 0x0A Flush;
  0x0B Close; 0x0C Write (mode 0 = the save block, 1 = the icon.sys block, 2 = the icon file); 0x0D / 0x0E Read
  (0x0E also checks a checksum); 0x0F Delete; 0x10 GetDir; 0x11 / 0x12 size computations; 0x13 builds the icon.sys
  block from three strings; 0x14 takes the icon file; 0x16 sets the file's date (`SetFileInfo`).
- The scripts are in `DATA.AFS` entry 106 (a pack of 24 files; extracted with `tools/fpk.py` into
  `work/saves/ex/106/`): `mcard/0000.seq` (load), `mcard/1000.seq` (boot check), `mcard/2000.seq` (save, with
  "overwrite?" and "format?" branches), `mcard/3000.seq` (the same with "replaced/inserted" texts: the automatic
  save). The strings in all of them: directory `/BISLPS-25675game`, files `/BISLPS-25675game/icon.sys`,
  `/BISLPS-25675game/battle_logo.ico`, `/BISLPS-25675game/BISLPS-25675game`, wildcard `/BISLPS-25675game/*`.
- Who starts which script (strings of the scripts of the same pack, not executable code):
  - `cmn/0000.seq` lists the scenes in order and ends with `mcard/1000` and `mcard/3000`. `mcard/1000` is the boot
    check (screenshot of the "no game data" dialog).
  - `option/0000.seq` loads `mcard/2000` and `mcard/0000`: the OPTIONS menu has a "MEMORY CARD" group with
    **AUTO SAVE ON/OFF, SAVE, LOAD**. SAVE runs `mcard/2000`, LOAD runs `mcard/0000`.
  - `mcard/3000` is the scene for the **automatic save** (when AUTO SAVE is ON). What triggers it in play (end of a
    match, an unlock, leaving the results screen) was **not determined**; the scene list in `cmn/0000.seq` puts it
    last, after the ending and the slot scenes. Finding the executable code that selects that scene id was not done.
- Sizes of a save (the script's constants in `FUN_001c7bc0`): the save block is **0x3E0 = 992 bytes** (a checksum word at
  +0, then the 0x3D0-byte game state at +4 copied from RAM `0x5341C0`; the checksum is the XOR of 0xF6 words);
  the `icon.sys` block is **0x3C4 = 964 bytes**; the icon file is `mcard/battle_logo.ico` of the pack, **0xEDA8 =
  60,840 bytes** (confirmed: the extracted file is 60,840 bytes). Together 62,796 bytes plus the directory entries,
  which is the "65KB". The free-space script check is `(iconBytes + 0x3FF) >> 10) + 6` KB for the icon part.
- After writing, the script calls GetDir on `/BISLPS-25675game/*` again and decides from its result and from reads
  (opcode 0x0E compares the XOR with the stored word); a failed save ends in the "corrupted ... delete it in the
  console browser screen" text (screenshot below), a good one in "Save ... completed".

### 4. Trying it

All with `work/saves/s*.txt` (copies of the `to_main_menu` route: `LEFT`+`CIRCLE` answers the boot dialog YES,
`START` twice, main menu, `UP` wraps to OPTIONS, `CIRCLE`; in OPTIONS four `DOWN` taps reach SAVE, `CIRCLE`; in the
dialog `LEFT` selects YES, `CIRCLE`). A run takes about two minutes.

1. `s1`/`s2`: the OPTIONS screen is reached (AUDIO MODE / BGM / SE / AUTO SAVE / SAVE / LOAD). It starts on AUDIO MODE.
2. `s3`: on SAVE the game logs a second `GetDir ... -> -4`, shows "The memory card (PS2) in MEMORY CARD slot 1 is
   inserted. Do you want to save the game?" YES / NO. `mc0` still empty.
3. `s4`: YES. The log, besides GetInfo polling:
   ```
   [MC] Sync cmd=11 result=0                     (Mkdir BISLPS-25675game: created)
   [MC] Chdir port=0 '/BISLPS-25675game' -> result=0
   [MC] Sync cmd=12 result=0
   [MC] Sync cmd=2 result=1                      (Open: fd 1; the file was created, 0 bytes)
   [MC] Sync cmd=6 result=-4                     (Write: NO ENTRY)
   [MC] GetDir '/BISLPS-25675game/*' -> result=3 (".", "..", the empty file)
   ```
   `mc0` after: `BISLPS-25675game/` (dir) and `BISLPS-25675game/BISLPS-25675game` **0 bytes**. No `icon.sys`, no
   `.ico`. The screen: "The memory card (PS2) in MEMORY CARD slot 1 containing game data is corrupted. Delete it
   in the (PS2) console browser screen." with OK (`work/shots/saves-1/saving_2.png`, `saving_5.png`).
4. Second boot of the same folder (`s5`): the log's GetDir now returns 3, and the first dialog changes to "slot 1-A
   containing game data is corrupted ... Do you wanna continue the game?" (`boot2_dialog.png`): the port now sees
   the folder, and rejects it because its file is empty. (The `mc0` test output was removed afterwards, the
   folder is empty again as it was.)

## Cause

Not determined for the write. What is measured: the first command after `Open` (which answers fd 1) is a `Write` that the
runtime answers -4, and the only way `sceMcWrite` returns -4 is "that fd is not in `g_mcFiles`"
(`MemoryCard.cpp`, `sceMcWrite`). So the fd the game passes is not 1, or the file was closed/reset between the two. The
script takes the fd from a script variable fed by `sceMcSync`'s result pointer (+0x4C) and by the *return value* of
`sceMcOpen` and `sceMcSync` (+0x54, opcodes 0x09 and 0x01); the runtime returns 0 from `sceMcOpen` and 1 from
`sceMcSync`. Candidates, none verified: (a) the script reads +0x54 or another copy and gets 0 or 1 where it expects the
fd; (b) `sceMcSync` is called in a mode or twice so that the pending result is lost; (c) real libmc numbers the first fd
0 and the script compares against it. A trace of the arguments of `sceMcWrite`/`sceMcClose`/`sceMcSync` would decide
in minutes (step 2 of the list).

## Fix

None (investigation only).

## Verification

Commands run (all headless, one at a time): the 20 s boot above; `s1.txt`..`s5.txt` in `work/saves/` with
`--input-script` and `--shots work/shots/saves-1`; `grep` of the `[MC]` lines; `find` on `mc0` before and after.
`mc0`/`mc1` of the variant were empty before and were left empty. `git status` shows no file of this task other than
this entry.

## Temporary diagnostics

None added. Scratch only under `work/saves/` and `work/shots/saves-1/`.

## What has to be done for saving to work

Each step is one change that can be made and checked in under an hour, in this order.

1. (DONE, follow-up 3) **Integrate patch 0040** (card file numbers from 0, three files at most, -7 when full): add it to `series`, run
   `bash tools/setup/fetch_ps2recomp.sh --reset`, rebuild. Check: `mc_file_numbers_test` passes on the tree with it.
2. (DONE, follow-up 3) **Run the save.** `s4.txt` on the build with 0040. Check: `mc0/BISLPS-25675game/BISLPS-25675game` has 992 bytes
   (and the `[MC] Sync cmd=6` line says 992, not -4). If it still fails, trace the calls as in follow-up 1 (gdb
   script `work/saves/trace.py`).
3. (DONE) **Check the other two files.** Same run: `icon.sys` must be 964 bytes and `battle_logo.ico` 60,840 bytes. If a
   Write is short or a Seek/Flush is missing, fix that handler. Check: `find mc0 -printf '%p %s\n'` shows the three
   sizes and the screen says "Save ... completed".
4. (scripts exist in work/saves; to commit once retimed) **A script that saves and one that loads.** Add `tools/run/scripts/save_to_card.txt` (the route of `s4.txt` with
   a final `quit`) and `load_from_card.txt` (OPTIONS, `DOWN` x5 to LOAD, YES). Check: after the first, the boot
   dialog of a second run (`s5.txt`) must *not* say "no game data" and not say "corrupted"; after the second,
   a "Load ... completed" shot.
5. (DONE) **Check the load's read-back.** The read opcode 0x0E compares the XOR of 0xF6 words. If it reports a mismatch on
   what step 2 wrote, look at `sceMcRead`/`sceMcSeek` (return values, short reads at the end of the file). Check: the
   load script of step 4 ends on "completed", not "Load failed".
6. **Make `GetInfo` report real free space.** Free blocks are 8192 regardless of contents; make it
   `capacity - bytes used in the folder` (KB, with the 1 KB cluster the game assumes) so "not enough space" can be
   tested and cannot be wrong for a big folder. Check: a test with a pre-filled `mc0` (a 8191-KB file) makes the
   boot dialog say "There is not enough space".
7. **Card states for tests.** Let an environment variable (`DON_MC_STATE=none|unformatted|full|ok`) make `GetInfo`
   return the console's codes for a missing card (-1/-2 as libmc does), unformatted (-2) and full, so the other
   dialogs of the scripts (`No memory card inserted`, `unformatted`, `Do you wanna format?`) can be run from a script.
   Check: each value shows its dialog in a shot.
8. **Implement `SetFileInfo` and the dates.** The game sets the modification date after saving (opcode 0x16); the
   runtime only checks existence. Not needed for saving to work, but the listing's date should be the save's. Check:
   `GetDir` after the save returns the date the script set.
9. **Find what starts the automatic save.** Trace which scene id selects `mcard/3000` (the string is in the scene list of
   `cmn/0000.seq`; find the executable code that switches to scene 20 or the equivalent) and what the result screen or the
   unlock code does with AUTO SAVE. Check: a journal paragraph naming the function and the condition; then a script
   that finishes a short match with AUTO SAVE ON and sees a save in `mc0`.
10. **Where the card lives for the player.** The launcher already runs from a per-variant folder in the user's data
    folder (`~/.local/share/don/variants/<variant>/`), so `mc0` is already there. Check that `make_variant_dir` does not
    delete or replace `mc0`/`mc1` when it re-prepares a variant (a re-run with a save in place must keep the save), and
    document in `docs/RUNNING.md` where the save is and how to back it up.
11. **Remove the dead `kCv*` handlers** from `MemoryCard.cpp` (they belong to another game and give a false
    impression that save-related functions exist). Check: the build and the regression tests still pass.

## Follow-up 2026-10-13: the failing write on the build with patch 0039

**Step 1 (repeat, unchanged).** `build/linux-dev/don` dated 2026-10-13 16:17:27 (the first runs) and 16:32:30 (the
last gdb run; the binary was relinked in between, same behaviour). `DON_PLAY_MOVIES=1 DON_VU1=microvu`, English variant,
`mc0` emptied before every run (`ls -A mc0 | wc -l` = 0), route `s4.txt`. **The write still fails**:
`[MC] Sync cmd=2 result=1` (open), `[MC] Sync cmd=6 result=-4` (write), and `mc0` holds only
`BISLPS-25675game/BISLPS-25675game` of 0 bytes. The second boot lists the folder (`GetDir ... result=3`), as before.
So 0039 did not fix it, and I did not go on to the second boot or LOAD. The test output was removed from `mc0`
after each run (empty again).

**Step 2 (gdb, no code changed; `work/saves/trace.py`, `trace2.py`, output `mctrace*.txt`).** Breakpoints on
`ps2_stubs::sceMc*` plus a finish breakpoint for the return value; arguments read from the context
(a0..a3 at ctx+0x40+0x10*i), return from ctx+0x20. Sequence from YES:

| # | call | arguments | returns | what the following sync hands back |
|---|---|---|---|---|
| 1 | sceMcMkdir | port 0, `/BISLPS-25675game` | 0 | result 0 |
| 2 | sceMcChdir | port 0, `/BISLPS-25675game` | 0 | result 0 |
| 3 | sceMcOpen | port 0, `/BISLPS-25675game/BISLPS-25675game`, flags 0x202 | 0 | Sync(mode 1) returns 1, result **1** (the fd) written to 0x1A30ADC |
| 4 | sceMcWrite | **fd 0**, buf 0x535800, size 992 | 0 | Sync(mode 1) returns 1, result **-4** |
| 5 | sceMcGetDir | `/BISLPS-25675game/*` | 0 | result 3 |

No `sceMcClose` or other card call between 3 and 4. The runtime's numbering is as documented (fd 1 in `g_mcFiles`;
`MemoryCard.cpp` `sceMcOpen` 983-1044, `sceMcSync` 1193-1235); `sceMcWrite` (1267-1307) answers -4 because fd 0 is not in
`g_mcFiles`.

**Where the two part.** The interpreter's variable block is at 0x1A30A90 (the Sync result pointer 0x1A30ADC is its
+0x4C). At the entry of `sceMcWrite` the words are `+0x4C = 1` (the Sync result, the fd) and `+0x54 = 0`; the fd the
write receives (0) equals `+0x54`, not `+0x4C`. By the decompiled opcodes, Open (0x09) stores sceMcOpen's return (0) in
both, and Sync (0x01) stores the result in +0x4C and sceMcSync's return in +0x54. The trace shows sceMcSync returning 1,
so something between the Sync and the Write sets +0x54 back to 0, or the Write's operand is not the variable I
think it is. **I did not determine which**: the operand decoding (`FUN_001c9a20`) and the script bytes between the Sync
and the Write (`mcard/2000.seq` around 0x61C-0x640) were not fully decoded in the time box. So the defect is **not
named with file and line yet**. Established: the runtime delivers the fd (1) through the Sync result at +0x4C, as the
console's library does, and the game passes 0. Next action, no code change: a gdb watchpoint on `rdram + 0x1A30AE4`
(+0x54) from the Open to the Write, to see which game instruction writes the 0. If it is the Sync opcode storing a 0
return, the fix is the value `sceMcSync` returns; if it is the script's own assignment, the script expects the fd in
+0x54. Candidate smallest change, **unverified**: make `sceMcSync` return the fd/result as its return value when the
finished command was Open.

**List of steps.** Superseded by follow-up 2: step 1 (trace) is no longer needed.
unchanged, except that step 2 starts from the writer of +0x54.

## Follow-up 2 2026-10-13: the defect is the file numbering; patch 0040

**Defect.** `MemoryCard.cpp` `g_mcNextFd = 1` (line 86) and `g_mcNextFd++` in `allocateMcFdLocked` (line 420): the first
open returns file number 1 and numbers only grow. The game passes 0 to the write because that is what a first open
gives on the console, whichever script variable carries it. (This replaces the watchpoint of the previous follow-up.)

**What `MCMAN.IRX` shows** (`disc/MODULES/MCMAN.IRX`, rabbitizer, `work/saves/mcman.dis`): a table of **3** open-file
entries, 0x30 bytes each, at module data offset 0x262B0 (first byte of each entry = in use). The open function
(code offset 0x6C40) scans entries 0, 1, 2 for a zero flag (`slti fp, 3`, stride `addiu v1, v1, 0x30`), takes the
first free index, and if all three are in use jumps to the error exit with **-7** (`sceMcResUpLimitHandle`). So the
numbers are the lowest free index from 0, three at most, -7 when full.

**Patch.** `patches/ps2recomp/0040-mc-file-numbers-from-zero.patch` (not in `series` yet; the orchestrator integrates it):
the open returns the lowest free number from 0, a close frees it, a fourth open fails with -7, `kMcMaxOpenFiles` 32 -> 3,
`g_mcNextFd` starts and resets at 0 and is recomputed after each open/close/port close. Test
`tests/runtime/mc_file_numbers_test.cpp`: 10 checks fail on the tree without the patch (first open 1, write to 0 returns
-4, file 0 bytes), all pass with it. Not run with the game.

**For the next steps.** The save's three writes (992, 964, 60,840 bytes) go to numbers 0, 1, 2 in turn only if the script
closes each before opening the next; otherwise they are three open at once (the table's size), which fits. After the
patch is in a build: rerun `s4.txt` (step 2 of the list), then the second boot (does the first dialog change?) and LOAD
(opcode 0x0E reads back and checks the XOR of the first 0xF6 words, which `Read`/`Seek` must return correctly).

## Follow-up 3 2026-10-13: saving works end to end on the build with patch 0040

Build `build/linux-dev/don` 2026-10-13 16:41:43 (patch 0040 in). Every run: `DON_PLAY_MOVIES=1 DON_VU1=microvu`, headless,
software renderer, English variant, one at a time. The card started **empty** (`en_patch/jpvoice/disc/mc0` with no
entries; I emptied it again at the end and kept the cards in `work/saves/card_saved` and `work/saves/card_mono`).
Two earlier attempts of the save route ended with "signal 2/15: leaving" after 20-90 s (the game is stopped from
outside, not a crash: the log ends with the harness's signal); the run below was started with `setsid` and completed.

1. **Save** (`work/saves/save_from_options.txt`, the route of `s4.txt`). `[MC]` from the open on:
   `Chdir ... result=0`, then three times `Sync cmd=2 result=0` (open, **file number 0**), `Sync cmd=6 result=N` (write),
   `Sync cmd=10 result=0` (flush), `Sync cmd=3 result=0` (close), with N = **992, 964, 60840**. Screen (`saving_3`..
   `saving_5`): "Save from memory card (PS2) in MEMORY CARD slot 1 is completed." + OK. Card afterwards:
   `BISLPS-25675game/BISLPS-25675game 992`, `icon.sys 964`, `battle_logo.ico 60840` (folder entry 78 bytes).
2. **Second boot** (`work/saves/second_boot_dialog.txt`). `GetDir ... -> result=5` (3 files + `.` and `..`), then
   two times `Sync cmd=2 result=0` / `Sync cmd=5 result=992` / `Sync cmd=3 result=0` (open, **read of 992**, close). The
   first dialog is no longer the "no game data" one: it is a black screen with "Load from memory card (PS2) in
   MEMORY CARD slot 1 is completed." and OK (the boot loads by itself because AUTO SAVE is read from the card).
3. **LOAD** (`work/saves/load_from_options.txt`: OK on the boot dialog, OPTIONS, 5x DOWN, CIRCLE, LEFT = YES, CIRCLE).
   Screen: "Load game data from the memory card (PS2) in MEMORY CARD slot 1?" with YES/NO (NO selected by default), then
   "Load from memory card (PS2) in MEMORY CARD slot 1 completed." `[MC]`: `GetDir -> 5`, then the same open /
   `Read` 992 / close twice. Only the 992-byte file is read back (`icon.sys` and the icon are for the console's
   browser). No "corrupted" message, so the checksum of opcode 0x0E matched what the save wrote.
4. **A setting survives** (`change_audio_and_save.txt`, then `boot_and_show_options.txt`). AUDIO MODE STEREO -> MONO
   (RIGHT on the first OPTIONS row), SAVE, YES: only `BISLPS-25675game` is written again (992 bytes; `icon.sys` and the
   icon keep their time stamps; 8 bytes of it differ from the first save). After the next boot the OPTIONS screen shows
   MONO selected: mean brightness of the two halves of the row `STEREO | MONO` is 128.4 / 102.0 at the start
   (STEREO), 87.2 / 134.0 after the change, and **87.2 / 134.0 after the reboot** (`work/saves/cmp.py`; shots
   `work/shots/saves-5/options_before.png`, `options_mono.png`, `work/shots/saves-6/options_after_boot.png`).

**Verdict:** the memory card works end to end: save, second boot (auto-load), LOAD and persistence of a setting, with
patch 0040 and no other change.

**What is left for the memory card, in order:** (a) integrate 0040 (done by the orchestrator); (b) committed test
scripts: `save_from_options.txt` and `load_from_options.txt` retimed for skipped videos (they depend on the frame blanks
at 240/270 (boot dialog + OK), 2400/2760 (the two START taps of the title, timed with the video), 3000 (main menu), and
the relative waits +240 for OPTIONS, +60 per DOWN); (c) card states (step 7 below: no card, unformatted, full) and real
free space (step 6); (d) `SetFileInfo`/dates (step 8); (e) the other opcodes untested here (Format, Delete, the
"overwrite?" and automatic-save script 3000).

## 2026-10-13 follow-up: one memory card for every variant (`--mc-dir`)

The runtime puts the card at `IoPaths::mcRoot` (`ps2_runtime.cpp` ~341, `Kernel/Stubs/MemoryCard.cpp` ~119; `mc1` is the sibling of `mc0`), which
`loadELF` resets to `<ELF folder>/mc0`. A public setter exists (`PS2Runtime::setIoPaths`), so no runtime patch: the host program sets
`mcRoot = <dir>/mc0` after `loadELF` and the game setup (`src/host/host.cpp`). Source of the folder: `--mc-dir`, else `DON_MC_DIR`, else nothing
(the old behaviour: next to the ELF, so scripts and tests never touch a player's saves). The launcher passes `--mc-dir` with `DON_MC_DIR` or
`<data>/don/memcards`, and says in one line when a variant's own folder holds a save and the shared folder none (no copy, no delete).

Checked: `tests/test_launcher.py`, 23 passed. English variant, `save_from_options.txt` with `DON_PLAY_MOVIES=1` and `--mc-dir work/saves/mc-test` (empty):
`mc0/BISLPS-25675game/{battle_logo.ico, BISLPS-25675game, icon.sys}` appeared there; the variant's `mc0` and `mc1` were unchanged. The same
route with no option on a scratch folder of symlinks: the same three files in `<that folder>/mc0`.
Not verified: Windows paths, `--mc-dir` with a folder that cannot be written, a launcher start with real saves in both places.
