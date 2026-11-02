# 14 · IOP modules: what they offer, what they import, what the emulator covers

Inventory of the 17 IRX files in the two module packs of the game (16 distinct modules,
`libsd` is in both) and of the IOP kernel/library functions they import. Context:
[05-audio-video-input.md](05-audio-video-input.md).

Markers: **Measured** = obtained by running `tools/re/irx_imports.py` on the packs or by reading
the emulator source. **Declared** = stated by a document. **Unverified** = not checked.

Data: [data/irx_imports.tsv](data/irx_imports.tsv), one row per imported function
(columns: `module`, `module_name`, `module_version`, `imported_library`, `version`, `ordinal`,
`supported`, `provider`, `package`). It holds names, versions and ordinals only, no code.

## Result in one paragraph

**Measured.** The 17 modules import 709 functions in total (`libsd` appears twice, 7 + 7 counted
once per package). The IOP emulator implements 506 of them (`yes`), 169 are satisfied by an export
table of another module of the game's own set (`module`), and **34 are unsupported** (`no`).
**None of the 34 belongs to a sound module** (`sdrdrv`, `modhsyn`, `modmidi`, `modmsin`, `sndfi`,
`cri_adxi`, `libsd`): 0 unsupported there. The 34 are in `mcman`, `padman`, `cdvdstm`, `rtfssiop`
and in `tsnddrv` / `tsnddrvc`, whose loading is **Unverified** (05 notes that the names do not
appear in the executable; task T-102 will settle it). "Supported" only says that the import has a
handler; it does not say that the handler is faithful (see "What `supported` does not mean").

## Modules

Versions: module version from the `.iopmod` section; library versions are `major.minor`
from the tables. The "module name" of `.iopmod` is the internal one (`Sound_Device_Library`),
the first column is the file name inside the pack. **Measured.**

| Pack | Module (file) | `.iopmod` name · version | Library it offers (version · functions) | Imports | Unsupported |
|---|---|---|---|---:|---:|
| 0000 | `libsd` | Sound_Device_Library · 3.03 | `libsd` 1.05 · 34 | 19 | 0 |
| 0000 | `sio2man` | sio2man · 3.01 | `sio2man` 2.04 · 64 (corrected on 2026-10-11: was 4; `irx_imports.py` counts 64) | 28 | 0 |
| 0000 | `mtapman` | multitap_manager · 3.16 | `mtapman` 2.04 · 7 | 37 | 0 |
| 0000 | `mcman` | mcman · 2.38 | `mcman` 2.07 · 62 | 35 | 2 |
| 0000 | `mcserv` | mcserv · 2.16 | `mcserv` 2.01 · 8 | 53 | 0 |
| 0000 | `padman` | padman · 4.33 | `padman` 2.04 · 18 | 55 | 1 |
| 0000 | `cdvdstm` | cdvd_st_driver · 2.02 | `cdvdstm` 1.01 · 5 | 34 | 7 |
| 0000 | `rtfssiop` | (empty name) · 0.00 | `rtfsiop` 1.01 · 9 | 52 | 5 |
| 1000 | `sdrdrv` | sdr_driver · 4.01 | `sdrdrv` 1.01 · 6 | 52 | 0 |
| 1000 | `modhsyn` | SPU2_Synthesizer_Module · 1.05 | `modhsyn` 1.04 · 30 | 24 | 0 |
| 1000 | `modmidi` | Midi_Sequencer_Module · 2.02 | `modmidi` 1.05 · 26 | 8 | 0 |
| 1000 | `modmsin` | Midi_Message_Input_Module · 1.01 | `modmsin` 1.01 · 10 | 3 | 0 |
| 1000 | `tsnddrv` | CRI_ADX_Driver · 9.31 | none | 104 | 10 |
| 1000 | `cri_adxi` | CRI_ADX_Driver · 9.31 | none | 42 | 0 |
| 1000 | `tsnddrvc` | CRI_ADX_Driver · 9.31 | none | 102 | 9 |
| 1000 | `sndfi` | SNDF_Driver · 2.13 | `sndfi` 1.01 · 18 | 42 | 0 |
| 1000 | `libsd` | Sound_Device_Library · 3.03 | `libsd` 1.05 · 34 | 19 | 0 |
| | **Total** | | | **709** | **34** |

Notes:

- The function count of an export table follows the emulator: words up to the first zero word
  after loading (`iop_imports.cpp:120`). In `modhsyn`, `modmidi` and `modmsin` slot 0 is zero in
  the file but carries an `R_MIPS_32` relocation (pointer to offset 0 of the module), so it is
  non-zero once loaded; the script takes that into account.
- The two copies of `libsd` have the same imports. Whether they are byte-identical is
  **Unverified** (only the uncompressed size, 28,661, is known to match, see 05).
- `tsnddrv`, `tsnddrvc` and `cri_adxi` share the `.iopmod` name `CRI_ADX_Driver`; they are
  different files (sizes 185,036 / 171,580 / 69,237).

### What each module needs, by library

Counts of imported functions per library, **Measured**. `(…)` after a count: how many are not
`yes`.

| Module | Imports (library count) |
|---|---|
| `libsd` | intrman 7, loadcore 2, sifman 2, sysclib 1, thevent 7 |
| `sio2man` | loadcore 1, intrman 6, stdio 1, dmacman 5, thbase 3, thevent 5, thsemap 4, sysclib 3 |
| `mtapman` | sysmem 1, loadcore 2, intrman 2, stdio 1, thbase 6, thevent 3, thsemap 4, sifman 4, sysclib 3, sifcmd 4, sio2man 7 (7 module) |
| `mcman` | loadcore 2, intrman 2, sio2man 5 (5 module), sysclib 10, stdio 1, thbase 3, thsemap 5, modload 1, ioman 2, **secrman 3 (1 no)**, **cdvdman 1 (1 no)** |
| `mcserv` | loadcore 2, intrman 2, sysclib 4, stdio 1, sifman 4, sifcmd 7, thbase 8, thsemap 2, mcman 23 (23 module) |
| `padman` | sysmem 1, loadcore 2, intrman 5, stdio 1, sio2man 7 (7 module), thbase 7, thevent 6, thsemap 4, timrman 6, **sifman 5 (1 no)**, sifcmd 5, vblank 2, sysclib 4 |
| `cdvdstm` | sysmem 1, loadcore 2, intrman 3, stdio 1, thbase 4, thevent 5, thsemap 4, sifman 2, ioman 2, **cdvdman 10 (7 no)** |
| `rtfssiop` | sysmem 3, intrman 2, **ioman 5 (5 no)**, loadcore 1, sifcmd 7, sifman 2, sysclib 9, thbase 10, thevent 4, thsemap 4, cdvdman 5 |
| `sdrdrv` | libsd 26 (26 module), sysmem 1, intrman 2, loadcore 2, sifcmd 8, sysclib 3, thbase 10 |
| `modhsyn` | libsd 19 (19 module), intrman 2, loadcore 1, thbase 2 |
| `modmidi` | intrman 2, loadcore 1, stdio 1, sysclib 4 |
| `modmsin` | intrman 2, loadcore 1 |
| `tsnddrv` | sysmem 5, intrman 3, **ioman 5 (5 no)**, loadcore 1, sifcmd 8, sifman 2, stdio 1, sysclib 9, thbase 10, thevent 5, thsemap 6, **thmsgbx 5 (5 no)**, timrman 6, vblank 1, libsd 14 (14 module), cdvdman 6, modhsyn 7 (7 module), modmidi 7 (7 module), modmsin 3 (3 module) |
| `cri_adxi` | libsd 10 (10 module), intrman 2, loadcore 1, sifcmd 7, sifman 3, stdio 1, sysclib 8, thbase 5, thsemap 4, vblank 1 |
| `tsnddrvc` | sysmem 2, intrman 3, **ioman 3 (3 no)**, loadcore 1, sifcmd 8, sifman 2, stdio 1, sysclib 9, thbase 10, thevent 5, thsemap 7, **thmsgbx 5 (5 no)**, timrman 6, vblank 1, libsd 14 (14 module), cdvdman 8 (1 no), modhsyn 7 (7 module), modmidi 7 (7 module), modmsin 3 (3 module) |
| `sndfi` | sysmem 2, intrman 2, loadcore 2, sifcmd 6, sifman 2, stdio 1, sysclib 2, thbase 10, timrman 5, libsd 10 (10 module) |
| `libsd` (1000) | same as `libsd` (0000) |

The sound chain is `sdrdrv`, `modhsyn`, `sndfi` and `cri_adxi` → `libsd`;
`modmidi` and `modmsin` import only kernel libraries. `libsd` itself only imports kernel
libraries, all `yes`. So the sound modules do not depend on any emulator function that is missing.

## How the emulator decides whether an import is handled

**Measured** by reading `third_party/PS2Recomp/ps2xIOP` (commit `2c5fbb9`, with this
repository's patches applied).

1. `IopImportRegistry::decode` (`src/emulator/imports/iop_imports.cpp:48`) recognizes a call to
   an import when the CPU reaches a `jr $ra` + `li $zero, N` stub. It searches backwards, up to
   64 KiB, for the `0x41E00000` header and returns (library name, ordinal, version)
   (called from `IopEmulator::step`, `src/emulator/iop_emulator.cpp:381`).
2. `IopEmulator::dispatchImport` (`iop_emulator.cpp:240`) is a chain of `if`s on the library
   name (case-insensitive), in this order, first match wins:
   - `sysmem`, `cdvdman`, `loadcore`: the per-library `dispatchImport(ordinal, cpu)`
     (`imports/iop_sysmem.cpp`, `iop_cdvd.cpp`, `iop_loadcore.cpp`); if it returns `false` the
     chain continues.
   - `thbase`/`threadman`, `thsemap`, `thevent`: `IopKernel::dispatchThreadImport`,
     `dispatchSemaphoreImport`, `dispatchEventImport` (`core/iop_kernel.cpp:37, 353, 527`);
     `false` means **Missing** immediately.
   - `sifcmd`, `sifman`: `IopRpcBridge::dispatchSifCmdImport`, `dispatchSifManImport`
     (`services/iop_rpc.cpp`); `false` means **Missing**.
     (corrected on 2026-10-11: the `default:` of `dispatchSifManImport`, `iop_rpc.cpp:128-130`,
     does `setV0(0)` and returns `true`, so **every** `sifman` ordinal is handled, with 0 for the
     ones without a `case`; `sifman:32` of `padman` is therefore not unsupported. **Measured.**
     The `sifcmd` default returns `false`. Other libraries' defaults were not re-checked
     individually; `ioman` and `cdvdman` return `false` and fall to step 3.)
   - `intrman`, `ioman`, `vblank`, `timrman`, `stdio`, `heaplib`: their `dispatchImport`; `false`
     continues the chain. `sysclib`: `false` means **Missing**.
   - Inline in `iop_emulator.cpp`: `secrman` ordinals 4 and 5 (lines 295-309), `modload` ordinal 13
     (line 311), `dmacman` every ordinal (line 329, returns 0).
3. If nothing above handled it, `imports.resolve(library, ordinal, version)` (`iop_emulator.cpp:345`)
   looks for an **export table registered by a loaded module** (`IopImportRegistry::resolve`,
   `iop_imports.cpp:157`). A module registers its exports with `loadcore` ordinal 6
   (`iop_loadcore.cpp:41` → `registerExportTable`). The match is by library name
   (case-insensitive) and **major version** (`iop_imports.cpp:141`); the highest minor wins;
   the ordinal indexes the function array, which ends at the first zero word
   (`iop_imports.cpp:120`). If found, the CPU jumps to the module's function (`JumpToGuest`).
4. Otherwise it logs `[IOP] unhandled import <lib>:<ordinal>` (`iop_emulator.cpp:354`), sets
   `$v0 = 0` and returns to the caller (**Missing**).
   (corrected on 2026-10-11, **Measured**, `iop_emulator.cpp:345-358`: this `$v0 = 0` happens
   only for libraries whose handler returned `false` and then found no export table, i.e. `ioman`,
   `cdvdman`, `thmsgbx`, `secrman`, etc. For `thbase`, `thsemap`, `thevent`, `sifcmd` and `sysclib`
   a handler that returns `false` ends as **Missing** without touching `$v0`, so the caller sees
   whatever the register held. Resolution is always at call time, never at load time: an
   unresolved import cannot make a module load fail.)

So the emulator's own handler wins over a game module that exports the same library; and a
library that no handler knows is served only if some loaded module exports it.

### How `supported` was derived

`tools/re/irx_imports.py` repeats this decision statically:

- For every library in the `if` chain of step 2 it reads the **numeric `case N:` labels at the top
  level of `switch (ordinal)`** of the dispatching function (the table `DISPATCHERS` in the
  script maps each library to its source file and function; nested `switch`es are ignored).
  A (library, ordinal) with a label is `yes`. The three inline cases are in the table `INLINE`.
- Otherwise it looks for a provider among all modules of both packs: same library name, same
  major version, and ordinal below the number of exported functions. If found: `module` (column
  `provider` names it). If a provider exists but the table is too short: `no` with the provider
  still shown (does not happen in this data).
- Otherwise `no`.

Libraries handled by the emulator, with the ordinals it has (derived by the script, **Measured**):
`sysmem` 4-10, 14, 15; `cdvdman` 4-8, 10-13, 28, 37, 50, 75; `loadcore` 3-17, 20, 21, 27;
`thbase` 4-41; `thsemap` 4-9, 11, 12; `thevent` 4-11, 13, 14; `sifcmd` 4-29; `sifman` 4, 5, 7, 8, 29;
`intrman` 3-9, 14-18, 23-25, 28, 30; `ioman` **20, 21 only** (AddDrv, DelDrv); `vblank` 4-9;
`timrman` 3-18, 20-24; `stdio` 4-14; `sysclib` 4-38, 40-43; `heaplib` 4-8, 11, 15; `secrman` 4, 5;
`modload` 13; `dmacman` all.

### What `supported` does not mean

**Measured** about the method, **Unverified** about the behavior:

- `yes` means a `case` exists, not that it is faithful. Several handlers are approximations (for
  example `iop_sysclib.cpp`: `longjmp` is marked TODO, `vsprintf` is a fallback, `strtok` state
  is not shared; `dmacman` returns 0 for everything; `sceCdSC` controls are no-ops). A sound
  module may call such an import and misbehave without any "unhandled import" message.
- `module` means the export table exists in the game's own set and has that ordinal. It requires
  the provider to be **loaded before** the call, and the provider then runs against the
  emulated hardware: `libsd` writes the SPU2 registers, `sio2man` the SIO2 port. Whether the
  emulator models those registers is a separate question (not covered here).
- The script does not check that the game loads each module (task T-102).

## Unsupported imports (34)

Grouped by library, **Measured**. "Module" is the file that imports it.

| Library | Ordinals (importer) | Count |
|---|---|---:|
| `ioman` | 4 (`rtfssiop`, `tsnddrv`, `tsnddrvc`), 5 (same three), 6 (`rtfssiop`, `tsnddrv`), 7 (`tsnddrv`, `tsnddrvc`), 8 (`rtfssiop`, `tsnddrv`), 28 (`rtfssiop`) | 13 |
| `cdvdman` | 24 (`mcman`), 39 (`cdvdstm`, `tsnddrvc`), 40, 46, 48, 49, 61, 114 (`cdvdstm`) | 9 |
| `thmsgbx` | 4, 5, 6, 8, 9 (`tsnddrv`, `tsnddrvc`) | 10 |
| `secrman` | 6 (`mcman`) | 1 |
| `sifman` | 32 (`padman`) | 1 |
| | **Total** | **34** |

(Counts are per importing module and ordinal: `ioman` is 5 + 5 + 3 in `rtfssiop`, `tsnddrv`,
`tsnddrvc`; `cdvdman` is 1 + 7 + 1 in `mcman`, `cdvdstm`, `tsnddrvc`; `thmsgbx` is 5 + 5.)

Per module: `mcman` 2 (`secrman:6`, `cdvdman:24`), `padman` 1 (`sifman:32`), `cdvdstm` 7
(`cdvdman`), `rtfssiop` 5 (`ioman`), `tsnddrv` 10 (`ioman` 5, `thmsgbx` 5), `tsnddrvc` 9
(`ioman` 3, `thmsgbx` 5, `cdvdman:39`).

### Sound modules (highlighted)

| Module | Imports | Unsupported | Provided by another module (`module`) |
|---|---:|---:|---|
| `sdrdrv` | 52 | **0** | 26 `libsd` |
| `modhsyn` | 24 | **0** | 19 `libsd` |
| `modmidi` | 8 | **0** | none |
| `modmsin` | 3 | **0** | none |
| `sndfi` | 42 | **0** | 10 `libsd` |
| `cri_adxi` | 42 | **0** | 10 `libsd` |
| `libsd` | 19 | **0** | none |
| **Sum** | **190** | **0** | |

(`tsnddrv` / `tsnddrvc` are third-party sound drivers too; they import `thmsgbx` and `ioman`
functions that are unsupported, but they are not in the list of modules the task asks about and may
not be loaded.)

### The CRI audio driver's thread (journal 011, T-111; **Measured** 2026-10-13)

`cri_adxi` (CRI ADX Driver 9.31, base `0x6E000`, entry `0x6E57C`) runs one thread (id 10, entry
`0x78734`) that, with the stand-in sound service and no SPU2 library, kept the IOP busy: one
instruction per cycle given (`cycles == instructions`), about 9.4 M instructions per second, 18,524
gdb samples of 18,524 in thread 10, 99 % at `0x72E24`-`0x72E54`. That is the body of the
function at `0x72DFC`: a loop of 64 iterations over records of 0x24 bytes at `0x7E9E0`, which calls
the per-record service at `0x72D8C` when the state byte is 1 (five calls inside the module). It does
not poll SIO2 (`cri_adxi` does not import `sio2man`; its imports are `libsd` 10, `thbase` 5,
`thsemap` 4, `vblank` 1, `intrman` 2, `sifcmd`/`sifman`, `sysclib` 8, `stdio` 1, `loadcore` 1).
With the host SPU2 installed the thread waits (about 2 % of the cycles are instructions; its
`t10` waits on semaphore 6 with count 0, `t8` is in a delay, the RPC server of service
`0x90000200` sleeps). What the chip receives from the sound modules:
[05](05-audio-video-input.md), "Sound".

### The memory-card modules (journal 022; **Measured**)

`mcman` (2.38) and `mcserv` (2.16) are **loaded and not used**: the game's `libmc` functions are
replaced by the runtime, so no RPC to `0x80000400` is ever made, and the real `mcman` would need
SIO2 hardware that has no model. `MCMAN.IRX` keeps a table of **three** open-file entries of 0x30
bytes at module data offset `0x262B0` (first byte of an entry = in use); its open function scans from
entry 0 and returns the first free index, and `-7` (`sceMcResUpLimitHandle`) when all three are in
use. That table is the source of patch 0040 ([13](13-runtime-host.md), "The memory card").

### What the unsupported imports affect

**Unverified** (needs the load order, T-102, and a run):

- `ioman` 4-8 (open/close/read/write/lseek on files) and `thmsgbx` (message boxes) have **no
  handler at all**: a call logs a warning and returns 0. If `rtfssiop` is loaded it cannot open
  files; the same for `tsnddrv`/`tsnddrvc`.
- `cdvdman` 39, 40, 46, 48, 49, 61, 114 are the streaming functions used by `cdvdstm`
  (a module the game loads, by name in the executable); `cdvdman` 24 is `mcman`'s.
- `secrman:6` and `sifman:32` are called by `mcman` and `padman`.
- A returned 0 may or may not be a harmless value for each of them.

(corrected on 2026-10-11, **Measured** by disassembling the modules and walking `jal` from each
start function: none of the 34 is reached from a start function. `cdvdstm`'s seven are called
only from functions outside the start tree (`0x124`, `0x174`, `0x384`, `0xd38`, `0x1724`,
`0x2238`, `0x289c`, `0x3538`); `mcman`'s `cdvdman:24` is at `0x20c8`, reached only from the card
probe `0x6e8`, which the start function skips when `AddDrv` succeeds; `secrman:6` is at `0x3680`
and `0x10c64`; `sifman:32` is handled with 0 (see step 2). None of these can stop a load.)

## Load plan for the port

Added 2026-10-11 (T-016 iteration 1c). Question: for each of the 14 modules the game loads
(order in [16](16-boot-sequence.md) section 2), what does the runtime do with it, and what is
the lightest correct handling? Everything below is from reading source and disassembling the
modules; nothing was run. Modules were read in memory with `tools/re/irx_imports.py`/`tools/fpk.py`.

### A. What a load does (applies to all 14)

1. **Path.** `0x114C68` is replaced by `sceSifLoadModuleBuffer` (`System.cpp:288-324`) →
   `PS2Runtime::loadIopModuleBuffer` (`ps2_runtime.cpp:573`) → `IopSubsystem::loadModuleBuffer`
   (`iop_subsystem.cpp:148`) → `IopEmulator::loadModuleBuffer` (`iop_emulator.cpp:655`).
   **Measured.**
2. **Recognition is by ELF content, never by name.** The built-in name list
   (`iop_module_manager.cpp:13-53`, `loadHle`) is used only by `IopSubsystem::loadModule(path)`
   (`iop_subsystem.cpp:117-146`). The buffer path does not touch it, so "accept the name as
   built-in" is **not an available option** for these loads. **Measured.**
3. **Return value.** The handler returns -1 only if `handled` is false or `moduleId <= 0`
   (`System.cpp:315`). `loadImage` (`iop_emulator.cpp:591-642`) returns the module id (> 0) as
   soon as the ELF loads and relocates, whatever the start function returns. The -1 comes from:
   an invalid ELF header (`readElfFromGuest`, `iop_module_loader.cpp:323-330`), a module that
   does not fit the arena (`IopModuleLoader::load`, `iop_module_loader.cpp:435`), or a failed
   relocation. Computed placement of the 14 loads ends at `0x9BD00`, below `HeapBase 0x120000`
   (`iop_memory.h:25`): no arena problem (**Measured** by replaying the loader's arithmetic on
   the modules). **An unsupported import can therefore never cause the retry loop at `0x136708`.**
4. **Start function.** Run by `callFunction` (`iop_emulator.cpp:418`) on a temporary CPU,
   outside any IOP thread, budget 2,000,000 instructions (`iop_emulator_const.h`). Blocking calls
   (`WaitSema`, `WaitEventFlag`) do not block there. `CreateThread`/`StartThread` only queue
   threads; they run later in `runCycles` (`iop_emulator.cpp:556`), driven by EE cycle accounting.
   `RpcLoop` (`sifcmd:22`) puts the thread to sleep (`iop_rpc.cpp:243-246`). **Measured.**
5. **Unresolved imports** bite at call time only (see "How the emulator decides", step 4).

### B. Blocker found while reading: the ELF is read from the wrong memory

**Measured.** `IopModuleLoader::readElfFromGuest` calls `IopHost::readGuest`
(`iop_module_loader.cpp:323-330`). The runtime's `PS2IopHostAdapter::readGuest`
(`ps2_iop_host.cpp:132-146`, through `guestRange` `:105-129`) reads **EE RDRAM** (`getMemPtr` on
`m_runtime.memory().getRDRAM()`), and `loadModuleBuffer` does not translate the address. The game
passes the **IOP** address it got from `sceSifAllocSysMemory` (`SIF.cpp:309-314` →
`PS2Runtime::allocateIopMemory`), after copying the module there with `sceSifSetDma`
(`SIF.cpp:693-719`, `writeIopMemory`). At that low address EE RAM holds something else, so the ELF
header check fails and **every** load returns -1: the loop at `0x136708` would never end, and
this is the first thing the next run would hit once the cdvd bind of doc 16 section 10.3 is passed.
The adapter already has `readIopMemory` (`ps2_iop_host.cpp:197`) and `normalizeIopAddress`
(`:211`). `docs/knowledge/16-boot-sequence.md` section 2 says "reads the ELF from IOP memory";
that statement is wrong for the code as it is (proposed correction in NOTES of the report;
that file is not edited by this task). The size of the fix is small (see point 1 of section D);
that a run then loads all 14 is **Unverified**.

### C. Per-module plan, in load order

Legend: (a) what the load does, (b) start function, (c) what the game needs afterwards,
(d) verdict, (e) lightest handling. Imports named by library:ordinal; Sony names only where
the ordinal is one the emulator handles (taken from its `case` comments) or marked
**Unverified** (no local SDK copy; names recalled from public SDK headers).

| # | Module | (a) load | (b) start function | (c) needed afterwards | (d) | (e) |
|---|---|---|---|---|---|---|
| 1 | `sio2man` | real code, ELF header valid, relocatable at a low address; returns id > 0 (**Measured**) | start `0x634`: registers library `sio2man` (64 exports), `dmacman:33/34` (handled, 0), creates a semaphore and a thread. All imports supported, none unsupported anywhere in the module (**Measured**) | No SID. Its 64 exports are called by `mtapman` (7), `padman` (7), `mcman` (5), through the export table (**Measured**, TSV `provider=module`). The EE never talks to it (**Measured**: no SID) | works (loads); useless for the port beyond being a provider. Its exports drive SIO2 hardware at `0x1F808200`, which has no model (plain map, `iop_memory.cpp`) | nothing. Do not try to model SIO2 |
| 2 | `mtapman` | real | start `0xb24`: `RegisterLibraryEntries`, `loadcore:27`, then `0x14b0` creates and starts a thread; imports `sifcmd` 14/17/19/22 (`InitRpc`, `RegisterRpc`, `SetRpcQueue`, `RpcLoop`, all handled) and `sio2man` 48, 51-56 (**Measured**). The thread registers `0x80000900` once it runs | The EE binds `0x80000900` from `0x255CA0` and calls RPC functions 1-6 (see section E). Needs `sio2man` loaded first (it is) | loads and binds. A real RPC call would reach real mtapman code that probes the tap through `sio2man`/SIO2 hardware; with no hardware model it most likely blocks or fails (**Unverified**: no run) | replace the `sceMtap*` functions by game overrides (section E); keep the load as is |
| 3 | `mcman` | real | start `0x178` returns 2; calls `RegisterLibraryEntries`, `secrman:4/5` (inline handlers), `modload:13`, `ioman:21`/`20` (`DelDrv`/`AddDrv`, device `mc`, handled). Device init at ops[0] `0xfc10` returns 0, so `AddDrv` succeeds and the card probe `0x6e8` is **not** run (**Measured**) | Its 23 exports are imported by `mcserv` (provider). The EE does not use memory cards at boot (`sceMcInit` not reached, doc 16 section 10) | works (loads) | nothing. If the card probe ever runs it reaches `cdvdman:24` at `0x20c8` (likely `sceCdReadClock`, **Unverified**); returning 0 there is harmless for a probe, but do not implement it now |
| 4 | `mcserv` | real | start `0x40`: `RegisterLibraryEntries`, one thread through `thbase:4/6`. No unsupported import (**Measured**) | Registers `0x80000400` once its thread runs. `sceMcInit` (`0x11BD58`) is not reached at boot | works; useful later for saves, where card access goes through `mcman` → `sio2man` hardware and will not work (**Unverified**) | nothing now; saves are a later task (HLE of the `sceMc*` calls, as doc 16 section 6 already says) |
| 5 | `padman` | real | start `0x3404`: creates threads via `0x7b40`. `sifman:32` at `0x6e4` is in function `0x360`, outside the start tree, and is handled with 0 anyway (**Measured**, see correction in step 2 above) | SIDs `0x80000100/101`, bound only by `scePadInit` `0x118590`, which is replaced (doc 16). The main processor does not talk to it | loads; its threads poll through `sio2man` (no hardware) and read nothing. Whether they spin (cost IOP budget) is **Unverified** | nothing. Pad input comes from the host (D9) through the replaced `scePad*` functions |
| 6 | `cdvdstm` | real | start `0x15dc`: `RegisterLibraryEntries`, `ioman:21`/`20`, `0x13b0`, `cdvdman:50` ×2 (`sceCdSC`, handled), intrman (**Measured**). Its unsupported `cdvdman` 39/40/46/48/49/61/114 are all outside the start tree | No SID. Nobody calls it (CD streaming goes through the replaced `sceCd*`) | works but useless | nothing. Do not implement the seven `cdvdman` ordinals; names: **Unverified** |
| 7 | `libsd` (pack 0000) | real | start `0xb0`: `intrman:17/18`, `RegisterLibraryEntries`, `0x1550`, `0x45a0` (SPU2 setup writing `0x1F900000-0x1FA00000`; **Unverified** whether it polls a status bit and spends its 2M budget) | Exports `libsd` 1.05 (34 functions) imported by `sdrdrv` (26), `modhsyn` (19), `sndfi` (10), `cri_adxi` (10). No SID | works; the sound path of D6. Behaviour depends on the SPU2 model (doc 13) | nothing at load; the SPU2 bridge is T-205-like work, not here |
| 8 | `libsd` (pack 1000) | real; loaded a second time | same start function; it registers a second `libsd` 1.05 export table. `findLibrary` (`iop_imports.cpp:135-149`) keeps the **first** one registered (a replacement needs a strictly higher version), so the first copy stays the provider | none | works; the second start may re-register interrupt handlers and threads, effect **Unverified** (the two copies are only known to have the same size) | nothing. Not skipping it: the game needs a positive id |
| 9 | `modhsyn` | real; type relocatable, entry offset 0 (**Measured**) | start at offset 0: `intrman:17`, `loadcore:6`, `intrman:18`. No `libsd` call in the start tree | exports `modhsyn` 1.04 (30), imported by the `tsnddrv*` modules the game does not load; the EE does not talk to it | works but useless | nothing |
| 10 | `modmidi` | real; entry offset 0 | `sysclib:27/8/29/36`, `intrman:17/18`, `loadcore:6` | exports `modmidi` (26), not used by a loaded module | works but useless | nothing |
| 11 | `modmsin` | real; entry offset 0 | `intrman:17/18`, `loadcore:6` | exports `modmsin` (10), not used by a loaded module | works but useless | nothing |
| 12 | `sdrdrv` | real | two functions in the tree: `0x0` (`intrman:17/18`, `loadcore:6`, `sysmem:14`, `sysclib`, `thbase:4/6`) and `0x284` (`loadcore:7`, `libsd:26/26/27`, `thbase:10/5`, `sifcmd:24/25`) (**Measured**). `libsd` must already be registered (it is: order 7) | SID `0x80000701`, bound by `sceSdRemoteInit` `0x119CC0` (doc 16 10.4). The EE keeps talking to it: all `sceSdRemote*` | works. The `sifcmd` ordinals 24/25 are handled; their meaning is **Unverified** | nothing |
| 13 | `cri_adxi` | real | start `0x57c` → `0x18c`: `stdio:4` (printf), `sysclib`, `loadcore:5`, `sifman:29`/`5`, `sifcmd:14`, `thbase:4/6` (thread) (**Measured**) | SID `0x90000200` (doc 16 section 6 correction), bound by `0x325E58`; its 10 `libsd` imports need orders 7/8 first. The EE keeps talking to it (ADX) | works; if the bind runs before this load, it waits (doc 16 10.4, **Unverified** order) | nothing |
| 14 | `sndfi` | real | start `0x104d0` → `0x10244` (`stdio:4`, `thbase:4/6` ×2, `loadcore:6`, calls `0xdda0`: `libsd:11/9/23/19`, i.e. SPU2 writes) and `0x103d4` (`thbase:10/5`, `sifcmd:24/25`, `loadcore:7`) (**Measured**) | SIDs `0x534E4446` ("SNDF"), `0x53465356` ("SFSV"), bound at `0x37A4A8`. The EE keeps talking to it | works; the `libsd` calls in start touch SPU2 hardware, so completion within 2M instructions is **Unverified** | nothing |

Not loaded by the game (never named in the executable, **Measured**, doc 16): `tsnddrv`,
`tsnddrvc`, `rtfssiop`. Their unsupported imports (`ioman` 4-8/28, `thmsgbx` 4-6/8/9,
`cdvdman:39`) do not matter.

Summary of (e): no module needs an import implemented in the IOP emulator before the next run.
The 34 "unsupported" imports are all outside the start trees or handled (section A, item 3, and
the correction under "What the unsupported imports affect"). What a run must still confirm: that
every start function finishes within its budget, because several (`libsd`, `sndfi`) write SPU2
registers.

### D. Changes before the next run, in boot order

| # | Change | Where (AGENTS.md table) | Size | Certain / run must confirm |
|---|---|---|---|---|
| 1 | Make a buffer load read the ELF from **IOP RAM**. Either `IopModuleLoader::readElfFromGuest` uses `IopHost::readIopMemory` when `isIopMemoryRange(address, ...)`, or the adapter's `readGuest` falls back to the IOP range | a patch in `patches/ps2recomp/` (the runtime / IOP emulator row) | about 10-20 lines | Certain that the current code reads EE RAM (source, section B). A run confirms that the 14 loads then return > 0 and the loop at `0x136708` ends. Keep EE reads for module argument buffers |
| 2 | Bind `0x11A7A8` (`sceCdSearchFile`) to the runtime handler, as in doc 16 10.3 | `src/game/don_overrides.cpp` | one call | Certain as to the fit (doc 16). Reached at step 9 of `0x136770`, before any module load |
| 3 | Game overrides for the multitap functions (section E): `0x255CA0`, `0x255DC0`, `0x255E30`, `0x255EA0` | `src/game/don_overrides.cpp` | 4 small functions | Certain about the return values from the callers (section E). A run confirms that `0x138DC0` and the per-frame pad poll `0x138FB0` pass |
| 4 | Nothing else for the 14 loads | | | |

Change 1 is written (2026-10-11): patch `0004-iop-load-module-buffer-memory`, in the series. The
bytes were followed first (**Measured**, T-016 iteration 2a): `sceSifAllocSysMemory` returns an
address in the emulator's own RAM (the first one is `0x120000`) and `sceSifSetDma` copies the
module there correctly, so the copy needs no change and only the read was wrong. The patch makes
`loadModuleBuffer` read the ELF from IOP RAM first and fall back to the host read only when no
valid ELF is there. `tests/runtime/module_buffer_load_test.cpp` fails on the unpatched emulator
(module id -1) and passes with the patch. Still to be confirmed by a run: that the fourteen real
modules load through the real adapter and that their start functions finish.

Order of events at boot: (1) before the module loop; (2) at step 9 of `0x136770` (module pack
`0000`) before any load; (3) at `main` step 3, after the 7 loads of pack `0000`.

### E. Multitap: what each function must return

**Used by the port (2026-10-13):** the four replacements of this section exist in
`src/game/don_overrides.cpp` (`donMtapInit`, `donMtapPortOpen`, `donMtapPortClose`,
`donMtapGetConnection`, bound at `0x255CA0`, `0x255DC0`, `0x255E30`, `0x255EA0` in `applyDon`) and
return exactly the values of the last column (1, 1, 1, and 1 with `*a = *b = 0`); the four-pad
mode is asked for only with `--players 4` (T-306): then `donMtapGetConnection` writes `*a = 1` for port 0 and
scePadGetState answers 99 for a slot not opened (patch 0061), which is what the poll (0x138FB0) needs to open slots 0 to 3. The `mtapman` and `sio2man` modules are still loaded, and the game
does not make an RPC to `mtapman` any more through these four functions. See also
[05](05-audio-video-input.md), "Gamepad".

All **Measured** by decompiling `0x255C40`-`0x2560C3` and their callers (`tools/re/callers.py`).
The functions are thin RPC wrappers on the client at `0x53F2C0` (`sceSifCallRpc` `0x1101E8`,
buffer `0x53F300`, 0x80 bytes, synchronous). Function numbers: 1 PortOpen (`0x255DC0`),
2 PortClose (`0x255E30`), 3 `0x255F80` (no callers), 4 ChangeThreadPriority (`0x255FF0`, no
callers), 5 GetModVersion (`0x256060`, one caller `0x255D10`), 6 GetConnection table fill
(`0x255C40`, callers `0x255D80`, `0x255DA8`). Names beyond these numbers are **Unverified**.

| Function | Address | What the game does with the result | Return for one pad on port 0 |
|---|---|---|---|
| `sceMtapInit` | `0x255CA0` | calls `sceSifInitRpc(0)`, binds `0x80000900` (retries on `server == 0`), calls `0x256060` and requires `(version >> 4) == 0x31` (mtapman 3.16), then `0x255C40(0x53F380)` which makes mtapman fill the 0x80-byte table at `0x53F380`. Returns 1 on success, 0 on failure. The only caller `0x138DC0` ignores the result | return 1; the table at `0x53F380` only matters to `0x255EA0` (overridden) |
| PortOpen | `0x255DC0` | RPC 1 with port in `0x53F300`; returns the reply word 1 (non-zero = opened). Called with 0 and 2 from `0x138DC0`, results ignored | return 1 |
| PortClose | `0x255E30` | RPC 2; returns reply word 1; called with 0 and 2 from `0x138CB0` (shutdown) only when the 4-pad mode flag `0x4D69E8` is 1, result ignored | return 1 |
| GetConnection | `0x255EA0` | `(port, *a, *b)`: copies the table at `0x53F380` under a cache/interrupt guard, returns 0 if the first word is zero, else stores table words `+0x18+port*4` to `*a` and `+0x28+port*4` to `*b`, returns 1. One caller: the per-frame pad poll `0x138FB0` at `0x139024`, with port 0; `*b` is not used afterwards (**Measured**) | return 1, `*a = 0`, `*b = 0` |
| `0x255F80`, `0x255FF0` | | no callers | not needed |

Why `*a = 0` for one pad: in `0x138FB0`, return 0 makes the poll skip the whole update (no pad
input); `*a != 0` with the mode flag `0x4D69E8 == 0` takes the branch that opens port 0 **slots
0-3** (`0x1189E8(0, slot, ...)`) and sets `0x4D69E8 = 1`, `0x4D69F8 = 4` (four pads); `*a == 0`
with the flag set goes back to two ports (`0x4D69F8 = 2`). So with `*a = 0` the game stays in the
normal two-port mode and reads port 0 slot 0 (and port 1 slot 0), through `scePadRead` `0x139390`.

**Four players** need `*a != 0` for port 0 (any non-zero value works for the game code; the exact
value mtapman would give is **Unverified**), PortOpen(0) returning 1, and the pad layer
(`0x118EB0` = pad state query, `0x1189E8` = `scePadPortOpen`, `0x139390`) answering for port 0,
slots 0 to 3. `*b` stays 0 (unused). So the multitap replacement can be a function of one boolean
"four pads mode" from the host input layer, no IOP involved. The decision to enable it belongs to
T-306 (doc 13).

This is not "returning success to hide a failure" (rule 4): the real mtapman needs SIO2
hardware the port does not model and the game only consumes three values from it, which are
known above. The replacements are reported in the journal as a known divergence.

## How to repeat the measurements

From the repository root (the packs are game files, read in place, never copied):

```bash
# list of modules, exports and imports (decompressed in memory only)
uv run --no-project python -I tools/re/irx_imports.py disc/MODULES/0000.FPK disc/MODULES/1000.FPK

# the TSV of this document (supported = yes / module / no)
uv run --no-project python -I tools/re/irx_imports.py disc/MODULES/0000.FPK disc/MODULES/1000.FPK --tsv \
    > docs/knowledge/data/irx_imports.tsv

# counts: total, per support value, unsupported per module
tail -n +2 docs/knowledge/data/irx_imports.tsv | wc -l
tail -n +2 docs/knowledge/data/irx_imports.tsv | cut -f7 | sort | uniq -c
awk -F'\t' '$7=="no"{print $1}' docs/knowledge/data/irx_imports.tsv | sort | uniq -c
awk -F'\t' '$7=="no"{print $4":"$6}' docs/knowledge/data/irx_imports.tsv | sort -u

# test with a synthetic module
uv run --no-project --with pytest python -I -m pytest tests/test_irx_imports.py -q
```

`supported` is computed from the emulator source in
`third_party/PS2Recomp/ps2xIOP/src/emulator/`; if that source changes (new PS2Recomp commit or
patch), regenerate the TSV and re-read the dispatch chain, since the script depends on the shape
`switch (ordinal) { case N: ... default: return false; }`.
