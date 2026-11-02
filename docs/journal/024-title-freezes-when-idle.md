# 024 · The title freezes when it is left alone (the memory card is not the cause)

- **Date:** 2026-10-13
- **Task:** T-113 (follow-up of journal 023)
- **Status:** open (cause not determined; one premise of the task is measured false)

## Symptom

Journal 023: the title appears at about blank 2060-2100; left alone, the picture stops changing at about blank 2460
and Start does nothing afterwards. Reproduced with
`build/linux-dev/don --headless --renderer software --shots work/shots/idle --shot-every 60 --seconds 300 --input-script work/idle/run.txt en_patch/jpvoice/disc/SLPS_256.75`
(`work/idle/run.txt` = a copy of `work/t113/run2.txt`). Of 80 shots, 36 in a row at the end are byte-identical.

## Classification

Not a crash, not a hang of the host. Not a card problem (see below).

## Investigation

### 1. The memory card is NOT polled after the boot dialog (the premise of the task is false)

- The whole log of the 300 s run holds exactly **189** `[MC] GetInfo` / `[MC] Sync cmd=1 result=0` pairs and **one** `[MC] GetDir
  '/BISLPS-25675game/*' -> result=-4` (right after the last IOP module, id 14 `iopbuf:fnv64:b67c95016e73bd74`, is loaded at
  boot, not at 2460). 189 pairs is the boot dialog: one per blank from about blank 120 to the answer at 330.
- Checked a second way, under gdb: a breakpoint on `ps2_stubs::sceMcGetInfo` with `ignore 1 189` is never hit again in a run
  that goes to blank 4560. A gdb count of every `ps2_stubs::` and `ps2_syscalls::` handler up to blank ~1560 gives
  `sceMcGetInfo` 189, `sceMcSync` 190 and nothing else of the card. So at blank 2460 and later **no card function is
  called**; "GetInfo and Sync every frame without end" is the dialog's polling, seen at the end of the log only because
  the log is block-buffered and the card lines are the last lines in it.
- The module that is loaded last in the log is therefore not related to the freeze either.
- So the runtime's `sceMcGetInfo` (`MemoryCard.cpp:859`, result 0 and "formatted, 8192 free" every time) and
  `sceMcSync` (`:1193`) cannot be what holds the title. Whether the console reports "new card" (-1) on the first
  information call, as the task says, is true of the library and the game's boot script then takes the "formatted, no
  game data" branch the runtime already shows; that is a different question (journal 022) and has no connection to the title.

### 2. What the game is doing at the freeze (measured)

- The game thread is alive: `DON_TRACE_STATE=60` RAM dumps at blanks 3000 and 3540 (`work/idle/dump/`, not versioned) differ
  in 35,562 words of the game image + heap. Per-blank counters advance by exactly the number of blanks: `0x3D1208`
  (ADX vsync handler's counter, `0xBB3 -> 0xDCF` = +540 over 540 blanks), `0x4C998C` (`0xB39 -> 0xD55`, +540), `0x3F1888`
  (`0xBAB -> 0xDC7`), `0x3D2350/58/70`; `0x3DDD24` grows by 4 per blank (`0x1F79 -> 0x27E9`). The ADX audio streaming
  buffers (`0x50D3C0..0x52FD60`, 35,433 words) keep being rewritten: the background music is still being streamed.
- The EE thread table (`DON_TRACE_THREADS=1`) is the same at blank 60 and at 4500: t1 (the main thread, `pc=0x0031587C`, the
  ADX vsync callback `FUN_003157e8`'s return after the call to `0x314fb8`), t3/t4 asleep in `SleepThread`, t2/t5 suspended. It
  does not discriminate: it is also what it prints during the healthy part of the run.
- gdb stack samples of the game thread (6 at blank > 3000, 6 at blank ~1500) show no guest `FUN_*` frame in either case:
  the thread is inside `EeScheduler::run` -> `accountCycles` -> the IOP emulator (the IOP runs at about 33 M instructions per
  second: one IOP thread is busy-waiting, as in journal 009/T-111) or in `processPendingEvents`. Not informative.
- Shots at 2340 and 3000 both show the title with "PRESS START button" (`work/shots/idle5/frame_002340.png`,
  `frame_003000.png`), so the title is up and static, not blank.

### 3. Hints that point at the skipped video, not at the card

- With `DON_PLAY_MOVIES=1` (journal 023) the title left alone to blank 6000 did not freeze. The only thing changed between
  the two runs is `donSkipMovie` (`src/game/don_overrides.cpp:109`), which returns 1 for opcode `0x50` of the script
  interpreter. What the video player leaves behind is described in journal 023 (GS reset, and the VBLANK callback `0x331F20`
  that calls `FUN_00315910(0)`, then `EI`): that callback is **not** what the game uses afterwards, but the player's other side
  effects (state that the title scene's timer or the "attract" logic reads) were never checked. This is a guess; nothing here
  shows it.

## Cause

Not determined. What is excluded: the memory-card handlers (no call after the dialog), the IOP module loaded last (loaded at
boot), a host hang (the blank counter, the per-blank counters and the audio streaming advance).

## Fix

None. No patch was written (step 4 of the task applies only if the cause is in the card handlers).

## What to do next (each is a short run)

1. Find the game's current script and its pointer at the freeze: the interpreter `0x1C7BC0` is reached from the scene loop
   (`FUN_001c9410`; the gdb bt at the dialog was `sceMcGetInfo <- FUN_001c7bc0 <- FUN_001c9410 <- EeScheduler::run`). A gdb
   breakpoint on `FUN_001c7bc0_0x1c7bc0` hit-counted after blank 3000 says whether the scene interpreter is still being entered
   once per frame at the freeze; if it is, print `$a2` (script pointer, saved in `s2`) and the opcode `(*a2>>8)&0xff` for the
   few distinct values, and map the pointer to a script in `work/saves/ex/106` (the DATA.AFS entry used in journal 022).
2. If the interpreter is NOT entered: the main thread is blocked inside a guest wait that `ps2_syscalls` hides; log the
   guest `pc` of t1 at the end of a vblank handler in `dumpThreads`, not the callback's own.
3. Run the same script with `DON_PLAY_MOVIES=1` and `DON_STATE_DUMP_AT=3540`, and diff the RAM against `ram_3540.bin` of this
   entry restricted to `0x4C0000..0x540000` (game variables): the words that differ are the state the video leaves behind.
4. A cheap experiment: let `donSkipMovie` also run what the player's start does to game variables (needs the decompile of
   `0x36A820`), only as a diagnostic.

## Verification

Commands run, all headless, one at a time: the reproduction above; `DON_TRACE_THREADS=1 DON_TRACE_IOP=1`; `DON_TRACE_STATE=60`
with `DON_STATE_DUMP_AT` 2160, 3000, 3540 (`work/idle/diff.py` compares two dumps); gdb breakpoints on `ps2_stubs::sceMcGetInfo`
(`work/idle/run.gdb`) and a counting run (`work/idle/count.py`). Scratch is under `work/idle/` and `work/shots/idle*`; the
RAM dumps are game memory and stay there (rule 1).

## Temporary diagnostics

None added to the repository. Existing env variables only (`DON_TRACE_THREADS`, `DON_TRACE_IOP`, `DON_TRACE_STATE`,
`DON_STATE_DUMP`). Process note: while the work was going on other agents' `don` and `gdb` processes were running; two
`pkill -x don` / `pkill -INT -x don` calls of this investigation (before this was noticed) may have interrupted runs that were
not mine. Use pids, never names.
