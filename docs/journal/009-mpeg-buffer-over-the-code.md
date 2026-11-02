# 009 · The MPEG stub writes the game's MPEG structure over a float routine of the game's code

- **Date:** 2026-10-13
- **Task:** T-114 follow-up (the 0x1717BC finding of journal 008)
- **Status:** open

## Symptom

The port's guest RAM has a pointer where the disc's executable and the console's memory image have an
instruction:

| source | word at 0x001717BC |
|---|---|
| `disc/SLPS_256.75` (the ELF, first LOAD segment 0x100000-0x4C9D80) | `0x46011041` |
| console, `work/ref/x/01/ee_ram.bin` (title) and `02` (menu) | `0x46011041` |
| port, `work/ramdump/t114-title.bin` | `0x0140EF58` |
| port, `work/ramdump/t114-menu.bin` | `0x0140EF58` |

0x46011041 is an FP instruction of a coherent MIPS float routine (the neighbouring ELF words are
0xE7A10048, 0xC6020000, 0xC6212110, 0x46012040, 0x46000834), so the word is **code**, and the port has a
value that looks like a pointer into the game's heap. A recompiled program does not execute guest memory,
so this does not stop the game; it means a write landed where it should not.

## When (bisection over vertical blanks)

`DON_DUMP_RAM` with a script that follows the route and quits at frame F (the dump is the 32 MiB of guest
RAM; the word is at file offset 0x1717BC, little endian):

| dump | frame it ends at | 0x1717BC |
|---|---|---|
| run.bin, run60.bin (`tools/run/smoke.sh`, the memory-card dialog) | ~180, ~300 | 0x46011041 |
| `work/ramdump/t114bis-1200.bin` | 1200 | 0x46011041 |
| `work/ramdump/t114bis-2400.bin` | 2400 | **0x0140EF58** |
| `work/ramdump/t114-title.bin` (the route, quit at 2760) | 2760 | 0x0140EF58 |
| `work/ramdump/t114-menu.bin` (the route, quit at 4440) | 4440 | 0x0140EF58 |

The same readout for the 13 other words, at the same frames (they are all zero in the ELF, so they are
zero-initialised data the game fills at run time):

| dump | 0x1717BC | 0x3D4B88 | the 0x3EA1xx table | 0x4C97B4 |
|---|---|---|---|---|
| dialog (~180-300) | 0x46011041 | 0x003D5C10 | 0 | 0 |
| 1200 | 0x46011041 | 0x003D5C10 | 0 | 0 |
| 2400 | **0x0140EF58** | **0x01F80000** | 0 | 0 |
| title 2760 | 0x0140EF58 | 0x01F80000 | 0x0013A600, 0x00139300, 0x00139FC0, … | 0x0069C980 |
| console 01 (title) | 0x46011041 | 0 | 0x00057C00, 0x00054000, 0x00054800, … | 0x0069C980 |

So they change in **two different moments**: 0x3D4B88 (which holds the main stack top, 0x01F80000 - the
Sofdec layer creates a thread in the same window) together with the code word, and the 0x3EA1xx table
between 2400 and 2760 (a table of pointers the game builds). The console's images are from the *start* of
the title screen while these dumps are a few seconds into it, which explains value differences in structures
the game keeps writing; the *code* word is the one that cannot legitimately differ, because the game never
writes its own instructions.

So the write happens between frame **1200 and 2400** (the logos end at ~1830, the opening video's first
black frame is ~2100). The **13 other words** with a heap-range value in the port and something else in the
console all sit in **zero-initialised data**: the ELF holds 0 at every one of them
(0x3D4B88, 0x3D5798, 0x3D57A0, 0x3D5884, 0x3EA108/10C/110/114/148/14C/150/154, 0x4C97B4) and the console
holds other values there (0x67C00, 0x64000, 0x64C00, 0x70030C00, 0x78F800, …): they are tables the game
itself builds (both machines do, at different moments), **not** the same defect.

## Who (a hardware watchpoint on the guest RAM word)

`work/orchestrator/t114/watch-args.gdb`: break on `ps2_stubs::scePadRead` (called once per frame by the
game's pad poll), take the guest RAM base from its first argument, `disable` that breakpoint so exactly one
watchpoint is armed, then `watch *(unsigned int*)(base+0x1717BC)`. It stopped at frame ~1300-2400 in:

```
Old value = 1174474817   (0x46011041)
New value = 21032792     (0x0140EF78, a guestMalloc(0x600,8) block)
#0  ps2_stubs::sceMpegCreate(uint8_t*, R5900Context*, PS2Runtime*)
#1  PS2Runtime::dispatchGuestBranch(...)
#2  FUN_0035d010_0x35d010   ... #4 FUN_0035ad88_0x35ad88 ... #6 FUN_00363f60_0x363f60
#8  FUN_0035f140_0x35f140   ... #10 FUN_0035f000_0x35f000 ... #12 FUN_00344e50_0x344e50
#14 FUN_003459b0_0x3459b0 (mwPlyCreateSofdec) ... #16 FUN_0036a820_0x36a820 (the Sofdec entry)
```

`config/stub_policy.toml` has `sceMpegCreate = "stub"`, and the analyzer's map puts the game's
`sceMpegCreate` at guest **0x00108FA8**; the recompiled `FUN_0035d010` calls exactly `FUN_00108fa8` there.

## Why

`sceMpegCreate` (`Kernel/Stubs/MPEG.cpp`) writes the game's MPEG structure at the buffer the game passes:

* `uVar3 = (a1 + 3) & ~3`, and the watched store is `mpegGuestWrite32(rdram, uVar3 + 0x44, allocResult)`
  with `allocResult = runtime->guestMalloc(0x600, 8)`: 0x1717BC = `uVar3 + 0x44` gives **`uVar3` = a1 =
  0x171778**, i.e. the game asked for an MPEG buffer **inside its own code**, and the handler wrote the
  structure (0x1xx bytes, and a 0x600-byte guest block) there. The other stores of the same call are at
  `uVar3 + 0x108`, `+0xB4`, `+0x1C` (0x1209F8) … and they do not show in a heap-value scan because their
  values are 0 or small constants.
* The decompile of the caller (`tools/re/decompile.sh 0x35d010`) shows the buffer's origin:
  `iVar1 = *(int*)(player + 0x1FC0)` (the player object of doc 19 §1), `FUN_00108fa8(iVar1+0x1150, uVar2,
  iVar7)` with `uVar2 = *(int*)(iVar1 + 0x28)` and `iVar7 = (*(iVar1+0xC) * *(iVar1+0x10) * 9) / 2 + 0x2800`
  (the frame size, 640x224 for this video). On its first use `*(iVar1 + 0x28)` is set to
  `align(*(iVar1 + 0x14))`, i.e. **the pool's base field**; later calls walk that ring forward.
* So the handler is **faithful**: it writes where the game tells it to. The bad address is the *game's*
  value `*(iVar1 + 0x14)`.
* The same run's *later* `sceMpegCreate` call is fine: the constant 0x001209F8 (which the handler writes at
  `buffer+0x1C`; it is the stub's stand-in, not a console value) appears exactly once in the port's dumps,
  at **0x00FCD000**, i.e. a legitimate heap address; the console's images have no 0x1209F8 at all (the real
  library writes its own pointers there). So the game allocates a *proper* MPEG buffer too, later: the
  early call's pool base is what is wrong.

## Comparison with the console at the same screens

* The ELF word and both console images agree on the instruction: the console **never** writes there.
* Areas I could compare are equal or explained: the CRI pool descriptors 0x4D69C0-0x4D6B00 (0 differing
  words), the 640x224 video-mode descriptors 0x3C9F00-0x3CA300 (0 differing words), the Sofdec parameter
  block 0x55A900-0x55AA00 (4 words differ, all counter-like: 0x1423/0x1446, 0x8008/0x800e, 0xa0023/0xa0046,
  0x46/0x8c).
* I did **not** locate the console's own MPEG structure (the patterns the stub writes do not exist there,
  because the real library fills them differently), so the console's *buffer address* for the early call is
  not read out yet; that is what would say what the pool base should be.

## Cause and verdict

**Not a defect of the replaced handler** (it writes where the game's own arguments say), and **not shown to
be a recompiler defect**: the wrong value is the game's pool base field `*(iVar1 + 0x14)`, and who sets it
to 0x171778 is **not determined**. Nothing was patched: a patch of the allowed kinds would need a
demonstrated handler defect (none) or a recompiler defect (not shown).

Further evidence (the same night):

* With a breakpoint on `ps2_stubs::sceMpegCreate` (the first call of a run under gdb,
  `work/orchestrator/t114/mpegarg.gdb`) the *same call site* had **good** arguments:
  `a0 (mpeg struct) = 0x0140EF58, a1 (buffer) = 0x00FCD000, a2 (size) = 0x00154000, a3 = 0x00FCD000,
  ra = 0x0035D118` (inside `FUN_0035d010`, as the decompile shows). So the buffer is **not always**
  0x171778: the value depends on the run (the watch run under gdb, which is slower, produced the bogus
  one). That is why the *dumps* of the three plain route runs hold 0x0140EF58 in the code word while their
  MPEG structure is the good one.
* With `iVar1 = a0 - 0x1150 = 0x0140DE08` (the pool of that call), the pool fields are **identical** in
  the port's title dump, the port's menu dump and the console's title image:
  `+0x0C = 0x280 (640), +0x10 = 0x1E0 (480), +0x14 = 0, +0x28 = 0x00FCD000 (the buffer), +0x2C = 0x01121040`,
  and the MPEG structure's `+0x40 = 0x00FCD000`. So in the steady state the game's pool points at a normal
  heap buffer, on the console and in the port alike, and the code word of the *bogus* run would have come
  from a **different** value of `*(iVar1 + 0x28)`: the field that holds the buffer - i.e. some allocation
  gave the game a buffer address inside its own code, in the runs whose timing put it there.

What would settle it: break on *every* `ps2_stubs::sceMpegCreate` call and print its arguments, to catch
the *bogus* call and its `a0` (then `iVar1 = a0 - 0x1150` is the pool of that run), and watch
`*(iVar1 + 0x28)` - the field that holds the buffer - for its writer: that writer is the allocation that
returned an address inside the game's code. The pool and its base come from the game's own pool function
(`FUN_001386d0(0x4d69d0, …)`, docs/knowledge/19 §1); the pool's `+0x14` is 0 in the steady state, so the
`*(iVar1+0x28) == 0` branch of `FUN_0035d010` is not the one that runs.

## Verification

* The bisection dumps and their values: the table above (`work/ramdump/`, `work/orchestrator/t114/`).
* The watchpoint stop with its backtrace and the guest arguments: `work/orchestrator/t114/watch-args.gdb`,
  log `/tmp/opencode/t114-watch5.log` (the evidence copy is in `work/orchestrator/t114/`).
* The three route runs that reproduce the state exactly are in journal 008.
