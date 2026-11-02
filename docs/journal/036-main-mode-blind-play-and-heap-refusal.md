# 036 · Main mode played blind; the IOP heap refusal seen once

- **Date:** 2026-10-14
- **Task:** T-210
- **Status:** partial (the end of main mode was NOT reached; one heap refusal reproduced)

Player folder commit: `68fa31a86ebad1a2c8725478f03ba919dcf0ac84` (BUILT_FROM.txt), binary md5 `0940d6d5cc73...`.
English data (en-jpvoice), headless, software renderer, sound on, own card folders under `work/t210/`.

## Matches seen

| # | Opponent | Stage | Result | Notes |
|---|---|---|---|---|
| 1 | Sakura | ship deck | lost on time (a1, b1); won by KO ~20500 (c1) | a blind bot wins by luck; Sakura's bar recovers |
| 2 | (mission box) | temple courtyard | time up ~33100, lost, CONTINUE? answered YES by Left+Circle, replayed from ~33800 | |
| 2 (d2, card with Auto save ON) | Frieza, Vegeta, Chopper (4 fighters, cloud stage) | | Vegeta won repeatedly (~19200, ~43800), time up | the loaded card started a different match |

Liveness: a1 30000 blanks, b1 11000, c1 40000, d1 24000, d2 46000: all `ok` (largest gap 120-180 vsyncs, limit 300). Wav: no
silence or saturation problems (c1 rms 0.169, silent 0.074, saturated 0.0002). Blind play won about 1 of 7 matches, so
the later matches, the boss, the ending, the credits and the unlocks were not reached.

## Heap refusal (owner's freeze)

Reproduced once in c1 (40000 blanks): line 841 of the log, between vsync 21120 and 21180, right after match 1 ended in
a KO (~frame 20520): `heap request refused (#1 of this size): size=0x8000 alignment=64 fixed=0 largest free gap=0x6780 total free=0x6840`.
The frame counter paused for about 2 s there (frames 20520 to 20580, a loading screen after the KO), then went on. The
owner's freeze was longer (4 refusals, 15 s, ended by Start). Not seen in a1, b1, d1, d2 (0 refusals).
Journal 011: the game never frees IOP heap in the first 3720 blanks; one more 0x8000 fits, two do not; the 0x8000 is the
sound-bank loader's per-stage request. So each new stage loaded adds 0x8000 and the margin (0x6840 free) runs out at
the third distinct load. Not verified: whether the console has more free there (work/ref dumps not checked), whether
the game retries until freed, whether Start ends a retry. Who asks is not logged (patch 0058 note: a gdb backtrace on
IopMemory::allocate gives it); gdb was not used.

## Saves

OPTIONS: AUTO SAVE ON asks a confirmation (Left+Circle = YES). Leaving OPTIONS with Cross asks "save?" (cursor on NO;
Left+Circle = YES) and writes `mc0/BISLPS-25675game/` (992-byte data, icon.sys, battle_logo.ico). With that card the
next run's main mode started a different match (4 fighters).

Scripts: `tools/run/scripts/main_mode_mash_loop.txt`, `main_mode_autosave_loop.txt`.
