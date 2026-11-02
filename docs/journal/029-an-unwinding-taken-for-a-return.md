# 029 · An unwinding taken for a return (T-304, patch 0054)

Date: 2026-10-13. Follows journals 026, 027 (the frame-end callback returns into its own entry) and 028.

## The rule

`PS2Runtime::dispatchGuestBranch` ends with "if the callee came back with `pc == entry`, set `pc` to the
fall-through and report a normal return". That rule exists for native stubs that return without setting pc.
When a checkpoint answers "due" at a nested dispatch, every generated caller returns in turn (`if
(!dispatchGuestBranch(...)) return;`) with `pc` = the resume address. If that address is the entry of a callee
that is active further up the host stack (guest recursion), that level takes the unwinding for a return and
continues its own code on the wrong frame.

## The recursion in the game

FUN_002D48D0 calls 0x2D4240 and 0x2D46B8; FUN_002D4240 calls 0x2D48D0 behind its re-entry guard; FUN_002D46B8
calls 0x2D4240 first. A checkpoint due at one of those nested dispatches reproduces the rule's misfire. FUN_002C9CC0
keeps the callback pointer 0x2E1CD8 in s2 and every function below spills s2, so a return address read from the
wrong frame's s2 slot is 0x2E1CD8 (journal 027's first bad entry).

## Confirmation (temporary print, removed)

In the reproduction of journal 026, with patch 0041 applied, the rule fired on an unwinding exactly once in the
whole run:

    [diag] UNW-FIRE blank=7076 entry=2d48d0 ra=2d4674 sp=1f7fdf0 src=2d4d34

That is blank 7076, the first bad blank, from FUN_002D4C18 (call at 0x2D4D34) into FUN_002D48D0. The entry is
0x2D48D0 rather than 0x2D4240 or 0x2D46B8 as predicted; the mechanism is the same (a recursive callee's entry
equal to the unwinding pc). No other function fired and no earlier "mild" firing happened.

## The fix

`EeScheduler::checkpointDue` sets `m_unwinding` when it answers "due"; the run loop clears it right before it
calls a guest function; `dispatchGuestBranch` returns false after a callee comes back while the flag is set.
One byte load on the path after every call. The flag is only set between the answer and the run loop, during
which no guest code runs, so a native stub's return is never taken for an unwinding.

## The test

`tests/runtime/unwind_not_return_test.cpp`: a small guest written like generated code (mutual recursion A/B/C
behind a guard, a callee-saved register holding a code address, a loop, an indirect call, a native function that
returns without setting pc). A reference run, then one run per k with a checkpoint forced due at the k-th
dispatch or back edge. Unpatched: 6 of 32 differ (k = 3, 11, 13, 14, 18, 27; at k=3 the guest ran A 16 times
instead of 11 and the native function 15 times instead of 8). Patched: 0 of 32. Also passing on the patched tree:
irq_di_section_test, pacing_neutral_test, scheduler_fast_path_test (14.83 ns base, 15.31 ns patched per dispatch,
the same within noise; that test does not exercise many calls through dispatchGuestBranch).

Not covered: a checkpoint due exactly at the dispatch of a native function that never sets pc makes the
scheduler call it as the resume address, and it returns with pc unchanged, so it runs again, forever. This
happens with and without the patch; the test avoids that dispatch. Not known to happen in the game.

## The three runs (patch applied to the shared tree, build linux-dev, software renderer, headless)

- a. Reproduction of journal 026 (`work/control/ctl.txt`, Japanese, movies on), shots every 60: ran to the end
  of the script (exit 0); all 63 shots from blank 6900 to 7510 are different from each other; the shot at 7500
  shows a character in a lightning effect, so the fight is going on past the old stop at 7076.
- b. Idle title (English voices folder, dialog answered, nothing until blank 6000, then Start, run to 7300):
  every shot from 4200 to 7260 differs from the previous one (the title animates past 4440), and the shot at 7300
  is the main menu (Start opened it).
- c. `--sound on`, `DON_PLAY_MOVIES=1`, `tools/run/scripts/to_match_1v1.txt`, Japanese folder: ran to the end of
  the script (exit 0, blank 7800), past the title's fade-in that stopped at 2520; match_3 (blank 7740) shows
  the fight on the ship's deck with the health bars and the characters positioned differently from earlier shots.
  Of the 89 shots from 2400 to 7800, two pairs of consecutive shots are identical (loading screens).

Limits: the shots were looked at, not input-by-input; what player 1 does at each input in run a was not
checked frame by frame.

## Follow-up 2026-10-13: do host replacements rely on "pc equals entry"? Yes, six of them

Read from a scratch worktree with 0054 applied; nothing was built or run in the shared tree.

| family | sets pc itself? |
|---|---|
| generated functions (FUN_/entry_, about 57,000 files) and the SDK-named wrappers (syscall wrappers like AddDmacHandler: `jr ra` sets pc) | yes |
| stub and syscall wrappers the recompiler writes (`ctx->pc = ra` before calling the `ps2_stubs::` / `ps2_syscalls::` handler) and the call sites that inline them | yes (pc = ra is set before the handler runs) |
| `replaceFunction` that wrap generated code (donFrameCounter, donVblankHandler, donWrite for descriptors other than 1 and 2; only with DON_TRACE_* or other descriptors) | yes (the generated code sets pc) |
| host replacements: donMtapInit 0x255CA0, donMtapPortOpen 0x255DC0, donMtapPortClose 0x255E30, donMtapGetConnection 0x255EA0 (each about once or a few times per session, plus GetConnection every frame by the pad poll 0x138FB0), donWrite 0x10C868 for descriptors 1 and 2 (only the game's own TTY prints) | **no** (M = 6 with the next row) |
| `ps2_stubs::sceCdSearchFile` bound directly at 0x11A7A8 by `kHandlerBindings` (a handful of calls at load) | **no** |

N is the generated code and the wrappers; M = 6: the four multitap wrappers, donWrite's host path, and
sceCdSearchFile. `EeScheduler::run` never sets pc to ra for a function that left pc alone, so a host replacement
that the run loop dispatches (a checkpoint due exactly at its call) comes back with pc unchanged and is called
again, for ever, with its side effects repeated. The multitap GetConnection is the likely one in a session: it is
called every frame; the main thread would stay in it (the picture frozen, the game's frame loop not running; the
vblank handler and interrupts keep running). Not observed in any run so far; the chance per call is about the
checkpoint interval over the calls.

Patch 0055 (not in the series; for the orchestrator after the regeneration): `replaceFunction` records the slot as
a host implementation, `registerFunction` clears it (the runtime's tests register guest-shaped functions that return
with pc unchanged to be called again, which the first attempt broke: irq_di_section_test, pacing_neutral_test,
scheduler_fast_path_test, ee_timer_test); the run loop continues at ra when a recorded slot comes back with pc
unchanged. The rule in dispatchGuestBranch stays, so the rule is not dead code. The test of 0054 now forces the
checkpoint at the native function's dispatch as well: unpatched 8 of 40 runs differ (k = 8 9 18 23 28 29 38 39,
the native function called 1001 times), patched 0; the 21 host tests pass on the patched tree. Not verified in the
game. A host replacement that blocks or changes the running thread and relies on this rule would not be handled
(none of the six does).

Two hardenings of 0055 (2026-10-13): the run loop's rule also requires that no unwinding is in progress (the flag of
0054), because a replacement that wraps generated code (donFrameCounter, donVblankHandler, donWrite) can be unwound by
a checkpoint with pc equal to its own entry and must then be called again; the test forces two checkpoints in a row
(at the replacement's dispatch and at the back edge inside it): without the condition 6 of 49 runs differ, with it 0.
`nativeFunctionSlots()` now grows to the table's present slot count on demand and `ps2IsNativeFunction` answers false
for a slot beyond it.
