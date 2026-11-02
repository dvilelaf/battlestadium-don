# 027 · Interrupt handlers started inside the game's DI ... EI (T-304, patch 0041)

## Question
Does the scheduler start an interrupt handler inside the game's protected section "start the DMA, then mark the queue busy",
and is that the cause of the stop (one packet pending at 0x4C9F08/0x4C9F0C, main thread spinning in FUN_002D46B8)?

## What the game's code shows (decompile.sh 0x2D46B8 and 0x2D3A60)
- FUN_002D46B8 (the wait/pump): `di; sync; mfc0 v0,Status; and v0,v0,0x10000; bne ... (loop back to di)`, then it sets the
  queue pointers, `ei` (0x2D4808), flips the buffer index. Later (0x2D4890..0x2D48B4): `di`, the same Status check, `jal 0x2D3A60`
  with argument -1, `ei`. So the start of a transfer is called inside DI ... EI.
- FUN_002D3A60: with argument 1 or 2 (the DMA end-of-transfer handler) it first decrements the busy counter at gp-0x7570 if it
  is not zero; then, if the counter is zero and the queue has an entry, it sends it (DMA start) and marks busy (counter set)
  AFTER the control-register store. It ends with `ei` (0x2D3F98). The kernel's interrupt calls are not used: DI/EI are the
  instructions, recompiled as clearing/setting Status bit 16.
- So the hypothesis holds on paper: an interrupt taken between the store and the busy mark finds the counter zero and does
  nothing; the busy mark then waits for an interrupt that is not coming.

## Mechanism in the runtime
`EeScheduler.cpp` never read `cop0_status`. A DMA start finishes at once and queues its handler; `run()` starts it at the next
checkpoint (every guest call), inside the section.

## Patch 0041
The head of the pending queue is not started while it is an interrupt (Interrupt, Alarm, GsCallback) and the context it would
interrupt has EIE (bit 16), IE (bit 0) clear or EXL/ERL (bits 1, 2) set. It stays queued in order. `m_interruptsDeferred` makes
the slow path look again every 64 EE cycles (8 dispatches; after an EI nothing calls the scheduler, and waiting for the IOP
batch, 256 dispatches, would be slower and was not needed). The end of the time slice is postponed the same way (the console's
timer interrupt cannot arrive in the section either; otherwise another thread's handler could touch the same queue).
Idle path (no thread running) and the runtime's own invocations (RPC, SIF, HLE, syscall override, exit handler) are not gated.
Only the head is looked at, so nothing is reordered; nothing is dropped or duplicated. Left alone: a cause arriving while its
mask/handler is not enabled is still dropped (journal 011).

## Test
`tests/runtime/irq_di_section_test.cpp`: 300 rounds of DI; `dispatchIrq(true, 1)` (what a finished channel-1 transfer calls);
40 small guest calls; set busy; EI; the handler clears busy. Base tree: `handlerCalls=300 insideSection=300 sawNotBusy=300
neverCleared=300` (the stop, 300 times). Patched: `handlerCalls=300 insideSection=0 sawNotBusy=0 neverCleared=0`, three runs in
a row. pacing_neutral_test and scheduler_fast_path_test pass patched (12.47 ns per dispatch).

## The game, with the patch on the shared tree (build 35 s)
- a. Journal 026's route (`DON_PLAY_MOVIES=1`, ctl.txt, Japanese folder, shots every 60): the picture still stops at blank 7080;
  s7080..s7510 are byte-identical (shot s7080 is a fight frame). The patch does not remove this stop.
- b. Idle title (English, run2.txt): the title keeps changing until at least blank 3960 (journal 023 measured it frozen from
  about 2460 on an older binary; no run of the unpatched current binary was made here, so this is not a clean before/after).
  After Start (3960) the shots change until frame 4200, then the picture is the same to 4560, and it is a garbled title
  (after_start.png), not the menu.
- c. Not run (time box).

## Conclusion
The section is real and the runtime did start handlers inside it (the test reproduces the stop's state exactly); the patch makes
the runtime respect it. It is not the only cause of the fight stop: that stop at 7080 persists. Next: with the patch on, read the
stopped state again (queue words, busy counter at gp-0x7570, whether the handler is registered and its cause enabled), since the
dropped-cause gap of journal 011 is now the main suspect.

## Follow-up 2026-10-13: with 0041 in, the stop is a self-return of FUN_002E1CD8 (cause not found)

Reproduction of journal 026 (`DON_PLAY_MOVIES=1`, `work/control/ctl.txt`, Japanese folder), binary with patch 0041 and a temporary
diagnostic in `EeScheduler.cpp` (`DON_TRACE_IRQ=<blank>`, removed; the file is back to the series).

Measured at blank 7080-7520 (stop): main thread status 0x40010001, no invocation pending, nothing deferred, DMAC and INTC masks
0xFFFFFFFF, D_STAT 0x306, channel 1 CHCR 0x70000045, busy byte 0x4C9E9F = 1, counter 0x4C9F00 = 0, queue words alternate between
(0x5A9F08, 0x5A9F10) and (0x69CD88, 0x69CD90). Every blank: cause 2 (vblank) handler, DMAC cause 1 handler (pump, a0=1: counter 1 then 0),
causes 3 and 9 handlers. No `DROPPED-MASK`, no deferral, no handler left waiting: the interrupt side is clean. What changes at blank 7076-7077 is that
DMAC cause 8 (SPR) goes from about 130 per blank to 1, because the frame loop above has stopped.

What the main thread dispatches per blank in the stop (spin at 0x136584/0x136598/0x2D47xx left out):
`2D47D0 -> 2D4890 -> 2D4D0C (FUN_002D4C18 after FUN_002D46B8) -> 2E1D1C (FUN_002E1CD8 after FUN_002D4C18) -> 2E1CD8 (ra = 0x2E1CD8, sp = 0x1F7FE50)`
and again. So FUN_002E1CD8 returns into its own entry address: its prologue saves ra = 0x2E1CD8 at 0x1F7FE30, so each return goes to itself. It is
self-sustaining; the frame loop above never gets control. The pointer [0x5406D8] is 0x2E1CD8 (the callback FUN_002C9CC0 calls with `jalr`), normal.
First entry with ra = 0x2E1CD8: blank 7076, a0 = 0xFD9B88, a1 = 0x6C5CE0 (a display-list packet that FUN_002D48D0 was posting), right after the buffer-full
path (FUN_002D48D0 -> FUN_002D4240 -> pump) ran in the same dispatch as the frame loop's native chain 131CA0 -> 135C40 -> 136530 -> 2C44F8 -> 2C9CC0 -> 2E1CD8 -> 2D4C18 -> 2D48D0 -> 2D4240.
Not found: which return (or call) first delivered ra = 0x2E1CD8. A hardware watchpoint on the stack slot 0x1F7FE20 for the value 0x2E1CD8 hit inside FUN_002D4240 with a normal native chain: that is
very probably a callee-saved register (s2 holds the callback pointer 0x2E1CD8 in FUN_002C9CC0) being saved, so it is not the origin; I did not confirm it.
Next step for whoever continues: watch the slot at 0x1F7FE30 (saved ra of the first FUN_002E1CD8 frame) at blank 7076 and print the guest sp and ra at the hit.
