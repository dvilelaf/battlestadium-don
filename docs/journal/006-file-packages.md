# 006 · The runtime gave the game a smaller heap than the console, and kept its own memory inside that heap; fixed by patch 0032 (resolved in iteration 10, 2026-10-12)

- **Date:** 2026-10-12
- **Task:** T-016
- **Status:** open (iteration 8 found the first point of departure with the console's own memory
  images: the runtime's heap limit, item 30, corrected in item 33. Iteration 9 was to implement
  patch 0032; its step 1-3 survey and measurements are items 34-36 and its step 3's answer
  ("something does reach the runtime's allocator") is the design's stop condition, so **no patch was
  written**: two allocators over one range is the orchestrator's decision.)

## Review (orchestrator, 2026-10-12)
The "Cause" and the "Options" below do not hold as written, and are kept only as the record of
this iteration. **Measured**: the disc has no `CMN` directory (`7z l` of the image lists
`SYSTEM.CNF`, `DATA.AFS`, `DUMMY.DAT`, `BGM.AFS`, `SE.AFS`, `DIR.DIR`, `MOVIE.AFS`, the executable
and `MODULES/`), so a search for `\CMN\1000.TXD;1` fails on the console too, whatever answers it
there. The failed search is therefore not where the port departs from the console: either the
game survives it on the console (a loose file tried before its packages), or the console never
gets that far down this path. No component for "package names" is missing from the port by this
evidence; the packages are read by the game's own code. The cause is **not determined**; the next
iteration looks for the first point where the run departs from what the game's code expects
(what was read from `DATA.AFS` before the crash, what the caller does when the search fails, and
where the bad pointer of item 6 comes from).

## Symptom
With patches 0030 and 0031 (`bash tools/pipeline/build.sh linux-dev`, then
`bash tools/run/smoke.sh 60`) the boot goes much further than journal 004 and 005 and then stops:

```
RESULT: MISSING_BRANCH_TARGET 0x0
frames=0 vsyncs=3540 lines=613
  modules named in the log: none          (14 loads: `module loaded by the game (x14)`)
  files named in the log: 0000.fpk, 1000.fpk, dir.dir, movie.afs
  sceCdSearchFile ok x11
  1 sceCdSearchFile failed: \\CMN\1000.TXD;1 (root: .../disc, repeat=1)
  1 sceCdSearchFile failed: \\CMN\1000.DFF;1 (root: .../disc, repeat=1)
[guest-branch:missing-target] kind=IndirectCall op=JALR source=0x2bf858 target=0x0 pc=0x0
```

After the two failed searches the game's main thread ends (it is `dormant`, `pc=0`, `ra=0x002BF860`
in the thread table of `DON_TRACE_THREADS=1`), the scheduler keeps delivering vertical blanks and no
`[don] frame` line ever appears. Logs: `work/logs/smoke-20261012-190747.log` (with the thread table)
and `work/logs/smoke-20261012-19*.log`.

## Classification
`MISSING_BRANCH_TARGET 0x0` (`tools/run/classify_log.py`). No precedent in the catalog of
`docs/knowledge/08-external-references.md`; the nearest entry is "unimplemented system call", which
this is not.

## Investigation
1. **The game got past the CRI set-up.** The four CRI threads of journal 004/005 are created, run
   and sleep/wake as their work arrives (thread table: t2 `susp` 8, t3/t4 `wait` 16/18, t5 `susp`
   25, main thread at 24 — see journal 005). The ADXF device state machine runs:
   `tools/run/gdb_trace.sh` with a breakpoint on `ps2_stubs::sceCdSearchFile` and the condition
   `$a0 == 0x3dfc48` shows the searches coming from the device path
   `0x31b820 → 0x31b618 → 0x3223f8 → 0x3265a0 → 0x326188 → 0x11a7a8`
   (`work/logs/t016f-c.log`). The poll loop at `0x331C60` of journal 004 returns.
2. **What the game asks for, and what is missing.** After `DIR.DIR` (`data.afs`, `bgm.afs`,
   `se.afs`, `movie.afs` — the file is a `\r\n`-separated list of archive names, read as text) and
   the four archives, the game asks the CRI CD layer for `\CMN\1000.TXD;1` and `\CMN\1000.DFF;1`.
   The runtime lowercases the path and strips the `;1` suffix, so it looks for `disc/cmn/1000.txd`:
   there is no `cmn/` (nor `fpack/`) in `disc/`, so both fail. This is the last file activity in the
   log.
3. **Where those names come from (the game's own data).** The executable holds the index of its
   file packages: at guest `0x4BA1B8` (equivalent file offset `0x3BB1B8` of `SLPS_256.75`) there is
   a name table with `fpack/cmn/1000.fpk`, `cmn/camera`, `cmn/0000`, `font/1000`, `game/0000` …
   `game/0005`, `cmn/1000.txd`, `cmn/1000.dff`, `cmn/1000`, and a parallel table of numbers
   (`0x1d770c`, `0x1d78f0`, …) before them. So `fpack/cmn/1000.fpk` is the *container* and
   `cmn/1000.txd`, `cmn/1000.dff` are its *members*. `DATA.AFS` (53,217,280 bytes) is an AFS
   archive with **135 unnamed entries** (offset/size only, no name table: the tail of the file is
   zeroes) and its members *are* the containers: member #107 (offset 37,408,768, size 647,184)
   starts with a header (`0`, `0x0b`, `0x10`, `0x9e010`) followed by entry names
   `cmn/0000_ps2.txd`, … — the **`_ps2` platform variants** of the logical names. Around offsets
   37.2-37.6 MB of `DATA.AFS` there are also names `fpack/game/00…`, `fpack/menu/0…`,
   `fpack/option/0…`, `fpack/viewer/0…` (a container name list). `DIR.DIR` and this layout were read
   with two throwaway scripts (ISO9660 + AFS inspection, `/tmp/opencode`, not kept).
4. **What the port can do with such a name.** Nothing: `ps2_stubs::sceCdSearchFile` resolves names
   against the host root only (`disc/`, the log prints `root: .../disc`; paths lowercased, `;1`
   stripped), and the game's `0x11A7A8` is bound to it by journal 002. There is no component in the
   port that knows about the RenderWare/CRI file packages.
5. **Who serves those names on the console — not proven.** The name goes to the CRI CD layer, whose
   search primitive ends in `0x11A7A8` (on the console: an RPC to the IOP). The game loads
   `cdvdstm` (pack `1000`, id=6) and `cri_adxi` (id=13); the disc also holds `MODULES/RTFSSIOP.IRX`,
   which is the **RenderWare File System** for the IOP (symbols `RtFs*`/`RwFS*`, `rtfsiop_entry`),
   and it is *not* loaded in any run so far. The standard `cdvdman` that answers SID `0x80000597`
   comes from the IOP reset image `IOPRP300.IMG`, which the runtime replaces with a no-op
   (`Stubs/SIF.cpp:560`, journal 002). Which of these resolves `cmn/1000.txd` on a real console is
   **not determined** here, and it decides which fix is the right one.
6. **The JALR to 0 is the game's own error path, not a missing function.** `0x2bf828` walks a
   callback list (`*(param_1 + 0x10)`, calling each record's `+0x20` callback) and was called with
   `param_1 = 0x3c99a8`; the JALR at `0x2bf858` used a record whose `+0x20` was 0. A gdb hardware
   watchpoint on `0x3c99b8` (the list head field, set after `ps2_syscalls::InitThread` with
   `set $ram = $rdi`, `/tmp/opencode/watch.gdb`, not kept) shows exactly three writes to it:
   (1) `0xe6c`, from `0x2bf3f0` ← `0x2c6768` ← `0x2910b8` ← `0x136d40` (the engine init of boot map
   step 4) ← `main`: the legitimate registration; (2) `0` from the object constructor `0x2c5b38`
   (← `0x2c64e8` ← `0x2c1de8`); (3) `0xc17f32af`, a float (-15.95), from `0x2c1de8` itself, which is
   the main thread's code. The game then writes *float data* into the manager object it had
   registered, and the next call of the callback dispatcher follows the corrupted list. The shape is
   a type confusion after a failed lookup, i.e. a consequence of the missing file, not a runtime
   fault: the runtime reports the null indirect target and ends the thread, as the console would let
   the game crash.
7. **Ruled out.** The CRI threads and the ADXF device step (they work now, item 1). `sceCdRead` and
   SID `0x90000200` (journal 004). The CD layer itself: the 11 successful searches in the run are
   served correctly from `disc/`. The vertical blanks (60 per second).
8. (2026-10-12, iteration 6, Q1) **Who asks for `cmn/1000.txd`, and what the search costs.** Measured
   with breakpoints on the game's own file layer (all guest addresses): the boot's first loop
   `0x132470` (and, later, `0x131D2C` inside the frame loop `0x131CA0`) calls `FUN_001fd500`
   (`0x1FD500`) → `FUN_001fdbe0` (`0x1FDBE0`) → `FUN_00150a60` (`0x150A60`, "open the `.txd` of this
   base name") → `FUN_0014a880` (`0x14A880`, **the game's file layer open**, with the `_ps2` format
   logic) → on failure `FUN_001336d0` (`0x1336D0`, "load this file") → which ends in the CRI CD layer
   and in the two searches `\CMN\1000.TXD;1` and `\CMN\1000.DFF;1` (callers, in order:
   `FUN_0014a880` ← `FUN_00150a60` ← `FUN_001fdbe0` ← `FUN_001fd500` ← `0x132470`; the search itself
   comes from the CRI device, `ra=0x3261bc`). **When the load fails, `FUN_0014a880` returns 0,
   `FUN_001fdbe0` skips the registration of that resource and goes on** (`if (lVar2 == 0) lVar2 = 0;`
   in its decompilation): there is no error message and no retry. The function that would make the
   request unnecessary is the *platform-flag check* of item 11: with bit `0x10` set, the file layer
   asks first for `cmn/1000_ps2.txd`, which the pack it already has (item 10) *does* contain, and the
   CD is never asked.
9. (Q2) **What was read from `DATA.AFS` before the crash.** Every `ps2_stubs::sceCdRead` call traced
   with its three arguments: `modules/0000.fpk` (lbn `0x100000`, `0x39` sectors → `0x1EE3730`),
   `modules/1000.fpk` (`0x10003A`, `0x81`), `DIR.DIR` (`0x1000BC`, 2 sectors, from the CRI primitive
   `0x3270C8`), the header of each AFS (`0x1000BE` DATA.AFS, `0x106640` BGM, `0x112F57` MOVIE,
   `0x112248` SE, 1 sector each, from `0x3244E8`), and then two whole members of `DATA.AFS`:
   **90 sectors at `DATA.AFS + 0x2380000` = member #106 = `fpack/cmn/0000.fpk`** (183,904 bytes, the
   24-entry `.seq` pack) and **317 sectors at `DATA.AFS + 0x23AE000` = member #107 =
   `fpack/cmn/1000.fpk`** (647,184 bytes; entries `cmn/0000_ps2.txd`, `font/1000_ps2.txd`,
   `cmn/1000_ps2.txd`, `cmn/1000_ps2.dff`, `cmn/1000.anm`, `game/0000..0005_ps2.txd`). Read sizes are
   the member sizes rounded up to sectors. **No CD streaming at all**: `sceCdStStart`/`sceCdStRead`
   are never called (no `[sceCdStStart]` line). The run **matches** the naming table in
   `docs/knowledge/data/data_afs_names.tsv` (index 106 = `fpack/cmn/0000.fpk`, 107 =
   `fpack/cmn/1000.fpk`) and doc 16 section 3 row 6; note the reads are issued by the CRI/ADXF layer
   (`0x3244E8`), not by the game's own loader of doc 16 row 6 (`0x1345B0`).
10. (Q3) **Are the bytes right.** Member #107's bytes in guest memory (dumped at the first
   `sceCdSync` after the read, `/tmp/opencode/t016g/live107.bin`, kept in
   `work/orchestrator/t016g/`) are **byte-identical** to `disc/DATA.AFS` at offset 37,408,768
   (`cmp` clean, 647,184 bytes). The completion answers are consistent with the runtime's handlers
   (`Stubs/CD.cpp`): `sceCdRead` returns 1, `sceCdSync` returns 0 ("completed"), `sceCdGetError`
   returns 0, and no `[sceCdRead] unresolved request`/`recovered with alternate args` line appears in
   any run. So a read that "completes before the data is there", a wrong sector count or a wrong
   completion is **not** the departure for this pack.
11. (Q4) **The pack in memory and the name variants.** The loaded container *is* in memory (item 10)
   and its *entry names carry the `_ps2` suffix* (`cmn/1000_ps2.txd`), while the loader asks for the
   base name (`cmn/1000.txd`). The file layer builds the name to use by copying the base name,
   cutting it at the first `.` and appending a *format* suffix from a table of 16-byte slots at
   `0x4B6948`: `.dff` (`0x4B6980`, flag bit 1), `.bsp` (`0x4B6968`, bit 2), `.anm` (`0x4B6950`, bit
   8), `.txd` (`0x4B6998`, bit 4), `.mot`, `ptl`, `.seq`, `.rws`, `.tmo`, `.fld`, `.uva`, `.tcg`,
   `.tmd`, `.bone`, `_cmp.anm`, `.par`; the `_ps2` variants are separate strings (`_ps2.bsp`
   0x4B6958, `_ps2.dff` 0x4B6970, `_ps2.txd` 0x4B6988) used in the *retry* branches. **Which variant
   is used at all is decided by the global `DAT_003c0360`**: for the `.txd` flavour,
   `if ((DAT_003c0360 & 0x10) == 0)` the code loads the plain name and **returns failure if it is not
   there — there is no `_ps2` fallback**; with bit `0x10` set it tries `_ps2.txd` first and only then
   the plain name. The `.dff` flavour has both orders (bit 1 of the flag). The flag is read at four
   places inside `FUN_0014a880` (`0x14AF10`, `0x14B1A8`, `0x14B30C`, `0x14B444`), read by the getter
   `FUN_0014a820` (`lw`, 12 bytes), set by `FUN_0014a830` (`sw`, 16 bytes, callers `0x131D18` and
   `0x1B6760`), and its initial value in the ELF is **0x11** (bits 0 and 4 = the PS2 index, and
   `0x23` = bits 0, 1 and 5 in the raw pack bytes, see item 16). The file-table walk of
   `FUN_0014a880` compares only the **last 15 characters** of the name (`strlen`, `+len-15`) against
   a name field of each entry (`FUN_001236b8`); for this run that comparison is never reached for
   `cmn/1000.txd` (its compare function is never called with those arguments), so no string mismatch
   is involved.
12. (Q5) **The bad write.** A hardware watchpoint on the flag (write) reports exactly one writer:
   `Old value = 17`, `New value = 0`, at `FUN_00123280` ← `FUN_002be650` ← `FUN_002ddfb0` ← the
   scheduler. `FUN_00123280` is `memmove`/`memcpy` (32-byte blocks, then 8-byte), `FUN_002be650` is
   the *stream read* (`mode == 3` = "read from a memory buffer": `memcpy(dst, stream.buf +
   stream.off, size)`, then the offset advances), and `FUN_002ddfb0` is the engine code that calls it
   as `FUN_002be650(stream, *(struct + 4), size)`. Measured values at the hit:
   `dst = 0x003BEF80`, `size = 0x2050`, `stream.off = 0xD36C`, `stream.total = 0x14B40`,
   `stream.buf = 0x176E470`; the written range `0x3BEF80..0x3C0FD0` **contains the flag** (at
   `+0x13E0`), which is why it becomes 0.
13. **Who the pointer belongs to.** `0x003BEF80` is **not** referenced by any instruction of the
   executable (scanned the whole text for `lui/addiu` pairs and for base-register accesses to it:
   none) and the value does **not** occur anywhere in `SLPS_256.75`, `DATA.AFS`, `BGM.AFS`, `SE.AFS`
   or `MOVIE.AFS`; the ELF's bytes there are all zero (a zero area with no code reference). The
   pointer comes from the object `iVar8` that `FUN_002ddfb0` receives from `FUN_002c97d8(...)`
   (created from the file's header words read into the stack), i.e. the engine reads through a
   *field of an object it believes is a buffer* which is **not a buffer**: a write through a garbage
   pointer. **This is the first point where the run departs from what the game's code expects.**
14. **Why it matters.** The flag's value decides which file names the game asks for (item 11). With
   the flag cleared the game asks the CD for the *plain* names (`cmn/1000.txd`, `cmn/1000.dff`, item
   8), which exist nowhere on the disc, the searches fail (item 8), `FUN_0014a880` returns 0 with no
   fallback, the loader skips the resource, and the later use of the unregistered resource ends in
   the null indirect call of item 6. With the flag intact the game asks for `cmn/1000_ps2.txd` /
   `.dff`, which the pack it already has in memory contains (item 9/10) and which needs no disc I/O.
15. **Order of the run, measured** with a breakpoint on the file-layer open that prints the flag
   (`work/orchestrator/t016g/plate.log`): `cmn/rotbl`, then `cmn/0000`, `font/1000`, `game/0000`,
   `game/0001`, `game/0002`, `game/0003`, `game/0004` and `game/0005` — **all with flag `0x11`**, and
   the loads for those all resolve to `*_ps2.txd` (successes) — and then the flag is **`0x0`** for
   every later open (`cmn/1000.txd`, `cmn/1000.dff`, …). The one write of 0 (item 12) happens between
   the `game/0005` group and the `cmn/1000` group. The setter `FUN_0014a830` is never called in this
   run (it is called by the boot only at `0x131D18`, inside the frame loop the run never reaches).
16. **What the disc says at that offset.** The pack's raw bytes at the position the game's stream
   covers are high-entropy (`10 fc fc a0 1c fe 84 fc …` in member #106 at `+0x1ADDC`), the game's
   stream holds structured data (`03 00 00 00 00 00 00 10 0e …`), and the byte that lands on the flag
   is `0x00` in the game's stream: the block the engine writes is a *processed/decompressed* buffer
   (`stream.total = 0x14B40`, not the size of any member or pack entry: DATA.AFS has no member of
   that size), so its content cannot be compared byte for byte with the disc. What can be said: the
   block is 0x14B40 bytes of *engine-computed* data and the write puts it over a *zero, unreferenced*
   area of the data segment (item 13) that happens to contain the flag.
17. **Ruled out by measurement** (this iteration): the CD read path and the pack bytes (item 10);
   the completion protocol (`sceCdSync`/`sceCdGetError`, item 10); the absence of the CMN directory
   on the disc (the orchestrator's review); the four CRI threads, the ADXF device and the vblank
   (journal 004/005); a missing entry point or an unbound SDK function (no `MISSING_FUNCTION` line,
   the search itself is a *successful* call that answers "not found"); a name-comparison bug in the
   file table (item 11, that code is not reached).
18. (2026-10-12, iteration 7, R1) **The failing texture read, measured.** At the `FUN_002be650`
   (stream read) whose destination is `0x003BEF80` and size `0x2050`, called directly by
   `FUN_002ddfb0`, the engine's raster object is **`0x003A05D8`** and its fields are
   `+0x00=0x3A05D8 +0x04=0x3BEFD0 +0x08=0x3C1050 +0x0C=0x10 +0x10=0x200 +0x14=4 +0x18=8 +0x1C=0
   +0x20=0x45000004 +0x24=0x3BEEF0 +0x28=0x10 +0x2C=0x200 +0x30=8`. The destination is
   `*(*(raster+0x24)+4)` = `*(0x3BEEF0+4)` = 0x003BEF80 (the *type-2* branch of `FUN_002ddfb0`, the
   third `FUN_002be650` call). **The raster is not a heap object**: it is a *static* address in the
   game's data segment, and the whole block the pool gave it (0x30407 bytes, item 20) spans
   0x3A05D8..0x3D09DF, i.e. it **contains both the destination (at +0x1E9A8) and the platform flag
   0x003C0360 (at +0x1FD88)**.
19. (R2) **The same values for other textures.** Of the pixel reads from `0x2DDFB0` that I could pair
   in one run (362), 347 have destinations in 0x100000..0x5A6800 — the engine's *static* pool area,
   e.g. 0x278D80, 0x279000, 0x279180, 0x279400 — 15 at 0x3C0000 or above, and **none** in the game
   heap (0x5A6800..0x1F00000); the failing one is the only destination I found inside the 0x3A05D8
   block itself. The *headers* of the good reads (words 0..5 of the 0x40-byte header, read from the
   stream) look like sane texture descriptors, e.g. `0x00000001 0x00000040 0x1c020037 0x00000020
   0x00000020 0x00000004`; the failing read's header words (read at the *stream's* offset, which for
   that object is not a texture header — **Unverified** that they are its header at all) were
   `0x000011a0 0xffffffff 0xffffffff 0x0000012c 0x00000008 0x0000040f`, which looks uninitialised.
   **Unverified**: which block belongs to which resource (the readers interleave), and whether the
   good textures use the 0x27xxxx pool blocks while the failing one uses the 0x3A0000 block.
20. (R3) **How the raster and its pixel pointer are made, by reading code and by watchpoints.**
   `FUN_002c97d8` (0x2C97D8, the "create raster") does:
   `lVar2 = (*table[0x118])(*(gp-0x75F0)+0x5406A0, 0x30407)` (the *raster block*), fills
   `+0xC = width, +0x10 = height, +0x14 = depth, *obj = obj, +4 = 0, +8 = 0`, then
   `lVar3 = (*table[0x58])(0, raster, format)` (the *surface*, which sets the pixel pointer), and on
   failure returns the block with `(*table[0x11C])(...)` (a free). The table base is `0x540640`
   (`lui 0x54; addiu s2, 0x640`), the size argument is hardcoded (`lui 0x3; ori 0x407` → 0x30407).
   **Measured with write watchpoints** on the two table slots: `0x540758` (table+0x118) is set at
   boot to **`0x002BCBB8`** (`FUN_002bcbb8`) by `FUN_002c54c8` ← `FUN_00136d40` (the engine init of
   boot map step 4) ← `main`; `0x540698` (table+0x58) is set to **`0x002E2030`** (`entry_002e2030`,
   the 8-byte entry at the end of `FUN_002e2028`) by `FUN_002e0430` ← `FUN_002c5378`.
   `FUN_002bcbb8` is a **bitmap allocator** over a pool descriptor: it walks a list head at
   `param_1[4]`, uses `param_1[1]` as a bit count per word and `param_1[2]` as a word count, tests
   bytes for `0xFF` and scans bits (`0x80 >> k`) — a free-bitmap pool, not a heap. The pool
   descriptor comes from the *bss* (`*(gp-0x75F0)+0x5406A0`, and 0x5406A0 is inside
   0x4C9D80..0x5A6800).
21. (R4) **Nothing on that path is a runtime handler.** All the functions of the chain are full game
   recompilations — `generated/FUN_002c97d8_0x2c97d8.cpp`, `generated/FUN_002bcbb8_0x2bcbb8.cpp`,
   `generated/entry_002e2030_0x2e2030.cpp`, `generated/FUN_002ddfb0_0x2ddfb0.cpp`,
   `generated/FUN_002be650_0x2be650.cpp`, `generated/FUN_00123280_0x123280.cpp` — not the few-line
   wrappers that a replaced function has (a `ps2_stubs::NAME`/`ps2_syscalls::NAME` call), and
   `config/stub_policy.toml` has no entry that would match them (they carry no SDK name; the map rows
   are `FUN_*`/`entry_*`). So the raster allocator is *not* `malloc`/`memalign`/`calloc` and the pool
   is *not* the runtime's guest heap; the runtime's CD layer is not in this path either (item 10).
22. (R5) **Is the pool exhausted?** The pool's memory is *static* game data (the rasters and the
   other texture buffers are all in 0x100000..0x5A6800, *below* the guest heap 0x5A6800..0x1F00000),
   so the *known* port difference of the guest heap (docs/knowledge/16-boot-sequence.md section 9
   item 1: the game asks for about 515 KB more than it gets, after about 2,000 retries) is **not on
   this path**. **Not measured**: the allocator's free-bit accounting at the failing call
   (`FUN_002bcbb8` walks a bitmap; its second argument `0x30407` behaves like a *bit pattern* and not
   like a size — **Unverified**). The reference images (orchestrator: work/ref/x/01..06/ee_ram.bin)
   show the flag at 0x3C0360 = 0x11 in all six and a *different* content at 0x3BEF80..0x3C0FD0, so on
   the console no allocation of this shape reaches the flag area.
23. (R6) **Not done to the instruction level.** No mis-translated instruction was identified. The
   remaining arithmetic candidates are inside the bitmap allocator (`FUN_002bcbb8`, which I read only
   in part) and in the pool base/bitmap bookkeeping; nothing in the *pointer* arithmetic of
   `FUN_002ddfb0` (plain `lw`s and one `addiu -0x50`) looks wrong. **Unverified.**
24. **Ordering, and what the next measurement must settle.** The failing *texture read* (item 18) and
   the *failed open* of `cmn/1000.txd` (item 8/15) concern the same resource. Measured: the platform
   flag is 0x11 until the `game/0005` group and 0x0 from `cmn/1000.txd` on (item 15); the one write of
   0 to it that I caught (item 12) is this stream read into 0x3BEF80. **Unverified**: whether that
   read belongs to the *cmn/1000* texture and whether it happened before or after that resource's
   failed open. That decides whether the cleared flag *caused* the failed load (iteration 6's
   reading) or whether a *failed load* left a garbage header whose *wrong allocation* covered the
   flag (this iteration's item 19 observation). The next iteration's Options 1 and 2 settle it.
25. (2026-10-12, iteration 8, step 1) **A reusable way to compare this run's memory with the
   console's.** `src/host/host.cpp` now writes the 32 MiB of guest RAM when `DON_DUMP_RAM=<file>` is
   set, once the game thread has stopped, with one `[don]` line saying what was written (the dump is
   game memory: under `work/`, never in git). Measured:
   `DON_DUMP_RAM=work/ramdump/run.bin bash tools/run/smoke.sh 30` →
   `[don] RAM dump work/ramdump/run.bin: 33554432 of 33554432 bytes`. New tool
   `tools/verify/ram_diff.py` (word-by-word comparison, `--range`, `--kind code|all`, function-map
   names, groups of consecutive differences) and `tests/test_ram_diff.py` (1 test, passes).
26. (step 2) **The engine's device table matches the console exactly.** Comparing
   `work/ramdump/run.bin` with `work/ref/x/01/ee_ram.bin` over 0x391000..0x5A6800 with `--kind code`
   gives 3693 differing words, but **none of them is a slot of the table at 0x540640**: every slot
   the orchestrator listed (+0x14 = 0x2E0430, +0x20, +0x24, … +0x13C) is **equal on both sides**,
   including **+0x58 = 0x2D9F40** (the real graphics "create" function) and +0x118 = 0x2BCBB8 (the
   bitmap allocator). **So the hypothesis of the iteration's card — "the engine installs default
   functions and the real graphics device did not run" — is refuted by measurement**: in this run
   0x2E2030 (the default) *is* written to +0x58 first and 0x2D9F40 *later*; the iteration-7 watchpoint
   had simply stopped at the first write. `work/orchestrator/t016i/diff01.txt`.
27. (step 2, the real differences) **The engine's pools are in the wrong memory.** The pool
   descriptors have identical configuration fields but different memory pointers (ours vs the
   console): 0x5446E0: +0x00..+0x0C = 0x60, 0x80, 0x10, 4 on both sides, but +0x10 = **0x001F9980**
   vs **0x0082A8C0** and +0x14 = **0x003A5C60** vs **0x00D29C00**; 0x544708: +0x00..+0x0C = 0x18, 5,
   1, 4 on both sides, but +0x10 = **0x00212E00** vs **0x00DA9D20** and +0x14 = **0x001FC9B0** vs
   **0x007A31F0**. **On the console both texture pools live in the guest heap (0x7A…-0xDA…); in this
   run they are at 0x1F…-0x3A…, i.e. the tail of RAM and the game's data/code.** That is why the pixel
   pointers are wrong for *all* textures (item 19) and why the copies clobber the code area first and
   then the platform flag at 0x3C0360 (items 12, 18).
28. (step 3) **Who allocates the pool memory, measured.** The engine init `0x136d40`/`0x1364a0` runs
   the device init `FUN_002c51e0` (0x2C51E0), which calls the device entry +0x14 (= 0x2E0430, mode 2)
   and then the callback dispatcher `FUN_002bf828(0x3C9890, 0x540640)`; the callback `FUN_002cc798`
   (0x2CC798) creates the two pools with
   `FUN_002bc848(DAT_003c99f0, DAT_004c98b8, 4, DAT_004c98bc, 0x5446E0, 0x40006)` and
   `(DAT_003c9a08, DAT_004c98c0, 4, DAT_004c98c4, 0x544708, 0x40408)`. `FUN_002bc848` (0x2BC848, the
   pool creator) gets the pool *memory* through the table slot **+0x108 = 0x136CF0** (the same value
   in both images), which is `entry_00136cf0`: a thin **recompiled** wrapper
   (`a1 = a0; a0 = 0x4D69D0; goto FUN_001386d0`) — i.e. **the game's own allocator over the arena at
   0x4D69B0/0x4D69D0**, not a runtime handler. Measured order (`work/orchestrator/t016i/order.log`,
   a gdb trace of the three functions): 1. device init (config 0x3C99F0 = 0x60, 0x3C9A08 = 0x18 —
   *correct*), 2. device init again, 3. pool creation (config still correct), 4. device init, then a
   long loop of object constructors (one of which zeroes those two config words *later*).
29. (step 3/4) **The arena is where the console and this run part.** The arena descriptor that the
   game's allocator uses (0x4D69B0 / 0x4D69D0) differs (ours vs the console): 0x4D69B8 = **0x1ED2F20**
   vs **0x1F50C20** (delta 0x7DA00 = 513.5 KiB), 0x4D69C4 = **0x9596F0** vs **0x9D73F0** (delta 0x7DD00
   = 514.0 KiB), 0x4D69B0/0x4D69D0/0x4D69D8 also differ, and — the smoking gun — the boot's
   heap-sizing **retry counter 0x4D6A08 is 0x6FF = 1791 here and 1 on the console**, which matches
   "about 2,000 retries" of docs/knowledge/16-boot-sequence.md section 9 item 1.
30. (step 4, the cause) **The heap limit of the runtime is not the console's.** The boot's heap sizing
   (`FUN_001365c0`, decompiled) asks for base 0x5A6800 + size 0x19D73F0, i.e. a heap ending at
   **0x1F7DBFF** (`lui 0x1F7; ori 0xDBFF`), and while `FUN_001270c0(0x40, size)` fails it **shrinks the
   request by 0x100 and retries** (`param_1 += -0x100`), then splits the result into two arenas
   (`FUN_00138810(0x4d69d0, base, 0x1000000)` and `(0x4d69b0, base + 0x1000000, size - 0x1000000)`).
   The runtime answers that size through `SetupHeap`/`EndOfHeap`
   (`ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp`): `kDefaultGuestHeapEnd = 0x01F00000` and
   `heapLimit = min(candidate, kDefaultGuestHeapEnd)` (:546-557); `EndOfHeap` returns
   `runtime->guestHeapLimit()` (:586-590); `ps2_runtime.cpp:94` has `kGuestHeapHardLimit = 0x01F00000`.
   The console's heap end is ~**0x1F80000** (32 MiB minus the 0x80000 stack the game reserved with
   `SetupThread`), i.e. ~512 KiB more, and the measured arena delta is 0x7DA00 (513.5 KiB) — the
   difference being the 0x100 retry granularity. **So in this run the game's whole heap is ~515 KiB
   smaller and shifted, its arenas are smaller, the engine's texture pools (item 27/28) are allocated
   from that smaller arena and land in the wrong memory, every texture pixel pointer is wrong, and the
   copies overwrite the code area and then the platform flag** — the whole chain of items 12-19 is a
   *consequence* of the heap limit. docs/knowledge/16-boot-sequence.md section 9 item 1 had already
   proposed the fix and measured the same 515 KB; this iteration shows that it is not a cosmetic
   difference but the *first* point of departure of the boot.
31. **What is NOT the cause** (measured): the engine's device table and its start-up (item 26 — every
   slot matches the console, including the real create function); a runtime handler on the pool path
   (item 28: the allocator is the game's own, recompiled); the CD read path, the pack bytes and the
   completion protocol (iteration 7, items 9-10); the CRI threads, the ADXF device and the vblank
   (journals 004/005); a missing entry point or an unbound SDK function (no `MISSING_FUNCTION` line,
   iteration 6 item 17).
32. **The fix, not applied (this iteration's stop list names it explicitly: "a heap limit that is not
   the console's").** Make `SetupHeap`/`EndOfHeap` answer the console's limit: when the game passes
   size `-1`/`0xFFFFFFFF`, the heap end should be `PS2_RAM_SIZE - <the stack size the game reserved with
   SetupThread 0x3C>` (= ~0x1F80000), keeping the 0x01F00000 clamp only for the runtime's own internal
   allocations — the "Proposed fix" already written in docs/knowledge/16-boot-sequence.md section 9
   item 1. A test can be written without the game: a guest that calls `SetupThread` with a known stack
   size, then `SetupHeap(base, -1)` and `EndOfHeap`, and checks the reported end is `RAM - stack`
   rather than 0x01F00000 (that is a runtime behaviour change, so the orchestrator decides it).
33. **Correction (2026-10-12, orchestrator).** Item 29's reading of `0x4D6A08` is wrong: that word is
   the **vertical-blank counter** that the game's vblank handler increments and the frame loop reads
   and clears (docs/knowledge/16-boot-sequence.md section 8), not the heap-retry counter — 1 in the
   console's image and 1791 here only says the frame loop never ran here. The *arena* words are the
   evidence and they are stronger: `0x4D69C4` is **0x9D73F0** on the console, which is exactly the
   game's first request `0x19D73F0` minus `0x1000000`, i.e. **the console's first attempt
   succeeded**; here it is **0x9596F0**, 0x7DD00 (≈515 KiB) smaller, i.e. the runtime's clamp forced
   the boot to shrink its request. `0x4D69B8` (0x1F50C20 vs 0x1ED2F20, delta 0x7DA00) says the same
   thing from the other end.
34. (2026-10-12, iteration 9, step 1) **Every use of the four pools and of the 0x01F0xxxx literals**
   (survey, `work/orchestrator/t016j/README.md`): `State.h:246/249/254/259` define
   `kRpcPacketPoolBase 0x01F00000` (0x10000 bytes, 64-byte packets), `kRpcServerPoolBase 0x01F10000`
   (0x10000, 0x80-byte stride), `kTlsPoolBase 0x01F20000` (0x10000, 0x100-byte blocks) and
   `kBootModePoolBase 0x01F30000` (0x1000); their only users are `Helpers/Runtime.h`
   (`rpcAllocPacketAddr:92`, `rpcAllocServerAddr:103`, `allocTlsAddr:335`, `allocBootModeAddr:349`),
   called from `ps2_iop_host.cpp:233/234`, `Kernel/Syscalls/RPC.cpp:304` (`SifRegisterRpc`),
   `System.cpp:945` (`GetThreadTLS`) and the BootMode helpers; the `0x1F10100` client pointer of
   iteration 3 is `kRpcServerPoolBase + 2 * 0x80` ✓. The literal `0x01F00000` also appears in
   `System.cpp:546/586` (the SetupHeap clamp and the EndOfHeap default), `ps2_runtime.cpp:94`
   (`kGuestHeapHardLimit`, used at :506, :509, :1008, :1017, :1668, :1674) and `ps2_runtime.h:570`
   (`m_asyncCallbackStackFloor` = 0x01F00000, top = PS2_RAM_SIZE): that last one is the **guest
   invocation stack** allocator (`PS2Runtime::reserveAsyncCallbackStack`, ps2_runtime.cpp:2021),
   whose only caller is `EeScheduler::invocationStackTop` (EeScheduler.cpp:1173-1174,
   `kInvocationStackSize = 0x4000`) for queued invocations.
35. (iteration 9, step 2) **What the game does with `SetupHeap`'s return**: the ELF entry
   (`0x1001E8`, decompiled) calls `syscall(0x3C)` (`SetupThread`) and `syscall(0x3D)` (`SetupHeap`)
   and **ignores both return values** (nothing is stored from them; the next statements are
   `FlushCache`, `EI` and the call to `main`). So today's return value (`guestHeapBase`) can stay;
   what the console changes is the *end* that `EndOfHeap` reports.
36. (iteration 9, step 3 — **the reason this iteration stopped**) **The runtime's allocator over the
   guest heap is live in this game, and it works from the same base as the game's own allocator.**
   Measured with breakpoints during a 30 s run (`work/orchestrator/t016j/allocators.log`):
   * `PS2Runtime::guestMalloc` is reached, from **`ps2_stubs::sceGsResetGraph`** during `FUN_00136770`
     (the other callers in the tree are the other GS handlers, the font handlers, and the
     LibC/Compatibility stubs); `allocateGuestBlockLocked` (ps2_runtime.cpp:1741) hands out the base
     of the first free block of `[guestHeapBase, guestHeapLimit)`, and `SetupHeap` configured that
     base as the address the game passed, **0x5A6800**. The game's own recompiled C library
     (`FUN_001270c0`, 144 lines of real code, no handler) manages the same range, and its first arena
     starts at **0x5A6840** (field `0x4D69D0`, the same in both memory images). So the runtime's first
     block `[0x5A6800, +size)` and the game's own heap header overlap **today**, before any change.
   * `PS2Runtime::reserveAsyncCallbackStack` is reached too (from `EeScheduler::invocationStackTop`),
     and its region `[m_asyncCallbackStackFloor, PS2_RAM_SIZE)` = `[0x1F00000, 0x2000000)` is exactly
     the area the console's heap top (0x1F00000-0x1F80000) and the game's stack (0x1F80000-0x2000000)
     occupy.
   The design says: "If something does reach it, STOP and report: two allocators over one range is a
   design problem the orchestrator must decide." That is what this iteration does; no patch was
   written, because the *design problem is not new but the range would become bigger*: with the
   console's limit the runtime's blocks, its invocation stacks and the game's own allocator would all
   share `[0x5A6800, 0x1F80000)`.
37. (iteration 9, the tests that were *not* written) The `tests/runtime/guest_heap_limit_test.cpp` of
   the design (EndOfHeap = 0x01F80000 after `SetupThread(gp, -1, 0x80000)` + `SetupHeap(0x5A6800, -1)`,
   `start + size` with an explicit size, the server structure at or above 0x000A0000 and below
   0x00100000, the pools not overlapping) is the acceptance test of patch 0032; it would **fail**
   against the unchanged runtime, and a failing executable breaks `tests/runtime` for everybody, so it
   is written down here as the specification and will be added with the patch once the orchestrator
   decides how the runtime's own allocations and the game's heap coexist.
38. (2026-10-12, iteration 10) **Decision (orchestrator): option 1**, as **one patch (0032)**, because
   "the runtime's own guest memory" is one topic: everything the runtime keeps in guest RAM gets a
   home of its own in the console's kernel area, outside the game's image, heap and stacks, and the
   game then gets exactly the console's heap. The table is fixed by the orchestrator (see item 39);
   the rules that came with it (one place for the layout with `static_assert`s, `SetupHeap`/`EndOfHeap`
   the console's kernel, loud exhaustion, the review of every `guestHeap*` use, the measurement of the
   invocation stacks and of the allocator's requests, the survey of the other fixed-address writes)
   are the skeleton of items 39-45.
39. (iteration 10) **The layout lives in one place**, `ps2xRuntime/include/runtime/ps2_guest_memory.h`
   (a new header; `Kernel/Syscalls/Helpers/State.h` includes it, so the four pool constants keep their
   old names and users): `0x00080000-0x000BFFFF` invocation stacks (16 of `0x4000`, the bump allocator
   of `reserveAsyncCallbackStack` starts at `0x000C0000` and goes down, never frees),
   `0x000C0000-0x000CFFFF` remote-call packets, `0x000D0000-0x000DFFFF` remote-call servers,
   `0x000E0000-0x000EFFFF` thread-local storage, `0x000F0000-0x000F0FFF` boot mode and
   `0x000F1000-0x000FFFFF` the runtime's own allocator (`guestMalloc` and friends). Six `static_assert`s
   say that no region overlaps another, that the stack area holds 16, and that every region ends at or
   below `0x00100000`. `EeScheduler.cpp` uses the shared `kInvocationStackSize` (it had its own
   `0x4000`); it must include `runtime/ps2_guest_memory.h` and **not** `Syscalls/Helpers/State.h`,
   whose `KE_*` constants clash with the file's own (first compile error of the patch).
40. (iteration 10, the design's rule d) **Every use of the heap accessors after the change**:
   `ps2_runtime.cpp:506/509` (constructor: `m_guestHeapLimit`, `m_asyncCallbackStackFloor`) and
   `:1008/1017` (the `loadELF` block that used to *suggest* a heap base from the loaded image and to
   raise the stack floor to it) are now the private range and the stack area from the table — the
   `loadELF` block is gone and the numbers are the constants; `:1668/1674`
   (`clampGuestHeapBase`/`clampGuestHeapLimit`) clamp **into** the private range, so any caller of
   `configureGuestHeap` (now only the runtime's own reset path) can never hand the runtime the game's
   heap; `guestHeapBase()/guestHeapEnd()/guestHeapLimit()` are the private allocator's state and are
   no longer used by `SetupHeap`/`EndOfHeap` (no test used them). `kGuestHeapHardLimit` and
   `kGuestHeapDefaultBase` are gone, replaced by `kRuntimeAllocatorBase`/`kRuntimeAllocatorEnd`;
   `kGuestHeapDefaultAlignment` (16) and the helper names stay. New: `configureKernelHeap`,
   `kernelHeapBase()`, `kernelHeapEnd()` — the *game's* heap, recorded by `SetupHeap` and reported by
   `EndOfHeap`, never allocated from.
41. (iteration 10, rule c) **Exhaustion is loud**: the boot-mode pool (`Helpers/Runtime.h`,
   `allocBootModeAddr`), the invocation stacks (`PS2Runtime::logInvocationStacksExhausted`, called
   from `reserveAsyncCallbackStack`) and the runtime's allocator (its own `std::cerr` line in
   `allocateGuestBlockLocked`) each print one line naming the region, the size asked and what is left,
   up to eight times. `EeScheduler::invocationStackTop` already throws
   `std::runtime_error("EE invocation stack space exhausted")` when the reservation fails, so the
   failure reaches the caller. The packet, server and TLS pools keep wrapping by design (unchanged).
42. (iteration 10, the acceptance) **The comparison with the console now matches.** `DON_DUMP_RAM`
   + `tools/verify/ram_diff.py` against `work/ref/x/01/ee_ram.bin`, range `0x4D69B0:0x4D6A10`: 3 of 24
   words differ, and `0x4D69B8 = 0x01F50C20` and `0x4D69C4 = 0x009D73F0` are **equal** to the console's
   (before: 0x1ED2F20 and 0x9596F0). The three that differ are arena pointers with a different
   allocation history. The two pool descriptors: `0x5446E0` `+0x10`/`+0x14` =
   `0x00D29C00`/`0x0082A8C0`, **equal to the console's**; `0x544708` `+0x14` = `0x007A31F0`, equal,
   and `+0x10` = `0x00DA9C40` vs `0x00DA9D20` (0xE0 away). All four are guest-heap addresses, no
   longer 0x1F…/0x3A…. Evidence: `work/orchestrator/t016k/README.md` sections 4-6.
43. (iteration 10, the result) **The boot is unblocked.** Before the patch a 60 s run ended at frame 0
   with `RESULT: MISSING_BRANCH_TARGET 0x0`; after it, three 60 s runs give `RESULT: OK_VSYNC_LOOP`
   (360 frames/480 vsyncs, then 420/540 and 420/540). The game boots to its **memory-card warning
   dialog** and draws it (`--shots work/shots/boot --shot-every 300` writes `frame_000300.png`: the
   Japanese panel that says the card in slot 1 has no data for the game, 65 KB are needed to save, and
   asks whether to start anyway, with 「はい」/「いいえ」 and 「いいえ」 lit). The 14 module loads, the
   file searches and the remote-call registrations still happen: the dump shows the registered server
   structures in the new server pool at `0x000D0000` (SID `0x80000900`, the multitap service).
44. (iteration 10, rule e, measured with breakpoints during a 60 s run, `work/orchestrator/t016k/`)
   `PS2Runtime::guestMalloc` is called **2** times, both of 0x80 bytes (the runtime's stand-in GS
   packet; the dump shows exactly one 0x80-byte block at `0x000F1000`), and
   `PS2Runtime::reserveAsyncCallbackStack` **4** times, each 0x4000 — 4 of the 16 stacks, and the dump
   shows 3 used stack slots (0x000B4000, 0x000B8000, 0x000BC000). `ps2_syscalls::CreateThread` is
   reached 4 times and does **not** allocate a stack from the runtime (the game passes its own stacks),
   so the 0xF000-byte limit of the runtime's allocator is not near.
45. (iteration 10, rule f — the survey, `work/orchestrator/t016k/README.md` section 6) The places
   where a runtime handler writes guest memory at an address the game did not give it are: (a) the
   four pools, the invocation stacks and the runtime's allocator — this patch; (b) the **scratchpad**
   `0x70000000`: `writeGsGParamToScratch` (`Kernel/Stubs/Helpers/Support.h:1945`) copies the runtime's
   `g_gparam` to `0x700100` and returns that address, from `sceGsGetGParam` (`GS.cpp:756`) and
   `sceGsPutGParam` (`GS.cpp:804`); the same block is read back by `sceGszbufaddr` (`GS.cpp:1351`),
   `Ps2GetResSpec` and `sceGsExecLoadImage`; (c) a fixed address **inside the game's image**:
   `Font.cpp` `kFontBase = 0x176148` (writes the glyph/kern block pointer and a flag at
   `0x176148 + fontId * kFontEntrySz`); (d) a thread's stack through `guestMalloc` when the game calls
   `CreateThread` with stack 0 (`Thread.cpp:296`); (e) the ELF loader can load a `PT_LOAD` segment
   into the scratchpad, but only when the ELF asks for it (`Loader.h:343`). **Which of them this game
   reaches**: (b) and (c) are **not reached** (breakpoints on `sceGsGetGParam`, `sceGsPutGParam`,
   `sceGszbufaddr`, `sceGsExecLoadImage`, `sceGsExecStoreImage`: no hit in 30 s; and every word of
   `kFontBase` in the dump equals the console's), (d) is not reached either (the game passes its own
   stacks), and (e) does not happen for this ELF (the game's first segment is 0x00100000 and it has no
   scratchpad segment).

## Cause
**Measured** (items 25-30, 33) and **confirmed by the fix** (items 42-43): the first point of
departure was the **heap the runtime gives the game**. With the runtime's own memory moved out of the
game's heap and `SetupHeap`/`EndOfHeap` following the console's kernel, the two arena words of the
game's boot match the console's image exactly and the boot no longer stops at frame 0.
The boot asks for a heap ending at 0x1F7DBFF and the runtime's `SetupHeap`/`EndOfHeap` clamp it to
**0x01F00000** (`kDefaultGuestHeapEnd`/`kGuestHeapHardLimit`), while the console's heap ends at
~**0x1F80000** (32 MiB minus the 0x80000 stack the game reserved with `SetupThread`): the game then
shrinks its request in 0x100 steps (measured retry counter 0x4D6A08 = 1791 here, 1 on the console)
and its two arenas come out ~515 KiB smaller (measured deltas 0x7DA00 and 0x7DD00 against the
console's memory image), exactly the difference docs/knowledge/16-boot-sequence.md section 9 item 1
had recorded. The engine allocates its texture pools from those arenas through the game's own
allocator (item 28, no runtime handler on that path), so the pools end up at 0x1F…-0x3A… instead of
the console's 0x7A…-0xDA… (item 27); every texture's pixel pointer is then wrong (items 18-19) and
the engine's copies to those pointers overwrite the game's code area first and then the platform flag
at 0x3C0360 (items 12, 18). Everything the previous iterations saw (the plain-name file lookups, the
failed load of `cmn/1000.txd`, the null indirect call, the corrupted manager object) is downstream of
this single difference. **The engine's own start-up is not at fault**: every slot of its device table
matches the console, including the real graphics create function 0x2D9F40 (item 26).

## Fix
**Applied: patch 0032, `patches/ps2recomp/0032-runtime-guest-memory.patch`** (iteration 10, decided as
option 1; items 38-45 and `work/orchestrator/t016k/README.md`). One topic: *the runtime's own guest
memory*. Everything the runtime keeps in guest RAM (the four pools, the invocation stacks and the
allocator behind `guestMalloc` and friends) moves to a home of its own in the console's kernel area
`0x00080000-0x00100000` (measured empty of the game on all six console images, item 39), defined once
in the new header `runtime/ps2_guest_memory.h` with `static_assert`s; and `SetupHeap`/`EndOfHeap` obey
the console's kernel again: the end is `start + size`, or, when the size is `-1`, the bottom of the
calling thread's stack as `SetupThread` recorded it — no `0x01F00000` clamp, and the runtime's
allocator no longer receives the game's range (rules b and d, item 40). Exhaustion of the new regions
is loud (rule c, item 41). `SetupHeap`'s return value is unchanged because the game ignores it
(item 35). The runtime's malloc handlers (`Kernel/Stubs/LibC.cpp`, `Compatibility.cpp`) are not used
by this game (its C library is recompiled) and now allocate from the runtime's private range; a game
that relied on them for its whole heap would need the old behaviour (in the patch header).

Iteration 9 stopped before writing the patch because the runtime's allocator was live over the game's
heap (item 36); the orchestrator then decided option 1, and the record of why is kept below
(`Superseded`) and in item 38.

### Options (for the orchestrator)
**Decided on 2026-10-12: option 1**, implemented as patch 0032 in iteration 10 (item 38). The options
are kept as written, for the record.
1. **Give the runtime's own guest allocations a home outside the game's heap** — the same treatment
   item 34's survey applies to the four pools: the free kernel area 0x000A0000-0x000FFFFF (measured
   zero on two console images) can hold the four pools (0xC0000/0xD0000/0xE0000/0xF0000 as designed)
   and, above/below them, the runtime's *other* guest structures: the GS/font/LibC packet buffers that
   `guestMalloc` serves from the guest heap, and the invocation stacks of
   `reserveAsyncCallbackStack`. `guestMalloc` would then need a base/limit of its own (e.g. the
   0x000A0000-0x000C0000 area, 128 KiB, or a region the runtime reserves with `guestHeapBase/Limit`
   while `EndOfHeap` still reports the game's console heap). This is the option that removes the
   collision instead of moving it.
2. **Patch 0032 alone, accepting the shared range** (the runtime's blocks keep coming from 0x5A6800,
   inside the game's arena). Cheap and testable (the design's step 4 test), but two allocators over
   one range stay, and the game's first arena header (0x5A6800-0x5A6840) is exactly where the
   runtime's first GS packet lands.
3. **Only raise the hard limit** (`kGuestHeapHardLimit`) and leave the runtime's allocator where it
   is: `EndOfHeap` would then report the console's end while the runtime's free list still starts at
   0x5A6800 — the same collision as option 2, with less work.
4. Whatever is decided, the acceptance is the comparison of item 25/26: `DON_DUMP_RAM` + `ram_diff.py`
   against `work/ref/x/01/ee_ram.bin`, checking `0x4D69B8 = 0x01F50C20` and
   `0x4D69C4 = 0x009D73F0` and heap addresses in the two pool descriptors, and then three 60 s smoke
   runs.

## Superseded (iteration 5, kept as the record)
**Iteration 7's Cause** ("the pixel pointer comes from the engine's own static bitmap pool and the
failing block covers the flag") was measured and stands as the *mechanism*; what is superseded is its
attribution — the pool is not a *static* area of the game's data by nature: items 25-30 show the
engine's pools are in the *guest heap* on the console and land in the data/code in this run because
the heap the runtime gives the game is ~515 KiB smaller (item 30). The pool path itself
(`FUN_002bcbb8`, `FUN_002c97d8`, `FUN_002ddfb0`) is unchanged and correct.

The "Cause" and "Options" as written before the orchestrator's review of 2026-10-12:

> The port has no component that resolves the game's file-package names (`cmn/1000.txd`,
> `cmn/1000.dff`, and the many that would follow: `game/…`, `menu/…`, `option/…`, `viewer/…`). On the
> console those names are served by the IOP side of the CRI/RenderWare file system (the modules
> `cdvdstm`, `cri_adxi` and the `rtfsiop`/`RtFS` driver, over the `fpack/*.fpk` containers inside
> `DATA.AFS`); in the port the EE-side lookup was bound to the host-folder `sceCdSearchFile`
> (journal 002), which only knows host files. The failed lookup makes the game take its error path,
> which writes into the wrong object and ends its main thread. Which console component answers that
> lookup is **not determined**, and that is why the fix is not of an allowed kind.

and the three options of that iteration: serve the names from the host folder (data extraction),
teach the host CD layer to read the packages (a new subsystem), or run the IOP side for real
(`IOPRP300.IMG` + `cdvdstm`/`rtfsiop`). **The orchestrator's review above rejects these**: the disc has
no `CMN` directory, so the failed search happens on the console too and no component for "package
names" is missing; the packages are read by the game's own code (items 8-10 confirm both: the game
loads `fpack/cmn/1000.fpk` itself and holds its entries in memory).

## Verification
This iteration added two pieces of code and ran them:
* `DON_DUMP_RAM` (src/host/host.cpp): `DON_DUMP_RAM=work/ramdump/run.bin bash tools/run/smoke.sh 30` →
  `[don] RAM dump work/ramdump/run.bin: 33554432 of 33554432 bytes` (32 MiB, the whole guest RAM).
* `tools/verify/ram_diff.py` and `tests/test_ram_diff.py`:
  `uv run --no-project --with pytest python -I -m pytest tests/test_ram_diff.py -q` → `1 passed`.
  The host tests (src/host changed): `bash tools/pipeline/build_host.sh` (23 s) and
  `uv run --no-project --with pillow --with pytest python -I -m pytest tests/host -q` → `9 passed`.
* The comparison itself (item 26/27): `uv run --no-project python -I tools/verify/ram_diff.py
  work/ramdump/run.bin work/ref/x/01/ee_ram.bin --range 0x391000:0x5A6800 --kind code` → 3693
  differing words, none of them in the device table at 0x540640.
* The run is still `RESULT: MISSING_BRANCH_TARGET 0x0` (nothing was fixed).

### Iteration 10 (patch 0032): verification

Run after the last change (`bash tools/setup/fetch_ps2recomp.sh --reset` applied all 14 patches from
clean and `bash tools/pipeline/build.sh linux-dev` rebuilt the tree, 6m40s):

* `DON_DUMP_RAM=work/ramdump/run.bin bash tools/run/smoke.sh 30` →
  `[don] RAM dump work/ramdump/run.bin: 33554432 of 33554432 bytes` (the classification of that log
  reads `NO_PROGRESS [don] RAM dump ...` only because the dump line is printed last and matches the
  classifier's progress pattern; with that line removed the same log is `OK_VSYNC_LOOP 180 300`).
* `uv run --no-project python -I tools/verify/ram_diff.py work/ramdump/run.bin
  work/ref/x/01/ee_ram.bin --range 0x4D69B0:0x4D6A10 --kind all` → `3 differing word(s) of 24`:
  `0x4D69B8` (0x01F50C20) and `0x4D69C4` (0x009D73F0) are **equal to the console's** (before:
  0x1ED2F20 / 0x9596F0); the three that differ (0x4D69B0, 0x4D69D0, 0x4D69D8) are arena pointers
  whose history differs (the console image is the title screen, the dump is the memory-card dialog).
* Pool descriptors: `0x5446E0` +0x10/+0x14 = 0x00D29C00/0x0082A8C0, **both equal to the console's**;
  `0x544708` +0x14 = 0x007A31F0 equal and +0x10 = 0x00DA9C40 vs 0x00DA9D20 (0xE0 away). All four are
  guest-heap addresses now (before: 0x1F9980/0x3A5C60 and 0x212E00/0x1FC9B0).
* Three runs of `bash tools/run/smoke.sh 60` → `RESULT: OK_VSYNC_LOOP 420 540`, `420 540`, `300 420`.
  Before the patch the same run was `RESULT: MISSING_BRANCH_TARGET 0x0` with frames=0. The 14 module
  loads still happen, the dump shows the registered remote-call server structures in the new server
  pool at 0x000D0000, and the log shows the memory card polled (`[MC] GetInfo`/`[MC] Sync`, 195/196
  times in a 60 s run) while the dialog waits for a button.
* `./build/linux-dev/don --headless --seconds 60 --shots work/shots/boot --shot-every 300
  disc/SLPS_256.75` → `work/shots/boot/frame_000300.png` (29374 bytes), the game's memory-card warning
  dialog (described in words in `work/orchestrator/t016k/README.md` section 7).
* `tests/runtime`:
  `env -u DISPLAY DON_RT_TESTS_DIR=build/rt-tests uv run --no-project --with pytest python -I -m pytest
  tests/runtime -q` → `9 passed` (the eight that existed plus `guest_heap_limit_test`). The new test
  was built **first** against the tree without the patch (`build/rt-tests-nopatch` = the pinned commit
  plus series 0001-0031): it compiles there and fails 13 checks (its output is in the evidence README),
  and passes against the patched tree (`heap end 0x01F80000, server 0x000D0080, packet 0x000C0040, tls
  0x000E0100, bootmode 0x000F0020, stack 0x000B7FF0, scratch 0x000F1100`).
* Breakpoints during a 60 s run (item 44): the runtime's allocator serves 2 requests, both 0x80 bytes
  (of its 0xF000-byte region) and 4 invocation stacks of 0x4000 are reserved (of 16).

## Temporary diagnostics
`DON_TRACE_THREADS=1` (`src/game/don_overrides.cpp`, off by default): the thread table.
`DON_DUMP_RAM=<file>` (`src/host/host.cpp`, off by default): the 32 MiB guest RAM dump (item 25) —
a permanent diagnostic of the same kind, not temporary. `tools/run/gdb_trace.sh` and
`tools/verify/ram_diff.py` (both external/reusable). The evidence of this iteration is under
`work/orchestrator/t016i/` (README.md with the numbers, diff01.txt, order.log, globals.log,
pool_writes.log, dec-pool.txt, dec-poolcreate.txt, dec-heapboot.txt); the RAM dumps stay under
`work/ramdump/`.
