# 061 · memchr on the Windows game thread: it is a large memmove, not memchr

- **Date:** 2026-10-28
- **Task:** T-763
- **Status:** resolved for the question asked (the "memchr" is a mislabel); the caller of the large copy is open

## Symptom
Journal 059 (T-761) symbolized the Windows game-thread profile of the 38cc0fe module build and listed
`VCRUNTIME140 memchr` 1880 samples (2.8 %) and `memmove` 867 (1.3 %). At about 0.4 ms per blank the memchr row
was the size of the +0.67 ms/blank gap against 63fc1b6 (journal 055, 14.96 vs 14.29 ms).

## Classification
Measurement artefact (symbol attribution) plus a possible real cost. No precedent in the catalog.

## Investigation
1. Call sites. Disassembled the profiled exe (same `don-map.exe` and `don.map` as T-761) with the mingw
   `objdump`, resolved callers with the map. Every call goes through the import thunks (`memchr`, `memcpy`,
   `memmove`). `memchr` has 9 call sites, all in third-party ImGui text code (`TextEx`, `RenderText`,
   `LogRenderedText`, `LoadIniSettingsFromMemory`) and one setup text reader. None is in `EeScheduler::run`,
   `accountCycles`, `processVIF1Data`, `writeIORegister`, the IOP core, `dumpState`, the GS front or the host's
   blank hooks. So `std::string::find`, `getenv` and `strchr` are not the cause. (List: scratch `callers.txt`.)
2. Re-reading the raw samples of the profile by RVA inside `VCRUNTIME140.dll` (T-761 used "nearest export
   below"): 1862 of the 1880 "memchr" samples sit at one address, RVA `0x1dafb`. The export `memchr` is at
   `0x1d960`; real memchr code (`0x1da53..0x1da93`) has about 18 samples. `0x1dafb` is
   `rep movsb [rdi],[rsi]` inside an unexported helper at `0x1daf0` (`push rdi; push rsi; mov rcx,r8; rep movsb`),
   placed just below the `memmove` export (`0x1db00`).
3. When memmove takes that path: from `0x1dc5e`, `jne 0x1daf0` at `0x1dc87` (and `0x1df60`) when the size is
   greater than `0x2000` and at most `0x180000` bytes and the fast-string flag (ERMS) bit is set; between 0x800 and
   0x2000 the other CPU variant branch jumps there too (`0x1df50`). So the 1862 samples are time inside
   copies of roughly 8 KiB to 1.5 MiB, not searches. The 867 "memmove" are the same function's small and
   medium paths, 34 more are `memcpy` (RVA `0x1f020`, a jump to memmove).
4. Cost: 1862 samples is 2.73 % of 68158 samples of the run, which is about 0.41 to 0.46 ms of a 15 to 16.7 ms
   blank (2.73 % times the blank length). The size matches the gap to 63fc1b6 (+0.67 ms), but the old build's
   own share is unknown (no profile of it).
5. Who copies that much each blank. The IP does not give the caller. Candidates by size and place, all in the
   game thread: the chain-DMA gather into `chainBuf` (`ps2_memory.cpp` lines 1421, 1440, 1809) and the
   `imagePacket` copy in `ps2_vif1_interpreter.cpp:377-378` (a whole display list or image upload), then the
   host GS ring copy `GsThread::gif` (`gs_thread.cpp:181,191,197`), the guest `memcpy` stub (`LibC.cpp:90`) and
   `HostGs::read_fifo` (`gs_glue.cpp:525`).
6. New since 63fc1b6? No. `git log 63fc1b6..main` on `patches/ps2recomp/` has only build patches (0089 CLI,
   0090 static libstdc++, 0091 emit-module-abi, 0092 windows-static); `gs_thread.cpp` has one commit (98f24a5,
   an ancestor of 63fc1b6); the chain buffer reuse is patch 0083, also older. `src/host` changes since are the
   module loader, the setup screens, crash trace and the profiler. So the code that does the copies is the same;
   what changed is how it is built and linked (module exe at `-O2` with the import thunk to the DLL; whether the
   old monolith inlined or statically linked a different memcpy is not known).

## Cause
Not determined. Determined: the row is a large `rep movsb` copy (8 KiB to 1.5 MiB) in the game thread, not
`memchr`; its share (about 0.4 ms per blank) is not from new source code.

## Fix
Proposal, not done. (a) Find the caller: in `profile_win32.cpp`, when the sampled RIP falls in `VCRUNTIME140`
(RVA `0x1daf0..0x1dc90`), also store the return address read from `[RSP]` (the helper has pushed two registers,
so read `[RSP+0x10]`; from the memmove entry itself `[RSP]`) as a second sample; one 130 s run on the PC names the
caller. (b) Then remove the copy at its place: if it is the DMA chain gather, hand the GS ring the guest RAM
spans directly (one copy into the ring instead of gather, then ring copy), or make the gather copies bigger
than one `memmove` per tag. Expected saving: most of the 0.4 ms per blank, about 2.7 % of the game thread,
which would close about 60 % of the 1.4 fps gap to the old build; unmeasured. (c) In parallel, the T-762 symbol
diff should say whether the old monolith called `memmove` for the same copies (imports of its exe).

## Verification
Raw sample counts by RVA from the profile (`o2-9dc-noffmpeg.profile`) and the disassembly of
`VCRUNTIME140.dll` 0x1daf0 and 0x1dc5e..0x1dc87, as above. No build, no run.

## Temporary diagnostics
None added.
