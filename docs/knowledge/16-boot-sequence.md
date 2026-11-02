# 16 · Static map of the boot sequence

Task T-028. Everything here comes from reading the executable (`SLPS_256.75`, load base
`0x100000`) and the PS2Recomp runtime. **Nothing was run.** Markers: **Measured** (read in the
executable or the runtime source), **Declared** (stated by a project document or a library
string), **Unverified** (static reading cannot settle it; the entry says what would).

Conventions: game addresses are EE virtual addresses. Runtime references are
`file:line` under `third_party/PS2Recomp/ps2xRuntime/src/lib/` (`Kernel/Syscalls/` is
shortened to `Sys/`). "Replaced" means the function is a stub in `config/stub_policy.toml`
(doc [11](11-stub-policy.md)) or a by-name wrapper; "recompiled" means it runs as game code.
Reachability was computed with a call-graph closure from `entry` and `main` that does not enter
replaced functions. Indirect calls (function pointers, interrupt handlers, thread entries) are
invisible to it, so "not reached" never means "not used".

## 1. Call sequence

Execution order, not address order.

| Step | Address | What it does | Marker |
|---|---|---|---|
| entry | `0x100008` | clears bss `0x4C9D80`–`0x5A6800` | Measured |
| | `0x1001C8` | syscall `0x3C` SetupThread: gp `0x4D1470`, stack `-1`, size `0x80000`, exit function `0x100220` | Measured |
| | `0x1001E4` | syscall `0x3D` SetupHeap: base `0x5A6800`, size `-1` | Measured |
| `_InitSys` | `0x116618` (called at `0x1001E8`) | see below | Measured |
| | `0x10C520` | `FlushCache` | Measured |
| | | `EI`, then `main` `0x132280` (at `0x100208`), then `exit` `0x127028` | Measured |

`_InitSys` `0x116618` calls, in order: `0x1163C8` (two `CreateSema`), `0x116508`
(`InitSystemCallTableAddress`; uses syscalls `0x74` and `0x83`), `0x116AF8` (`InitAlarm`,
replaced), `0x116C70` (`InitTimer`, `AddIntcHandler` at `0x10BF90`), `0x116EB0`
(`StartTimerSystemTime`), `0x10D078` (`InitThread`: `CreateSema`, `CreateThread`,
`StartThread`, `ChangeThreadPriority`), `0x116740` (`InitExecPS2`; syscalls `0x5A`, `0x5B`,
`0x74`, `0x4A`, `0x4B`), `0x115C30` (`InitTLBFunctions`), then a tail jump to `0x118330`.

`main` `0x132280`, in order:

| # | Call | Purpose | Marker |
|---|---|---|---|
| 1 | `0x1365C0(0)` | malloc arenas (section 9, item 1) | Measured |
| 2 | `0x136770` | IOP reset, GS init, modules of pack `0000` (section 2) | Measured |
| 3 | `0x138DC0` | `sceMtapInit` `0x255CA0`, `scePadInit` `0x118590`, VBLANK handler, `scePadPortOpen` `0x1189E8` (section 4) | Measured |
| 4 | `0x136D40` | engine init; `AddIntcHandler` at `0x136D54`; many calls to `0x36CE20` | Measured |
| 5 | `0x13A520`, `0x137F70`, `0x13A250`, `0x133230` (empty), `0x1CA870` | engine and state tables | Measured |
| 6 | `0x1364A0(1)` | CRI/Sofdec setup | Measured |
| 7 | `0x1345B0("modules/1000.fpk",1)` | loads pack `1000` from disc | Measured |
| 8 | `0x151E60(1,100,100,0)` | sound chain: IRX of pack `1000`, `sceSdRemote`, ADX, SNDF, banks (section 2) | Measured |
| 9 | `0x151DE0`, `0x36ACB0` | `0x36ACB0` calls `0x347B40`, MWSFD init ("MWSFD/PS2EE Ver.3.50") | Measured |
| 10 | `0x151E60(1,100,100,1)`, `0x134360(pack)` | volumes; free pack `1000` | Measured |
| 11 | `0x14C4B0(0x360)`, `0x149EA0`, `0x142E00(0x4D9380,8)`, `0x2477D0`, `0x132070`, `0x132000`, `0x131FA0`, `0x1DA5C0`, `0x150F50(0x13)` | engine subsystems | Measured |
| 12 | `0x132470(0)` | first loop: opens `fpack/cmn/0000.fpk` (string `0x4B63C0`) through `0x1345B0`; per-frame set `0x138B00`, `0x138ED0`, `0x13A180`, `0x135C10`, `0x136190`, `0x1F8CC0`, `0x1FBF80`, `0x135EC0`, `0x135C40`, `0x13E680`, `0x1FD2B0`; exits when `iRam004d78fc` is negative | Measured |
| 13 | state machine | sets state `0x4D78F4 = 3`; while state is not `0xE`: state `0xC` calls `0x38E910` and sets state 0; states 1, 2, 3 call `0x131CA0(0)` | Measured |
| 14 | after the loop | `0x2C5118`, `0x138CB0` (`RemoveIntcHandler`, `DisableIntc`), `0x136D00`, `0x136760`, `exit` `0x127028` | Measured |

What each state does is **Unverified** (needs a trace of `0x4D78F4` at run time).

## 2. I/O processor

All in `0x136770`, called from `main` (step 2).

| Order | Call | Address | Status in the port |
|---|---|---|---|
| 1 | `sceDevVif0Reset`, `sceDevVif1Reset`, `0x102980`, `0x102EB8`, `sceGsResetPath` | `0x1004F0` | GS ones replaced |
| 2 | `sceDmaReset` | `0x1022A0` | replaced |
| 3 | `sceSifInitRpc` | `0x10F860` | replaced |
| 4 | `sceCdInit` | `0x11AFD0` | replaced |
| 5 | `sceSifRebootIop("cdrom0:\MODULES\IOPRP300.IMG;1")` in a loop until non-zero | `0x1159D0`, string `0x4B6630` | replaced: `Stubs/SIF.cpp:560` resets the IOP and returns 1 |
| 6 | `sceSifSyncIop` in a loop until non-zero | `0x115980` | replaced: `Stubs/SIF.cpp:838` returns 1 |
| 7 | GS: `sceGsResetGraph` `0x100350`, `sceGsSetDefDBuff` `0x100EE0`, `FlushCache`, `sceGsSyncV` `0x1011D8` (call at `0x136834`), `sceGsPutDrawEnv` `0x100DF8`, `sceGsSyncPath` `0x101270` | | replaced |
| 8 | `sceSifInitRpc`, `sceSifLoadFileReset` `0x114C30`, `sceFsReset` `0x1113C0`, `0x133D60(1)` (file mode 1 = CD), `sceSifInitIopHeap` `0x114550` | | replaced (`Stubs/SIF.cpp:545`, `:519` returns 0) |
| 9 | `0x1345B0("modules/0000.fpk",1)`, then `0x136670` per module, then `0x134360` | | recompiled |

Reset image: **Measured** the string is `cdrom0:\MODULES\IOPRP300.IMG;1` at `0x4B6630`.
The runtime does not read it; `sceSifRebootIop` only resets its IOP emulator
(**Measured**, `Stubs/SIF.cpp:560`). Whether the stock IOP modules of that image (loadcore,
sifman, ...) are needed by the emulated IRX is covered in doc [14](14-iop-modules.md).

Modules, in load order (strings `0x4B6670`..`0x4B6730`, form `modules/NAME.irx`):

| Pack | Order | Module | Loaded from |
|---|---|---|---|
| `0000` | 1–7 | `sio2man`, `mtapman`, `mcman`, `mcserv`, `padman`, `cdvdstm`, `libsd` | `0x136770` |
| `1000` | 8 | `libsd` (second time) | `0x151E60` via `0x136670` (strings `0x4B85B0`..`0x4B8630`) |
| `1000` | 9–12 | `modhsyn`, `modmidi`, `modmsin`, `sdrdrv` | same |
| `1000` | 13 | `cri_adxi` | `0x331C60` via `0x136670(0x4C3F30,9,0x4C3F48)` |
| `1000` | 14 | `sndfi` | `0x37A460` |

`tsnddrv`, `tsnddrvc` and `rtfssiop` are never named in the executable (**Measured**, string
search), so the game does not load them by name.

Module loader `0x136670` (**Measured**):

1. size from `0x133260`, load from the already loaded pack through `0x1336D0`;
2. `sceSifAllocSysMemory` `0x114648` (replaced; `Stubs/SIF.cpp:309`, size from `$a1`) gives an
   IOP buffer;
3. `FlushCache`, then a DMA descriptor on the stack sent with `sceSifSetDma` `0x10C660`, polled
   with `sceSifDmaStat` `0x10C640` until it returns negative. The replaced `sceSifDmaStat`
   always returns `-1` (`Stubs/SIF.cpp:326`), so the poll ends at once;
4. loop `0x115238(iopaddr, 0, 0)` at `0x136710` until the result is `>= 0`. `0x115238` tail-calls
   `0x114C68`, which is `_sceSifLoadModuleBuffer` and **is replaced**
   (`Sys/System.cpp:288`);
5. `0x114740` frees the IOP buffer (recompiled; reaches the replaced `sceSifCallRpc`
   `0x1101E8` through `0x1146C8`; no `sceSifBindRpc` was done for that client, result ignored);
6. `0x138780` frees the EE buffer.

**Q-LOAD closed (Measured):** the wrappers `0x115238`/`0x115240`/`0x115258`/`0x115260` do not
need binding: they all end in the replaced `0x114C68`. `_sceSifLoadModule` `0x115278` has two
callers (`0x1154A0`, `0x1154C0`) and those have no callers, so the game does no
`sceSifLoadModule` by path. The handler (`Sys/System.cpp:288`) reads `$a0` buffer, `$a1`
argument size, `$a2` argument address; it returns the module id in `$v0` or `-1`; it does not
write through `$a3` (the game leaves `$a3` unset; harmless). `loadModuleBuffer`
(`ps2xIOP/src/iop_emulator.cpp:655`) reads the ELF at the guest address. (Corrected on 2026-10-11: this first said "from IOP memory". Reading the runtime's adapter, `ps2_iop_host.cpp:132`, the read goes to the main processor's memory, while the game passes an address in IOP memory; if that is right every load fails and the loop at `0x136708` never ends. Found by reading and then shown by a test that fails on the unpatched emulator, `tests/runtime/module_buffer_load_test.cpp`; fixed by patch `0004-iop-load-module-buffer-memory`. Not yet seen in a run of the game: see "Load plan for the port" in [14-iop-modules.md](14-iop-modules.md).)
If the handler returns a negative value, the loop at `0x136710` never ends (section 9).

## 3. Files opened, in order

File layer (**Measured**): mode variable `0x4D6860`, set by `0x133D60(mode)`. Mode 1 = CD
(`0x133070`: `sceCdInit`, `sceCdMmode(2)`); mode 0 = `host0:data/` through `sceOpen` (strings
used by `0x14A060`..`0x14A6F0`); mode 4 = ADXF. Size is `0x133260`, load is `0x1336D0`. In
mode 1, `0x132850` upper-cases the name, builds `\...;1`, calls `sceCdSearchFile` `0x11AAA8`
(recompiled; it calls `sceCdLayerSearchFile` `0x11A7A8`, which binds `0x80000597` and is where the first run stalls, section 10 (corrected on 2026-10-11 after the first run, journal 002)) and takes the size as sectors × `0x800`. `0x1330F0` returns `-2` (AFS lookup stub).
`0x133D60` also builds `atfile:%s!8420,/battle/data/` (string `0x4B6550`). `0x1345B0` appends
`.fpk`, checks the pack table (`0x4D6880`, entries `0x88` bytes), else loads the whole file
(`0x133520`) and registers it.

| # | File | Opened by | Interface |
|---|---|---|---|
| 1 | `IOPRP300.IMG` | `0x136770` | not read by the runtime (reset only) |
| 2 | `modules/0000.fpk` | `0x136770` | `0x1345B0` → CD, `sceCdSearchFile` + `sceCdRead` |
| 3 | `modules/1000.fpk` | `main` step 7 | same |
| 4 | `dir.dir` and the four AFS (`movie.afs` string `0x4C3F80`, others in the same table) | `0x331C60` | ADXF through `cri_adxi` (file mode 4) |
| 5 | `snd/se/ps2/system.dat`, `battle.dat` | `0x151C10` via `0x37A2A0`/`0x37A380` | SNDF (`sndfi` RPC) |
| 6 | `fpack/cmn/0000.fpk` | `0x132470` (string `0x4B63C0`) | `0x1345B0` |
| 7 | the pack at string `0x4B6370` | `0x131CA0` (`0x4D67E0 = 0x1345B0(0x4B6370,1)`) | `0x1345B0` |
| 8 | a file at string `0x4B6388` | `0x131CA0` through `0x1B1540` | **Unverified** (reads in `0x1B1540` not followed) |

Rows 2, 3, 6 and 7 go through `sceCdRead` (replaced, `sceCdSync` replaced). That the runtime
reads the disc image at the sector `sceCdSearchFile` returns is **Unverified** (needs a run).
`host0:` is used by `0x14A060`..`0x14A6F0`; not reached from `main` by the closure
(**Unverified** whether any boot path chooses mode 0).

## 4. Threads, interrupts and callbacks created during boot

| What | Created at | Entry / handler | Notes | Marker |
|---|---|---|---|---|
| Thread "SceKerneltopThread" | `0x10D0F8` (`InitThread` `0x10D078`) | `0x10CFA0`, stack `0x4CA380`, size `0x400` | Sony init thread; started through replaced `StartThread` `0x10D3E0` | Measured |
| Semaphores | `0x1163C8` (2), `0x10D078` (1), more in CRI | | `CreateSema` `Sys/Sync.cpp:69` | Measured |
| INTC handler (timer) | `0x10BF90` in `0x116C70` | timer code `0x117150`–`0x1182B8` (recompiled) | | Measured |
| INTC cause 3 (VBLANK start) | `0x138DFC` in `0x138DC0` | `0x138EE0` (listed in the function CSV as `entry_00138EE0`) | increments `0x4D6A08`, then `EI`, returns 0 | Measured |
| INTC handler | `0x136D54` in `0x136D40` | **Unverified** (cause and handler not read) | | |
| `sceGsSyncVCallback` | `0x331D34` in `0x331C60`; also `0x36A8C4` (movie player) | callback `0x331F20` | the callback calls `0x315910(0)`, `SYNC`, `EI`, returns 0 (ADX vsync tick) | Measured |
| CRI threads (4) | `0x314BE0`, `0x314D40`, `0x314DC8`, `0x314E50` | entries `0x314568` (stack `0x3D2410`, `0x800`), `0x314658` (`0x3D3C10`, `0x1000`), `0x314760` (`0x3D4C10`, `0x1000`), `0x314850` (`0x3D5C10`, `0x2000`) | created by `0x314FF0` inside `0x331C60`; each is followed by `StartThread` (replaced), the first by `SuspendThread` `0x314B80`; priorities from `0x3D22F4` etc. (defaults `0x19, 0x18, 1, 8, 0x10, 0x12`, `0x314FF0`) | Measured |
| EE-side RPC server thread | **Unverified** who creates it | `0x3258B8` (handler `0x325808`) | registers SID in `0x3DE484` (initial value `0x90000200`) with `sceSifRegisterRpc` `0x1104C0`, then `sceSifRpcLoop` `0x1108D8` | Measured (body) |
| Alarms | `0x117xxx`–`0x118xxx` | | syscalls `0xFC`, `0xFD`, `-254`, `-255` (section 5) | Measured |

The 4 CRI threads depend on `CreateThread` (`Sys/Thread.cpp:188`) reading the parameter block
the game builds on its stack (entry, stack, size, gp, priority). The thread functions are
recompiled; whether they run correctly under the runtime's scheduler is **Unverified**
(a run with `DON_TRACE_MODULES=1`-style tracing would show them).

Pad, mtap: `0x138DC0` calls `sceMtapInit` `0x255CA0`, `0x255DC0(0)`, `0x255DC0(2)`,
`scePadInit(0)` `0x118590` (replaced), then `scePadPortOpen` `0x1189E8` twice (`iRam004d69f8 = 2`).
Risky unlisted functions noted in doc 11: `scePadSetDmaCallback` `0x119B68`,
`sceMtapChangeThreadPriority` `0x255FF0`, `sceMtapGetModVersion` `0x256060`.

## 5. System calls

Reached by the boot call graph (entry + `main`, 1229 functions). Source of numbers: the
`li $v1` before each `syscall` (168 `syscall` instructions in the executable). Dispatcher:
`Sys/Dispatcher.cpp:7`, a `switch` on `$v1`; unknown numbers go to `PS2Runtime::handleSyscall`
(`ps2_runtime.cpp:1458`) and then `ps2_syscalls::TODO` (`Sys/System.cpp:326`), which logs
`[Syscall TODO]` and `Unknown syscallId=0x.. hits=` and returns 0.

| No. | Name | Wrapper(s) | Handler | Reached at boot | Marker |
|---|---|---|---|---|---|
| `0x10` / `0x11` | AddIntcHandler / RemoveIntcHandler | `0x10BF80`, `0x10BF90` / `0x10BFA0` | `Sys/Interrupt.cpp:118` / `:128` | yes | Measured |
| `0x14` / `0x15` | EnableIntc / DisableIntc | `0x10BFE0` / `0x10BFF0` | `Sys/Interrupt.cpp:98` / `:108` | yes | Measured |
| `0x20` | CreateThread | `0x10C0A0` | `Sys/Thread.cpp:188` | yes (5 uses) | Measured |
| `0x22` | StartThread | `0x10C0C0` | dispatcher | only through replaced `0x10D3E0` | Measured |
| `0x23` | ExitThread | `0x100224` (exit function `0x100220`) | dispatcher | at thread end | Measured |
| `0x29` | ChangeThreadPriority | `0x10C130` | `Sys/Thread.cpp:408` | yes | Measured |
| `0x2F` | GetThreadId | `0x10C190` | `Sys/Thread.cpp:329` | yes | Measured |
| `0x30` | ReferThreadStatus | `0x10C1A0` | `Sys/Thread.cpp:334` | yes | Measured |
| `0x37` | SuspendThread | `0x10C210` | `Sys/Thread.cpp:313` | yes | Measured |
| `0x3C` | SetupThread | `0x1001C8` | `Sys/System.cpp:473` (stack `-1` gives `sp = RAM_SIZE - size`) | yes | Measured |
| `0x3D` | SetupHeap | `0x1001E4` | `Sys/System.cpp:538` | yes | Measured |
| `0x3E` | EndOfHeap | `0x10C280` (via `0x10C9A0`, `0x124FB0`) | `Sys/System.cpp:582` | yes | Measured |
| `0x40`/`0x41`/`0x42`/`0x44`/`0x45` | CreateSema / DeleteSema / SignalSema / WaitSema / PollSema | `0x10C2A0`/`0x10C2B0`/`0x10C2C0`/`0x10C2E0`/`0x10C2F0` | `Sys/Sync.cpp:69`/`:89`/`:99`/`:109`/`:114` | yes | Measured |
| `0x4A` / `0x4B` | SetOsdConfigParam / GetOsdConfigParam | `0x10C340` / `0x10C350` (via `0x1166D8`) | `Sys/System.cpp:120` / `:94` | yes | Measured |
| `0x5A` | Copy | `0x115BC8`, `0x116438`, `0x116680`, `0x1167F0`, `0x116AA0` | `Sys/System.cpp:951` | yes (kernel patch) | Measured |
| `0x5B` | (RFU091, patch) | `0x115C10`, `0x1166C8`, `0x116AE8` | dispatcher | yes | Measured |
| `0x64` | FlushCache | `0x10C520` | `Sys/Thread.cpp:153` | yes | Measured |
| `0x74` | SetSyscall (RFU116) | `0x115C20`, `0x116608`, `0x116670`, `0x116830`, `0x116A94` | `Sys/System.cpp:462` | yes | Measured |
| `0x76` / `0x77` | sceSifDmaStat / sceSifSetDma | `0x10C640` / `0x10C660` | replaced by name | yes | Measured |
| `0x83` | FindAddress | `0x1164C0` | `Sys/System.cpp:791` | yes | Measured |
| `0xFC` / `0xFD` | SetAlarm / ReleaseAlarm | `0x10C030` is ReleaseAlarm; SetAlarm wrapper in the same block | `0xFC` mapped (`Sys/Sync.cpp:333`); **`0xFD` has no case** | **Unverified** | Measured (gap) |
| `-254` / `-255` | iSetAlarm / iReleaseAlarm | `0x10C080`, `0x10C090` | dispatcher maps iSetAlarm to `-0x1E`/`-0xFD`, iCancelAlarm to `-0x1F`/`-0xFF`; **`-254` has no case** | **Unverified** | Measured (gap) |
| `0x04` | Exit | `exit` `0x127028` path | special-cased in `handleSyscall` (`ps2_runtime.cpp:1458`) | at `exit` | Declared |

The dispatcher has no case for these numbers (`Sys/Dispatcher.cpp:7`): `0x00`, `0x03`,
`0x05`–`0x0F`, `0x27`, `0x28`, `0x3B` (JoinThread), `0x3F`, `0x49`, `0x4C`–`0x4F`, `0x55`,
`0x58`, `0x60`, `0x63`, `0x66`, `0x6B`–`0x6D`, `0x72`, `0x75`, `0x7B`, `0x7D`, `0x7E`, and the
negative variants. Of these, none is reached at boot except the alarm gap above. The
alarm numbers matter because the game's alarm wrappers (`0x10C020`..`0x10C090`) use `0xFC`,
`0xFD`, `-254`, `-255` while the dispatcher maps `0x18`/`0xFC`, `0x19`/`0xFE`, `-0x1E`/`-0xFD`,
`-0x1F`/`-0xFF`. What settles it: any `Unknown syscallId=0xfd hits=` or `0xfffffe02` line in
the log (T-015). The full census (168 instructions with function, name, address, number) is
reproduced by section "How to repeat".

## 6. Remote (SIF RPC) services

`sceSifBindRpc` `0x110008` and `sceSifCallRpc` `0x1101E8` are replaced
(`Sys/RPC.cpp:254`, `:342`). A bind publishes a server only if an EE server is registered
(`Sys/RPC.cpp:289`) or `canBindIopRpc(sid)` is true (`ps2_runtime.cpp:590`), that is, an HLE
route exists or the emulated IOP has an RPC server for the SID
(`ps2xIOP/src/iop_subsystem.cpp`, `canBindRpc`). HLE routes: MCSERV `0x80000400`/`0x80000480`,
LIBSD `0x80000701`, DBCMAN `0x80001300`; they are active only after a recognised module load.

Correction (corrected on 2026-10-11 after the first run, journal 002): the HLE routes are effectively dormant for this game. Modules loaded through
`_sceSifLoadModuleBuffer` (`Sys/System.cpp:288`) go only to `emulator.loadModuleBuffer` and run
as real IOP code; `rebuildRoutes` is not called, so MCSERV, LIBSD and DBCMAN routes never
activate. A SID is bindable only when the real module has registered it through sifcmd ordinal
17 (`hasServer`, `iop_rpc.cpp:344`), and that happens only when the IOP thread has run, which
advances only inside `EeScheduler::accountCycles` (`EeScheduler.cpp:396`). **Measured** (code
reading). Section 10 lists every bind and who answers it.
Unknown SIDs stay unhandled and the client's `server` field stays 0.

SIDs bound by the game (the constant in `$a1` at each `jal 0x110008`):

| SID | Caller (call address) | Service | Reached at boot | Provider in the port | Marker |
|---|---|---|---|---|---|
| `0x80000001` | `0x111128` (`sceFsInit`, `0x1111E4`) | fileio | no | none needed | Measured |
| `0x80000003` | `0x114550` (`0x11459C`, `sceSifInitIopHeap`) | IOP heap | the function is replaced | none | Measured |
| `0x80000006` | `0x114AA0` (`0x114AD4`, `_lf_bind`) | load file | no (replaced `sceSifLoadFileReset`) | none | Measured |
| `0x80000100`, `0x80000101` | `0x118590` (`0x1185F4`, `0x118648`, `scePadInit`) | pad | function replaced | `padman` IRX (doc 14: `sifman` 32 imports unsupported) | Measured |
| `0x80000400` | `0x11BD58` (`0x11BE40`, `sceMcInit`) | memory card | no | HLE MCSERV | Measured |
| `0x80000900` | `0x255CA0` (`0x255CF4`, `sceMtapInit`) | multitap | yes, recompiled | `mtapman` IRX | Measured |
| `0x80000592`, `0x80000593`, `0x80000595`, `0x80000597`, `0x8000059A`, `0x8000059C` | `0x11AFD0`, `0x11ADC8`, `0x11AAC8`, `0x11A7A8`, `0x11B2B8`, `0x11B4D0` | CD commands | `0x11A7A8` and `0x11B4D0` reached through `0x132850`; `0x11AFD0` and `0x11B4D0` are replaced | `sceCd*` replaced; `0x11A7A8` is recompiled and binds `0x80000597`; **nothing serves it** (section 10) | Measured |
| `0x80000701` | `0x119CC0` (`0x119CF4`, `sceSdRemoteInit`) | libsd | yes, recompiled | HLE LIBSD or `libsd` IRX | Measured |
| `0x534E4446` ("SNDF") | `0x375E78` (`0x375EC8`) | sound effects | yes | `sndfi` IRX | Measured |
| `0x53465356` ("SFSV") | `0x375E78` (`0x375F3C`) | sound file server | yes | `sndfi` IRX (**Unverified** which IRX exports it) | Measured |
| `0x90000200` (initial value of `0x3DE484`) | `0x325E58` (`0x325E9C`) | CRI EE-side service | yes (path `0x36ACB0` → `0x347B40` → `0x3134F8` → `0x316428` → `0x32C320`) | the real `cri_adxi` IRX registers `0x90000200` on the IOP; `0x3258B8` is not shown to run | Measured (IRX), Unverified (EE thread) |

`0x325E58` handles bind failure: if the bind returns negative it calls the error function
`0x3161C8` with a message and spins; if `client->server` (offset `0x24`) is 0 it spins and
retries the bind (`0x325EC8`..`0x325F0C`). That is the SDK retry signal the runtime already
mimics (`Sys/RPC.cpp:298`). The EE-server thread `0x3258B8` must therefore have registered the
SID before this bind can succeed. Who starts that thread is **Unverified** (no
`lui`/`addiu` reference to `0x3258B8` and no pointer to it in the image; the creator uses an
indirect form). What would settle it: log `CreateThread` entries at run time.

Correction (corrected on 2026-10-11 after the first run, journal 002): the SID `0x90000200` is registered by the
`cri_adxi` IRX itself on the IOP (its RegisterRpc constant is taken from a global that holds
`0x90000200`; **Measured** by scanning the IRX), so the bind at `0x325E58` can succeed once
`cri_adxi` is loaded and its start thread has run. The statement above that an EE server
`0x3258B8` must register the SID is therefore not required for the bind; and if an EE server did
register the same SID it would shadow the IOP one (`g_rpc_servers` is checked first,
`Sys/RPC.cpp:289`).

`cri_adxi`'s own SID is not bound by game code in the boot closure; it is set inside the IRX
(**Unverified** which SID and whether `ps2xIOP` exports it; doc 14 only says it has 0
unsupported imports). `sceSifCallRpc` use in the boot closure: `0x377E58` (SNDF/ADX service
loop, called every frame from `0x131CA0`), `0x1146C8` (heap free), and the callers behind
`0x11A7A8`/`0x11B4D0`.

Q-RPCLOOP: `sceSifRpcLoop` returns 0 (`Stubs/SIF.cpp:595`) and `SifCallRpc`
(`Sys/RPC.cpp:342`) dispatches an EE server directly through `eeScheduler().invokeCurrent`, so
EE servers do not need the loop. That the `0x325808` handler returns correctly is
**Unverified** (needs a run).

## 7. Opening video (input for T-113)

| Fact | Address | Marker |
|---|---|---|
| Table of video names, index 0 and 1 | `0x3F23C0` | Measured |
| Index 0 | `snd/movie/ps2/OP.sfd` (string `0x4C7410`) | Measured |
| Index 1 | `snd/movie/ps2/ED.sfd` (string `0x4C7430`) | Measured |
| Player | `0x36A820(name)` through `0x36AC70(index)` (`index < 2`) | Measured |
| Called from | script opcode `0x50` in the interpreter `0x1BC420` (no direct call; reached through a table) | Measured |
| MWSFD init | `0x36ACB0` → `0x347B40`, "MWSFD/PS2EE Ver.3.50", at boot step 9 | Measured |
| Player create | `0x3459B0` (`mwPlyCreateSofdec`) called at `0x36A970` | Measured |
| Files read through | `movie.afs` (ADXF, `0x4C3F80`) | Declared (doc 05: 2 videos in `MOVIE.AFS`) |

What `0x36A820` does (calls in order): `0x136950`, `sceGsResetPath`, `sceDmaReset`,
`sceGsResetGraph`, `sceGsSetDefDBuff`, `0x36A640`, `FlushCache`, `sceGsSyncVCallback`
(`0x36A8C4`), `0x123330`, `0x344A58`, `0x1386D0` (allocation), `0x3459B0` (create),
`0x348C50`, `0x315A70`, `0x34A140`; then a loop calling `0x138B00`, `0x315928`, `0x377E58`,
`0x346908`, and `sceDmaGetChan` / `sceGsSyncPath` / `sceDmaSend` (`0x102278`, `0x101270`,
`0x1025A8`) to push frames; it finishes with `0x347228` and `0x349C18`.

- **Measured:** the call-graph closure from `0x36A820` (735 functions) reaches no `sceMpeg*`
  and no `sceIpu*` function. `sceMpegCreate` `0x108FA8` and `sceIpuInit` `0x10BC38` are
  replaced and not reached from the player. So doc 05's "Sony libmpeg" is not on the path
  of these two videos.
  **Corrected 2026-10-13 ([19](19-videos.md) sections 1 and 2; journal 007, 015).** The
  closure follows only direct `jal`/`j` edges, and the CRI Sofdec core enters Sony's
  `sceMpeg*` through indirect calls (callbacks registered with `sceMpegAddCallback`). The
  game **does** call `sceMpegInit`/`Create`/`AddCallback`/`GetPicture`/`Reset` (call sites
  `0x35CF74`, `0x35D010`, `0x35D84C`, `0x35D490`, in the CRI cluster `0x356E38`-`0x35D768`),
  and those are the functions the runtime replaces (the exact call sites are in the table of
  doc 19 section 2). Journal 015 measured the proof the other
  way: the replaced `sceMpegCreate`/`sceMpegReset`/`sceMpegClearRefBuff` wrote the game's code
  (patch 0036, [13](13-runtime-host.md), "The patch series today").
- **Answered 2026-10-13 ([19](19-videos.md) section 2):** CRI's Sofdec decodes through Sony's
  `sceMpeg` with CRI callbacks that poke the IPU registers and DMA channels 3 and 4 themselves.
  The old "Unverified" question is settled by the register scan of doc 19.
- **The player is CRI Sofdec**: `0x3459B0` is `mwPlyCreateSofdec`, and the files are streamed from
  `MOVIE.AFS` by the CRI file layer. The port cannot decode the picture (no IPU model, FFmpeg
  off: doc 19 section 3).
- **The port's timeline against the console's (journal 007, update of 2026-10-13, T-114).** The
  console's own timeline (doc 19 section 4: logo 5-25 s, video 25-105 s, title about 110 s after the
  answer to the memory-card dialog) maps onto the port frame for frame: **frame = 330 + 60 x
  seconds** (**Measured**). The port's boot is the console's to the frame; only the video's
  picture is missing (black from about frame 2100).
- **The undecoded video never ends by itself (journal 007, T-114; Measured).** Without any
  button the picture is black from frame 2400 to 8400 (6000 frames, the console's whole video)
  and the title never appears: the CRI state machine does not reach its end-of-stream state
  (status 3) without decoded pictures, so the loop runs until Start. The log's only repeating
  line is the 16 KiB stream on DMA channel 9 (`0x1000D400`, a scratchpad channel, not SIF2; doc
  19 section 3). One `tap START 6` at frame 2400 gives the title at 2700; a second one 60 frames
  later gives the main menu at 3000 (`0x4D6A40 & 0x800` of doc 19 section 1 is reached and
  honoured).
- **What skipping does to the title (journal 023, 024; Measured).** Since journal 023 the port
  skips both videos by default: `0x36AC70` (the handler of script opcode `0x50`) is replaced in
  `src/game/don_overrides.cpp` (`donSkipMovie`) and returns 1 at once; `DON_PLAY_MOVIES=1`
  keeps the game's own player. Index 0 (OP, also the attract loop) and index 1 (ED) both pass
  through this one function, and `0x1BC420` ignores the result, so the game's own code goes on
  unchanged; what the player would leave behind (GS reset to 640x224, the VBLANK callback
  `0x331F20`) turned out not to be needed for the title to come up. The title then appears at
  about blank 2060-2100. **Left alone, the title animates until about blank 2460 and then the
  picture is byte-identical to the end of the run, and Start does nothing afterwards** (journal
  023). With `DON_PLAY_MOVIES=1` the title left alone for 3300 blanks did not freeze, so the
  skip is the suspect. Journal 024 measured that **the memory card is not the cause** (no
  card function is called after the boot dialog: 189 `GetInfo`/`Sync` pairs, all of the dialog;
  the polling seen at the end of a log is buffer order), that the game thread is alive (the
  per-blank counters at `0x3D1208`, `0x4C998C` advance by exactly the number of blanks, the ADX
  buffers are rewritten) and that the cause is **not determined**; a route that presses Start
  inside the title's animation (the committed scripts) is not affected.
- For T-113: hooking `0x36AC70(index)` to return at once is the smallest skip, and it is
  what the port did by default in journals 023 and 024. **Note 2026-10-13 (code, not a journal):**
  the working tree being edited by another task at the time of writing instead wraps the player
  `0x36A820` (`donVideoPlayer`, `src/host/video_skip.h`: the host holds Start for the opening
  and Circle for the ending while the player runs); the journals above describe the
  `0x36AC70` version. **Unverified** which one is committed; see the code.

## 8. Frame loop

- **Measured:** frame loop in `0x131CA0` (`do { ... } while (-1 < iRam004d78fc)`), shape:
  `0x138B00` (reads the VBLANK counter: `0x4D6A10 = 0x4D6A08; 0x4D6A08 = 0`), up to 2
  catch-up iterations of `0x13A180` and `0x1FD2B0` driven by the count in `0x4D6A10`
  (`0x138F10` returns it), then `0x138ED0` (clears `0x4D6A10`), `0x13A180` (per-object
  callbacks from the list at `0x4D7010`), `0x135C10`, `0x136190`, `0x1FBDF0`, `0x315928`
  (ADX), `0x377E58` (SNDF/RPC), `0x1FBF80` (render, calls `0x13E680`), `0x135EC0`, `0x135C40`,
  `0x13E680`, `0x1FD2B0`.
- **Measured:** the loop has **no blocking vsync call**. `sceGsSyncV` `0x1011D8`,
  `sceGsSwapDBuff` `0x101178`, `sceGsSwapDBuffDc` `0x1020C0` and `sceDmaSync` are not
  reachable from `0x131CA0`. Pacing is the VBLANK_START handler `0x138EE0` (INTC cause 3,
  registered at `0x138DFC`) incrementing `0x4D6A08`. If no vblank arrives, `0x4D6A10` stays 0
  and the loop spins at full speed instead of hanging.
- **Measured:** the only static `sceGsSyncV` callers are `0x136834` (boot, `0x136770`),
  `0x2DFA10` (`0x2DF720`), `0x2E0778` (`0x2E0760`), `0x37A308` and `0x37A3DC` (sound-bank
  loads). The engine ones (`0x2DFA10`, `0x2E0778`) are likely the frame flip and are reached
  through function pointers, so the closure cannot see them (**Unverified**). Direct GS
  privileged-register access (lui `0x1200`) is in `0x2D3FD0` (7 times), `0x36A130` and the
  SDK functions; `0x2D3FD0` has no direct caller and `0x2D5D28` (which calls
  `AddIntcHandler` at `0x2D5D4C`) is not reached from `main` or `0x131CA0` (**Unverified**
  who calls them; a run with a trace of `AddIntcHandler` would show it).
- **Reliable log sign for T-015 (proposed):** the count of calls to `0x138EE0` per second.
  It is about 60 (NTSC) when the runtime delivers VBLANK_START to the game's INTC handler; if
  it stays 0, the loop is running without pacing. A second sign, independent of the first: the
  first `sceGsSyncV` call from `0x2DFA10` or `0x2E0778` (replaced function, so the runtime
  can log it) and the first `sceDmaSend` from `0x1FBF80`'s path. The state variable
  `0x4D78F4` going from 3 to another value is a third sign that the first scenes were
  reached. All three need a run (**Unverified**).

## 9. What will probably fail first

Ordered by when it is hit during the boot. T-016 should check them in this order.

0. **Stall at `0x11A7A8` (`sceCdLayerSearchFile`).** (corrected on 2026-10-11 after the first run, journal 002): the first file
   open of `modules/0000.fpk` goes `0x1345B0` → `0x132850` → `0x11AAA8` → `0x11A7A8`, which
   loops on `sceSifBindRpc(0x80000597)` forever because no IOP module serves the cdvdman SIDs
   (journal 002; **Measured** by scanning every IRX). Fix in section 10 (bind `0x11A7A8` to the
   runtime `sceCdSearchFile`).
1. **Heap size (arena sizing loop). Resolved on 2026-10-12 by patch 0032** (journal 006,
   iterations 9-10). Measured: `SetupHeap` at `0x1001E4` passes base `0x5A6800` and size `-1`
   (0xFFFFFFFF); the handler (`Kernel/Syscalls/System.cpp`) used to clamp the heap end to
   `0x01F00000` (`kDefaultGuestHeapEnd` / `kGuestHeapHardLimit`) while the console's kernel lets it
   end at the bottom of the stack the game reserved with `SetupThread` (`0x01F80000`, the top
   0x80000 bytes of RAM). At boot the game asks, from `0x1365C0` (call `0x136600`,
   `0x1270C0(0x40, 0x19D73F0)`; the size is `(0x1F7DBFF - 0x5A6800)` rounded down to 16, so the game
   expects a heap ending near `0x1F7DBF0`), then retries decreasing the size by `0x100` (`0x136618`)
   until the allocation succeeds. With the clamp the second arena (`0x4D69B0`/`0x4D69C4`) came out
   `0x7DD00` (~515 KiB) smaller than the console's (`0x4D69C4 = 0x9596F0` here vs `0x9D73F0` there =
   `0x19D73F0 - 0x1000000`: the console's first attempt succeeded), and because the engine allocates
   its texture pools from those arenas, every texture pointer was wrong and the engine's copies to
   them clobbered the game's code area and then the platform flag at `0x3C0360` (journals 006/007).
   The handler now reports `start + size`, or the calling thread's stack bottom when the size is
   `-1`, with no clamp; the runtime's own guest memory (the four pools, the invocation stacks and the
   allocator behind `guestMalloc`) moved out of the game's heap into the console's free kernel area
   `0x00080000-0x00100000` (patch 0032, and `docs/knowledge/13-runtime-host.md`, "The runtime's own
   memory inside the guest's RAM"). After the fix the arena words match the console's memory image
   exactly, the pool descriptors at `0x5446E0`/`0x544708` hold guest-heap addresses (the first one
   equal to the console's in both fields) and the boot reaches the frame loop: three 60 s runs give
   `RESULT: OK_VSYNC_LOOP` and the game draws its memory-card warning dialog.
   (This item was first written on 2026-10-11, before any run: it predicted the shortfall and
   proposed this fix. It was left for later because the retry loop terminates by itself; journal
   006 showed that it was the first point where the run departed from the console.)
2. **Module load returns negative (infinite loop).** Measured: the loop at `0x136708`–`0x13671C`
   (`0x115238`) never ends if `_sceSifLoadModuleBuffer` returns `< 0`
   (`Sys/System.cpp:288`). Expected at the first non-supported import: `padman` (`sifman` 32
   imports), `mcman` (`secrman` 6, `cdvdman` 24), `cdvdstm` (`cdvdman` 7), per doc 14.
   Log sign: the same `sceSifLoadModuleBuffer` line repeating. Proposed fix: per doc 14 /
   T-1xx, supply the missing imports or decide per module whether the load is HLE
   (`mcserv`, `padman`, `cdvdstm` can be HLE). Also **Unverified**: that the module id
   result is the only thing the loop needs (`$a3` is garbage, `0x136710`).
3. **Unsupported IOP imports at module start** (same modules as 2): the module loads but its
   start function fails. Sign: log from `ps2xIOP`. Fix as above.
4. **Mtap / pad init** (`0x138DC0`): `sceMtapInit` `0x255CA0` is recompiled and does
   `sceSifBindRpc(0x80000900)`. It needs `mtapman` loaded and registered; if not, the
   recompiled code spins on the bind (the `0x325E58` pattern). Fix: add an HLE route or
   replace `sceMtapInit` and its helpers (`0x255DC0`, `0x255EA0`, `0x255FF0`, `0x256060`).
5. **Unmapped alarm syscalls `0xFD` and `-254`.** Dispatcher gap described in section 5;
   sign: `Unknown syscallId=0xfd` or `0xfffffe02`. Fix: map them in `Sys/Dispatcher.cpp:7`
   (one patch). **Unverified** whether they are reached at boot.
6. **CRI/ADX init loops.** `0x331C60` loops `while (0x311468(n) != 3) 0x315928()`; the loop only
   ends when the ADX state reaches 3, which depends on the `cri_adxi` IRX answering RPC and
   on the vblank tick (`0x331F20`). Sign: boot stuck after `modules/1000.fpk` with no log
   progress. Fix depends on the cause: IOP RPC for ADX missing (doc 14) or no VBLANK delivered.
7. **EE-side RPC server `0x3258B8` / bind `0x325E58`.** If the server thread is not created
   or does not register before the bind, `0x325E58` spins (section 6). Fix: ensure the thread
   starts (Q-RPCLOOP, T-302).
8. **`sceGsSyncVCallback` and VBLANK delivery.** The replaced `sceGsSyncVCallback`
   (`0x101DB8`) and INTC cause 3 must call `0x331F20` and `0x138EE0`; if they never fire,
   section 8 says the loop spins unpaced and ADX (item 6) never advances.
9. **First video (`OP.sfd`)** only after the title scripts; nothing at boot. See section 7.

## 10. Remote service binds (census)

Added on 2026-10-11 (T-016 iteration 1b), code reading only, no run. Method: every `jal 0x110008`
in the executable (**Measured**: 17 call sites; no tail jumps, no function pointers to
`0x110008`), the SID constant in `$a1`, then the callers of each containing function, then
which functions are replaced (small files in `generated/` calling `ps2_stubs::NAME`), then the
`sceSifRegisterRpc` constants of every IRX.

### 10.1 Sites

| Call | Function | SID | Status | Reachable at boot |
|---|---|---|---|---|
| `0x1111E4` | `0x111128` `sceFsInit` | `0x80000001` | replaced | no |
| `0x11459C` | `0x114550` `sceSifInitIopHeap` | `0x80000003` | replaced | no |
| `0x114AD4` | `0x114AA0` `_lf_bind` | `0x80000006` | recompiled, retry loop | no: every caller is replaced or has no callers |
| `0x1185F4`, `0x118648` | `0x118590` `scePadInit` | `0x80000100`, `0x80000101` | replaced | no |
| `0x119CF4` | `0x119CC0` `sceSdRemoteInit` | `0x80000701` | recompiled | yes, from `0x151EF8` (`0x151E60`, step 8) |
| `0x11A874` | `0x11A7A8` `sceCdLayerSearchFile` | `0x80000597` | recompiled, retry loop | **yes, first** (via `0x11AAA8`) |
| `0x11ABA4` | `0x11AAC8` `_sceCd_ncmd_prechk` | `0x80000595` | recompiled | no: callers `0x11AC20`, `0x11B6F0`, `0x11B8D0`, `0x11B9A0` are replaced |
| `0x11AEA4` | `0x11ADC8` `_sceCd_scmd_prechk` | `0x80000593` | recompiled | no: callers `0x11BA50`, `0x11BAE8`, `0x11BBA0`, `0x11BC98` are replaced |
| `0x11B09C` | `0x11AFD0` `sceCdInit` | `0x80000592` | replaced | no |
| `0x11B3A4` | `0x11B2B8` `sceCdDiskReady_old` | `0x8000059A` | recompiled | no: only caller `0x11B4D0` is replaced |
| `0x11B590` | `0x11B4D0` `sceCdDiskReady` | `0x8000059C` | replaced | no |
| `0x11BE40` | `0x11BD58` `sceMcInit` | `0x80000400` | replaced | no |
| `0x255CF4` | `0x255CA0` `sceMtapInit` | `0x80000900` | recompiled | yes, from `0x138DC0` (main step 3) |
| `0x325E9C` | `0x325E58` CRI DTX bind | `[0x3DE484]` = `0x90000200` | recompiled | yes (chain in section 6); also from `0x330680`, `0x3308D8` |
| `0x375EC8` | `0x375E78` | `0x534E4446` "SNDF" | recompiled | yes, from `0x37A460` |
| `0x375F3C` | `0x375E78` | `0x53465356` "SFSV" | recompiled | yes, same function |

All of the above is **Measured** except the reachability of `0x325E58` callers' own parents
(**Declared**, from section 6).

### 10.2 Who serves each SID

`SifBindRpc` (`Sys/RPC.cpp:254-340`) publishes a server only if an EE server is registered or
`canBindIopRpc(sid)` is true, i.e. the real IOP code registered it (section 6 correction).

| Module | SIDs it registers | Loaded by the game |
|---|---|---|
| `mtapman` | `0x80000900` | yes (pack 0000) |
| `mcserv` | `0x80000400` | yes |
| `padman` | `0x80000100`, `0x80000101` | yes (its `sifman:32` import is unsupported) |
| `sdrdrv` | `0x80000701` | yes (pack 1000, module 12) |
| `cri_adxi` | `0x90000200` | yes (`0x331C60`) |
| `sndfi` | `0x534E4446`, `0x53465356` | yes (`0x37A460`) |
| `libsd`, `sio2man`, `mcman`, `cdvdstm`, `modhsyn`, `modmidi`, `modmsin` | none | yes |
| `rtfssiop`, `tsnddrv`, `tsnddrvc` | not relevant | not loaded |

**Measured** (scan of each IRX's RegisterRpc constants, `tools/re`-style script kept outside the
repo). **No module registers `0x80000592`..`0x8000059C`, `0x80000001`, `0x80000003` or
`0x80000006`**: nothing serves the cdvdman, fileio or loadfile SIDs. That is what stops the
first run.

### 10.3 Binds that wait forever, in boot order

| # | Bind | Why it waits | Lightest fix |
|---|---|---|---|
| 1 | `0x11A7A8` (`0x80000597`), reached at step 9 from `0x1345B0` | nothing serves it; the loop retries with a 1M-iteration delay | bind the address itself to the runtime handler: `ps2_game_overrides::bindAddressHandler(runtime, 0x0011A7A8, "sceCdSearchFile")` in `src/game/don_overrides.cpp`. Binding `0x11A7A8`, not the wrapper `0x11AAA8`, covers both callers (below) |

Handler fit (**Measured**): `ps2_stubs::sceCdSearchFile` (`Stubs/CD.cpp`, about `:545-649`)
reads `$a0` (file struct) and `$a1` (path), ignores `$a2` and writes the result through
`writeCdSearchResult`, returning 1 or 0. `0x11A7A8` has the signature
(`sceCdlFILE*`, `path`, `layer`), copies `0x24` bytes back and returns 1 or 0, so the handler is
compatible. `ps2_call_list.h` has `sceCdSearchFile` but no `sceCdLayerSearchFile`; hence the
by-address bind.

**Second caller of `0x11A7A8`** (**Measured**): `0x3261B4` in `0x326188`, which passes the layer
variable at `0x3DF0BC` as `$a2`. Its initial value in the ELF is 0. The only writer is the
setter `0x326B80` (stores `a0` if `a0 < 2`, else 0), which has no direct callers, no `lui/addiu`
references and no data pointers, so the value stays 0. The handler ignoring `$a2` is therefore
harmless there. `0x326188` is called from `0x326400`, `0x326488`, `0x3265A0`, `0x326BB0`,
`0x3270C8`, `0x327250` (later paths, **Unverified** when they run). The wrapper `0x11AAA8` is
called from `0x132740`, `0x1328DC`, `0x132A78`, `0x132C04` (menus and loads after boot).

### 10.4 Binds that should succeed once their module has been loaded

| Bind | Module | Load order | Notes |
|---|---|---|---|
| `0x255CA0` (`0x80000900`) | `mtapman` | pack 0000 order 2, before `0x138DC0` | Measured. After binding it calls `0x256060` and expects `version >> 4 == 0x31` (mtapman 3.16); a negative bind returns 0, otherwise it retries with a `0x10000` delay |
| `0x119CC0` (`0x80000701`) | `sdrdrv` | `0x151E60` loads the five pack-1000 modules (`0x136670` at `0x151EA0`...) and calls `sceSdRemoteInit` after them | Measured. It calls `0x10F860` first, loops until `server != 0`, returns -1 on a negative bind |
| `0x375E78` (`0x534E4446`, `0x53465356`) | `sndfi` | `0x37A460` loads it (`0x136670(0x4C74C0)`) immediately before the bind (`0x37A4A8`) | Measured. Loops forever on server 0 or a negative return |
| `0x325E58` (`0x90000200`) | `cri_adxi` | loaded by `0x331C60` | **Unverified** that `0x331C60` runs before the chain `0x36ACB0` → `0x347B40` → `0x3134F8` → `0x316428` → `0x32C320` |

For all four the module's registration must have happened when the bind runs. The IOP runs only
when the EE accounts cycles (back-edges and inter-function transfers, doc 13), and the bind loops
are exactly such back-edges, so the wait is bounded by the module's start thread. If a start
thread fails (unsupported imports, section 9 items 2 and 3) the bind then waits forever; the
fix is then in the IRX imports, not in the bind.

### 10.5 Not reached or replaced

`0x80000001`, `0x80000003`, `0x80000006`, pad, mc, and the cdvdman SIDs `0x80000592`,
`0x80000593`, `0x80000595`, `0x8000059A`, `0x8000059C` (see 10.1). No action needed unless a
later path reaches a recompiled caller (`0x11AAC8`, `0x11ADC8`, `0x11B2B8` have only replaced
callers, **Measured**).

### 10.6 What could not be determined

- Whether `cri_adxi` is loaded before the `0x325E58` chain (10.4).
- Who starts the EE thread `0x3258B8` (no callers, no references, no data pointers).
- Whether the later `0x326188` and wrapper callers run in the first minutes of play.
- Whether other unnamed recompiled SDK functions have a public-name runtime handler besides
  `0x11AAA8` (`sceCdSearchFile`); only the cdvdman layer was compared with `ps2_call_list.h`.

## How to repeat

```bash
# call-graph closure (reads the executable; needs the helper scripts written for T-028,
# kept outside the repository). Equivalent public tools:
uv run --no-project python -I tools/re/callers.py --elf disc/SLPS_256.75 \
    --target 0x110008 --csv config/functions.ghidra.csv          # callers of sceSifBindRpc
uv run --no-project python -I tools/re/strxref.py --elf disc/SLPS_256.75 \
    --text-end 0x3902D4 --csv config/functions.ghidra.csv 'IOPRP|\.irx|movie'
bash tools/re/decompile.sh 0x136770 0x136670 0x131CA0 0x36A820   # sections 1, 2, 7, 8
grep -n 'kDefaultGuestHeapEnd\|kGuestHeapHardLimit' \
    third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp \
    third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_runtime.cpp
```

The syscall census scans every `syscall` instruction, takes the number from the preceding
`addiu $v1, $zero, N`, names the containing function from the CSV and the SDK names in
`work/analyzer/config.toml`, and compares with the cases of `Sys/Dispatcher.cpp:7`.
Run-time confirmation of every **Unverified** entry is the job of T-015/T-016.
