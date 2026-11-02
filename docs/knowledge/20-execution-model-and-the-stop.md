# 20 · How this port executes the game's code, and the stop

Written on 2026-10-13 from the journals of T-304 (010, 011, 023, 024, 026, 027, 028) and from reading the PS2Recomp tree
(`third_party/PS2Recomp/ps2xRuntime/`, paths below are relative to it). Nothing was built or run for this file.

**Conventions.** **Measured** = observed in a run, with the journal entry. **Read** = read in the code, with file and function
(line numbers move with the patches; the function name is the stable reference). **Supposed** = not checked. **Named by the
orchestrator** = told to the author of this file by the orchestrator with the evidence in journal 027's last follow-up, not checked here.
The tone and the thread/pacing model are in [13-runtime-host.md](13-runtime-host.md); what it already says is not repeated.

## 1. How generated code runs

- **Shape of the generated code.** One C++ function per game function, named `FUN_<addr>_0x<addr>`, plus `entry_<addr>_0x<addr>`
  fragments for labels in the middle of a function. In the present generation: 7,875 functions and 47,409 `entry_` fragments
  (`ls generated | grep -c` on 2026-10-13, **measured**).
- **The table.** `generated/register_functions.cpp` registers 191,241 addresses in a sparse table (base 0x100000, end 0x3902D0).
  Each address is either a function's own entry or a resume address for which the function it is registered to has a `case` in its entry
  `switch` (a function starts with a `switch (ctx->pc)` that jumps to the label). The agent of the check recorded it as verified
  mechanically on 2026-10-13 (**as told, not re-run here**).
- **Calls.** A guest `jal`/`jalr` becomes `runtime->dispatchGuestBranch(rdram, ctx, target, source, fallthrough, kind, name)`
  (**read**, `ps2_runtime.cpp`, `PS2Runtime::dispatchGuestBranch`). It sets `ctx->pc = target`, asks the scheduler for a checkpoint, and
  if none is due looks the target up and calls the C++ function. A jump (`j`, `jr`) is the same function with a non-call kind: it only
  sets `ctx->pc` and returns false.
- **Checkpoints.** Each dispatch charges `kGuestDispatchCycles` and calls `EeScheduler::checkpointDue` (**read**, `Kernel/EeScheduler.cpp`);
  loops use `runtime->eeCheckpointDue()` at back edges (`PS2Runtime::eeCheckpointDue`, `ps2_runtime.cpp`; **measured** to be present in the
  spin loop of `FUN_002D46B8`, journal 028). `checkpointDue` always does the cycle accounting (EE clock, timers, IOP batch); it returns
  true only when the "pending" flag is set or the cycle counter has reached `m_nextFastPathCycle` (the fast path of patch 0034) and then
  `slowPathCheckpoint` decides: an event is pending, a deadline (the next vertical blank) is reached, or the time slice (65,536 cycles) is
  over with another thread of equal or higher priority ready. Patch 0041 adds: a deferred interrupt is looked at again every 64 cycles.
- **What a due checkpoint does.** `dispatchGuestBranch` returns false with `ctx->pc` = the call's target, every generated function on
  the host stack returns at once, the whole host stack unwinds to `EeScheduler::run()` (**read**; **measured**: a gdb backtrace of the
  game thread shows only `entry_... <- EeScheduler::run()`, journal 027 follow-up). The scheduler later dispatches the saved `pc`.
  **Consequence:** every function must be resumable at every address a thread's `pc` can hold; each level of the unwound stack must
  resume exactly where it was, and a guest thread's state is only its `R5900Context` (registers and the guest stack in RAM), never a host
  frame.
- **For anyone debugging with gdb.** A breakpoint on a `FUN_<addr>` symbol is hit when that function is entered from above and when the
  scheduler resumes at an address registered to that function. It is **not** hit by a resume that goes through an `entry_<addr>`
  fragment (a resume at an address that has its own fragment): "0 entries" for a `FUN_` symbol does not mean it was not running
  (correction from the orchestrator, journal 028). To tell which is which: print `ctx->pc` (second argument, `R5900Context`) and the
  guest `ra` at the hit (`ctx` register file: gpr `r` is at `ctx + 16*r`, `ra` is r31, `sp` r29, `a0` r4); a fresh entry has `pc` equal
  to the function's own address and an `ra` outside the function; a resume has `pc` inside the function or at a label's address. Also
  break on the `entry_<addr>` symbols of the labels you care about, and watch guest memory through `rdram = $rdi` (first argument).
  Hits per blank counted at `ScreenshotWriter::on_vsync` (journal 011 method).

## 2. Interrupts and threads on top of that

- **Handlers are invocations.** `dispatchIrq(dmac, cause)` (**read**, `EeScheduler::dispatchIrq`) builds one `GuestInvocation` per matching
  enabled handler (own `R5900Context`, own stack from `invocationStackTop`), appends it to `m_pendingInvocations`; `run()` starts the head
  only when the running thread has no invocation open (so a cause arriving while a handler runs waits; it is kept). Handlers are
  started **between dispatches**, at the point where the scheduler has control, never in the middle of a guest function.
- **Patch 0041** (**read**, patch header and `EeScheduler.cpp`): the head of the queue is not started while it is an interrupt and the
  interrupted context has `Status.EIE` (bit 16, the bit the recompiled `DI`/`EI` clear and set, `ctx->cop0_status &= ~0x10000`), `IE`
  (bit 0), or `EXL`/`ERL` not in the enabled state. Before 0041 the scheduler never read Status and a handler could start inside the game's
  `DI ... EI` (journal 027). The end of a time slice is postponed the same way.
- **Two places where a cause is dropped instead of staying pending** (**read**, journal 011 correction; neither was ever observed in a
  run; **still open**): `EeScheduler::dispatchIrq` returns without keeping anything when the cause's bit is clear in `m_enabledDmacMask`/`m_enabledIntcMask`
  (`EnableDmac`/`DisableDmac`, `EnableIntc`/`DisableIntc`) or when no enabled handler exists for it; and `PS2Runtime::drainCompletedDmacHandlers`
  is called only after `Store32` and the GIF store paths (and `sceDmaSend`), so a cause queued by another store waits for the next one (kept, not lost). On the console a cause
  masked or disabled stays pending in the status register. The D_STAT mask bits (16 and up) are not consulted for delivery.
- **A DMA transfer completes at the moment the control register is written** (**read**, `ps2_memory.cpp`, `PS2Memory::processPendingTransfers`
  and `completeDmacChannel`): the pending bit of D_STAT is set and the cause is appended to `m_completedDmacCauses`
  (`queueCompletedDmacCause`), then delivered by `drainCompletedDmacHandlers` right after the store. So the game's pump sees the
  end-of-transfer interrupt queued before its own code after the store has run (the reason for 0041).
- **Measured in the stop** (journals 011, 027): masks 0xFFFFFFFF, D_STAT 0x306, DMAC channel 1 CHCR 0x70000045 (idle), no cause waiting, handlers
  for causes 1 and 2 (0x2D3A60), 2 (0x2D3FD0), 3 (0x138EE0), 9 (0x136BE0), 11 (0x117210) registered and enabled; the vertical-blank
  handler, the pump with a0=1 and the others run every blank. The interrupt side is clean there.

## 3. The game's frame

**Read** (decompiled with `tools/re/decompile.sh`, journal 028) unless marked.

| Function | What it does |
|---|---|
| `FUN_00132280` | Start-up of the main flow (loads, `FUN_00132470`, then a state loop on word 0x4D78F4: states 1, 2, 3 call `FUN_00131CA0`). The loop in `FUN_00132470` was named by the orchestrator; **not read here**. |
| `FUN_00131CA0` | The frame loop: set-up, then `do { FUN_00138B00 (per-frame logic); ...; FUN_00135C40 (frame end) ... } while (-1 < [0x4D78FC])`. |
| `0x138B00` | Per-frame logic (not read). |
| `FUN_00135C40` | Frame end: debug text, resets the spin counter [0x4D6900] to 0, calls the presentation callback `(*[0x4D6958])()`, increments the **frame counter [0x4D78E4]**. |
| callback = `FUN_00136530` | Calls `FUN_002C44F8(uRam004D8E64, 0, 1)` -> `FUN_002C9CC0` (calls `FUN_002BE2D0`, then `jalr` through the pointer [0x5406D8] = 0x2E1CD8; **measured**, journal 027). |
| `FUN_002E1CD8` | Presentation callback: `FUN_002F0570`, `FUN_002D4C18(0x544F30, n)`, `FUN_002E0898`; returns 1. |
| `FUN_002D4C18` | Builds the end-of-frame packet; if byte [gp-0x7560] is set calls `FUN_002D46B8` (return address 0x2D4D0C); then `FUN_002D48D0` (0x41 or 0x40 tag); DI loop; counter [gp-0x75D2]++; `FUN_002D3A60(-1)` if nothing is busy; EI. |
| `FUN_002D48D0` | Appends one packet (tag, address) to the display-list buffer; when the buffer is full (`puGpffff8a70 < next + 0x80`) it stores the entry and calls `FUN_002D46B8`. Calls `FUN_002D4240` first when words gp-0x75E0/-0x75DC are non-zero. |
| `FUN_002D4240` | Closes the list being built (not read in detail). `FUN_002D48D0` calls `FUN_002D4240`, and `FUN_002D4240` calls `FUN_002D48D0` behind a guard (**named by the orchestrator**). |
| `FUN_002D46B8` | The buffer flip: calls `FUN_002D4240`, then spins while `[gp-0x7568] != [gp-0x7564]` or byte [gp-0x7570] != 0 (callback [gp-0x7B98] = 0x136560, the spin counter), then DI, stores the new tail to [gp-0x7564] (0x2D47FC), toggles the double-buffer byte [gp-0x759C], EI, then `FUN_002D3A60(-1)` inside a DI/EI. |
| `FUN_002D3A60` | The queue pump and the DMA handler. Called with -1 (from the main thread), 1 or 2 (from the DMAC handler for channels 1 and 2). Starts the next VIF1/GIF transfer by storing the channel's CHCR; opcodes: 0x40/0x41 set the busy byte [gp-0x75D1]. |
| `FUN_002D3FD0` | INTC cause 2 (vertical blank start) handler; with a0 = 2 and busy set it clears busy, flips the display (writes DISPFB/DISPLAY), calls the pump. |

Words (gp = 0x4D1470; **measured**, journals 011, 026, 028): the queue words **0x4C9F08** (read pointer) and **0x4C9F0C** (tail), the counter byte 0x4C9F00, the busy
byte **0x4C9E9F**, the frame counter **0x4D78E4**, the scene ring index **0x535BE8**, the spin counter **0x4D6900**.
- **Two samples 60 blanks apart mislead:** the queue alternates between two buffers every blank (read pointer 0x5A9F08 or 0x69CD88, tail 0x5A9F10 or 0x69CD90), so
  any two samples an even number of blanks apart show identical words (journal 011, correction). Compare the frame counter and the ring index, not the queue words.
- **The spin counter 0x4D6900** wraps at 0x989681, is reset to 0 at the end of every `FUN_00135C40`, so a large value means the frame end has not run for a long time; in the
  stop it is in the hundreds of thousands at the first hit of a blank, in a healthy run it is small (journals 026, 028). It is not a sign of a *stuck wait*: the flip wait
  runs about 76,000 passes per blank in a healthy run and about 86,000 in the stop (journal 028).

## 4. The stop

- **Signature** (**measured**, journals 026, 028): the picture is constant; the frame counter [0x4D78E4] stops (6348 at every blank 7201-7204 of the
  reproduction); the scene ring index stops (194 in the fight, 406 in the sound stops); `FUN_00138B00` and `FUN_00135C40` are not entered; the body of the frame
  end (`FUN_002E1CD8`: `FUN_002F0570`, `FUN_002D4C18`, `FUN_002E0898`, the flip) runs once per blank; blanks, interrupts and sound go on.
- **Where seen:** journal 010 (a match frozen on "GET READY"); 011 (sound on: the title's fade-in; the character select); 023 and 024 (the title idle,
  sound off, with the port's own video skip: the picture stops at about blank 2460, later about 4440 on another binary; Start ignored); 026 (a fight with inputs: stops after
  blank 7080, the same blank in every run, about three minutes without sound). With patch 0041 in: the title with sound and the idle title still stop (journal 028, runs on 3 binaries; the earlier
  note that 0041 made the idle title animate was wrong and is corrected in `docs/STATUS.md`).
- **Readings that turned out wrong:** (1) "the game's queue is stuck waiting for an interrupt that never comes" (journal 011 follow-up 1): the two A dumps were 60 blanks apart, the queue
  alternates between two buffers; **corrected** in the second follow-up. (2) "no interrupt is delivered": measured delivered every blank in the stop (011, 027). (3) "0041 cured the idle
  title": it did not (028; the other agent's runs showed it stopping at about blank 4440). (4) "the main thread is in a wait that never ends": the flip wait ends once per blank (028).
  (5) "`FUN_002C9CC0` and its callers were entered 0 times, so they are not running": doubted for a while, because a breakpoint on a `FUN_` symbol does not see resumes through `entry_` fragments (section 1);
  for these functions the reading was right after all: every address of `FUN_002C9CC0` is registered to the function itself, and `FUN_002C44F8`, `FUN_00136530` and `FUN_00135C40` have no back edge, so there is no guest loop
  between the frame loop and the presentation callback (orchestrator, read in `generated/register_functions.cpp` on 2026-10-13). The caution of section 1 stands in general: check how the addresses are registered before trusting a count.
- **Cause, as named by the orchestrator** (status: **named by reading on 2026-10-13; confirmation and patch 0054 in progress**; evidence in journal 027's last follow-up): in `PS2Runtime::dispatchGuestBranch`
  (`ps2_runtime.cpp`, after the call `targetFn(rdram, ctx, this)`) the rule "if the callee returned with `ctx->pc == entryPc`, treat it as returned" (meant for native functions that do not set `pc`) also fires
  for a generated function that returns **unwinding** with `pc` equal to its own entry (a checkpoint was due at a nested call whose target is the same function). The game's packet-queue functions are recursive
  (0x2D48D0 -> 0x2D4240 / 0x2D46B8; 0x2D4240 -> 0x2D48D0; 0x2D46B8 -> 0x2D4240), so an unwind at a nested call of 0x2D4240 is taken as the return of the outer one, the outer code continues on the wrong frame,
  and a return address is loaded from a slot that holds the callback pointer 0x2E1CD8 (`FUN_002C9CC0` keeps it in `s2`, every function below spills `s2`): `FUN_002E1CD8` returns into its own entry for ever. **Measured**
  (journal 027): the first entry of `FUN_002E1CD8` with `ra = 0x2E1CD8` is at blank 7076 of the reproduction, on the buffer-full path of the queue (`FUN_002D48D0 -> FUN_002D4240 -> pump`); its prologue saves
  `ra = 0x2E1CD8` at 0x1F7FE30. **Not verified by me:** the claim that the `pc == entryPc` rule is the one that does it.
- **Shortest reproduction** (journal 026, about three minutes, sound off, sound is not needed): `DON_PLAY_MOVIES=1 build/linux-dev/don --headless --renderer software --shots work/control/ctl --shot-every 60 --input-script work/control/ctl.txt --seconds 1200 disc/SLPS_256.75`
  (the script is journal 026's `ctl.txt`: the committed route `tools/run/scripts/to_match_1v1.txt` to `match_load_2` at 6540, then inputs from 6700; the picture stops after blank 7080).

## 5. Investigating a stop of this kind

1. First read the **frame counter [0x4D78E4]** and the **ring index [0x535BE8]** at three blanks (a gdb breakpoint at `ScreenshotWriter::on_vsync`, counting blanks; enable the real breakpoints from a chosen blank: a Python stop on a hot symbol from the start makes a run extremely slow). Frozen counter = a stop; moving counter = the picture is just static.
2. Then read **where the main thread is**: guest `pc`, `ra`, `sp` from the context of any generated function the main thread runs (second argument), and the guest stack words that point into the game's code, each checked as following a `jal`/`jalr`. Stale words below `sp` are not frames.
3. Do not conclude from a wait loop being busy: the flip wait is a spin that ends once per blank in a healthy run too.
4. Do not trust "0 entries" of a `FUN_` breakpoint; count resumes through the `entry_` symbols too, and compare `pc` with the function's own address.
5. Do not compare two memory images a multiple of 2 blanks apart for the queue words.
6. Read the interrupt side in the stop (D_STAT, CHCR, masks, handlers, pending invocations) before blaming it: it was clean in every stop measured so far.
7. Compare with a healthy run at the same blank on the same binary and the same inputs; note the md5 of the binary and `git -C third_party/PS2Recomp status --short` for each run (another agent may rebuild).
8. A stop that appears at a fixed blank in a deterministic run is worth a watch on the guest memory that first holds the bad value (here, the saved `ra` slot).
9. gdb on this process: attach is blocked (`ptrace_scope=1`), run it under gdb with `--args`; the binary has no debug info, so structure offsets come from disassembly; calling runtime functions from a breakpoint script kills the inferior.
10. Keep one run at a time and stop only your own pids.
