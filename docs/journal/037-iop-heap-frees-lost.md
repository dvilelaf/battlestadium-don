# 037 · The game's IOP heap frees were lost: the freeze after a few fights

- **Date:** 2026-10-14
- **Task:** T-213
- **Status:** fixed (one binding in `src/game/don_overrides.cpp`)

## Symptom

The owner, playing main mode: after about three fights the game froze for about 15 s and went on when Start was
pressed. The log showed four `[ps2xIOP] heap request refused: size=0x8000 ... largest free gap=0x6780 total free=0x6840`.
Journal 036 reproduced the refusal once (route c1, `work/t210/`), after match 1's KO.

## Cause

The game's own `sceSifFreeIopHeap` (`0x1146C8`) was recompiled, not bound to the runtime's handler. Its code sends an
RPC to the IOP heap server (SID `0x80000003`), which the port does not serve, so every free the game made was lost.
The sound-bank loader allocates `0x8000` per stage load: with route c1 and a temporary trace of every IOP allocation
and free, thirteen such blocks went from `0x181880` to `0x1E1880` and the fourteenth was refused. Journal 036's
"the game never frees" had counted only the bound entry points, so it missed this path (corrected here).

The console's IOP RAM (dumps in `work/ref`) is in use up to about `0x1E0000` and non-zero up to `0x1FFFBE`; the port's
`HeapLimit 0x1F0000` costs at most 64 KB. With the frees working the limit is not what runs out, so it is unchanged.
The console's free list was not parsed: no per-block console numbers.

## Fix and checks

- `kHandlerBindings`: `{0x001146C8u, "sceSifFreeIopHeap"}`. Test `tests/test_heap_free_binding.py` pins the binding
  (fails with it removed, passes with it).
- Route c1 on the fixed build, to frame 28140 (its `--seconds 480` limit), well past the old refusal at vsync ~21120:
  0 refusals, 28 frees, 14 allocations of `0x8000`; liveness ok (largest gap 180 vsyncs).
- The trace (`DON_TRACE_IOPHEAP`) was a temporary diagnostic in the shared runtime tree and was removed.

Not verified: the owner's own case (four fights in a row) and main mode beyond match 2.

## Follow-up: the practice fight's match load is about 180 blanks longer, and that is the right timing

The regression route `fight_practice` changed from blank 6540 on: at 6800 the timer shows 180 where it showed 177. A/B on one
build (a temporary switch that skipped the binding, removed): without the binding the run reproduces the old landmarks exactly,
and at vsync ~6000, just after the stage is confirmed, the match load's `0x8000` sound-bank request was **refused** (the 27 frees
before it had freed nothing). With the binding there is no refusal and the bank loads: the same screens, about 3 s later. So the
old, faster load was the bank load failing; the console, which has the memory, loads it. The old path's free (`0x114740` ->
`0x1146C8` -> the replaced `sceSifCallRpc`) returned 0 synchronously, as the binding does; the callers do not use the result.
The route's landmarks from 6540 on were re-recorded (`record.py fight_practice`, then the test passed again on the same binary).
