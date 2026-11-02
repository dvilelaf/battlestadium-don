# 015 · The crash with sound: the replaced MPEG stubs were overwriting the game's own code

- **Date:** 2026-10-13
- **Task:** T-304 (the crash and the stop with sound)
- **Status:** resolved (the crash); the stop at the menu is journal 010's follow-up and journal 011

> **Correction (same day, journal 018).** The overwrite this entry is about is real, is what patch 0036
> fixes and is verified - but it is **not** what crashed the runs. The crash at 0x66F80 is
> `tools/pipeline/build.sh` rewriting a mapped `libSDL3.so.0` in place during a run that was taking
> screenshots. The sentences below that attribute the crash to this overwrite are wrong; the rest of the
> entry (the addresses, the handlers, the test, the fix) stands.

## Symptom

With the configuration that is to be the port's default (`DON_VU1=microvu`, `--sound on`, patch 0034 in the
runtime) the port crashes in about one run in three: SIGSEGV, a call or a return through a pointer that
holds a guest-looking value, `rip = 0x66F80` — an address that is inside the guest's own range and not
mapped as host code (journal 011, "the crash of run 1"). The earlier one, before the VU1 table fix
(journal 012), was seen near vertical blank 1740, which is where the opening video is set up.

## Investigation

**1. AddressSanitizer, attempted and blocked by the image.** `CMakePresets.json` gained a `linux-asan`
preset and the root `CMakeLists.txt` a `DON_SANITIZE` option (empty by default; `address` adds
`-fsanitize=address -fno-omit-frame-pointer` to the compile flags and the link of everything this project
builds). `DON_JOBS=6 bash tools/pipeline/build.sh linux-asan` compiled all 1,965 units (peak **6.1 GiB**,
no compiler killed) and then **failed at the link**:

```
ld.lld: error: cannot open /usr/lib/llvm-17/lib/clang/17/lib/linux/libclang_rt.asan_static-x86_64.a
ld.lld: error: cannot open .../libclang_rt.asan-x86_64.a
ld.lld: error: cannot open .../libclang_rt.asan_cxx-x86_64.a
```

The build image has **no clang compiler-rt** and **no GCC** (`cc` is clang 17; only GCC's
`libasan.so.6/.8` are installed, which clang-instrumented code cannot use), so no sanitizer build can be
linked in this container. The preset and the option are kept for the day the image has the runtime; the
runs of Part 1 were not possible. (Part 1.4's fallback — the core of a crashing run — had already been
taken in the previous session: `rip = 0x66F80`, the registers, the stack with `frame_00`/`3300.png`,
saved in `work/orchestrator/t304/core-*.gdb.txt`.)

**2. What the console's memory says.** A run of the port with microVU and sound was stopped at the menu
(its dump: `work/orchestrator/t304/match3-ram.bin`, saved by `DON_DUMP_RAM`) and compared with the
console's memory at the same screen (`work/ref/x/02/ee_ram.bin`):

```
$ uv run --no-project python -I tools/verify/ram_diff.py work/orchestrator/t304/match3-ram.bin \
      work/ref/x/02/ee_ram.bin --kind code --csv config/functions.ghidra.csv
range 0x100000:0x3902d4, kind code: 9 differing word(s) of 671925
0x00171800  ours=0x00171a50  ref=0x10800058
...  (nine words, 0x00171800 to 0x00171820, all one group: 0x00171800:0x00171824)
```

Nine words of the whole code range differ, all in one run of consecutive addresses, and ours are pointers
into the game's code where the console has instructions. **Journal 009's window, and exactly the class of
journals 006 and 009.**

**3. Which handler writes there.** A scan of the runtime's stubs for writes to literal addresses in the
game's range found three, all in `MPEG.cpp`:

```
MPEG.cpp:2049:  mpegGuestWrite32(rdram, 0x1717BCu, param_1);              // sceMpegCreate
MPEG.cpp:2055:  mpegGuestWrite32(rdram, 0x171800u + i * 4u, ...);        // sceMpegCreate, 9 words
MPEG.cpp:2535:  mpegGuestWrite32(rdram, 0x171904u, 0u);                  // sceMpegReset
```

and `sceMpegClearRefBuff` read *the same nine words* as pointers and wrote `0` at `value + 0x28`.

**4. They are inside a game function.** `bash tools/re/decompile.sh 0x1717BC`:

```
=== BEGIN 0x001717bc FUN_00171710 ===
// function FUN_00171710 00171710..0017197b
void FUN_00171710(int param_1) { ... float math, pointer dereferences ... }
```

0x1717BC, 0x171800-0x171823 and 0x171904 are all inside `FUN_00171710` (0x171710-0x17197B), the game's own
function, and the console's memory holds its instructions there. Ten instructions of the game's code were
being replaced by one pointer and nine pointer values (and `sceMpegClearRefBuff` could zero code at
`instruction + 0x28`), so the next call into that function executes garbage and jumps through it. (The
crash that was being chased is **not** this one: it is journal 018. This defect is a wild jump waiting for
the game to call that function, and it is fixed and verified in its own right.)

## Cause

The replaced `sceMpegCreate`/`sceMpegReset`/`sceMpegClearRefBuff` wrote to absolute guest addresses that
belong to the game's own code. They are not addresses the game passes (this **corrects journal 009**: the
0x1717BC write is not "the buffer the game itself passed", it is a literal in the stub), and no correct
program writes there: the SDK's reference values were addresses its *own code called*, not data to store.

## Fix

`patches/ps2recomp/0036-mpeg-stubs-do-not-write-the-games-code.patch` (applied to the series after 0035):
the two writes in `sceMpegCreate`, the one in `sceMpegReset` and the pointer chase in
`sceMpegClearRefBuff` are gone; every other effect of the stubs (the structure fields they fill, the
return values) is kept. The three sites are left in the source as commented-out lines with the evidence,
so the next reader does not re-add them.

## Verification

`tests/runtime/mpeg_stub_code_test.cpp` (a real `PS2Runtime`; sentinels in the ten words; the three stubs
called): **fails with 31 checks before the patch** ("0x001717BC (inside FUN_00171710) is 0x001F0000, not
0x0BAD0001") and **passes with it**. The runtime's tests against the rebuilt tree:
`1 failed, 15 passed` — the failure is `mmi_lanes_test`, agent B's own test in progress, expected to fail
and not this work. The regression the port's pictures must keep passing, and the runs of the crash's
configuration, are in the report of T-304.

## Temporary diagnostics

None added. `build/linux-asan/` is kept (it cannot link without the container's compiler-rt, see above);
the preset and `DON_SANITIZE` stay for the day the image has it.
