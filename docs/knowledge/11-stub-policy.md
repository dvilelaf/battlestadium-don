# 11 - SDK function policy: recompile or replace

Decision for each of the 197 SDK functions named in `work/analyzer/config.toml` `[general] stubs`.
The machine-readable form is [config/stub_policy.toml](../../config/stub_policy.toml).

Markers: **Measured** = checked in the decompile, the call sites or the runtime source.
**Declared** = stated by a task card or an upstream document. **Unverified** = not checked.

Decisions:

- `stub`: the PS2Recomp runtime handler replaces the game's function.
- `recompile`: the game's own code is recompiled (the runtime handler is not used).
- `review`: undecided; needs an answer to the question listed in "Review". Until resolved it is treated as `recompile`.

Counts (Measured, from the toml): 164 `stub`, 33 `recompile`, 0 `review` (T-020 delivered 143, 30 and
24; the 24 were decided by the orchestrator on 2026-10-11, see "Review"). Default for names not listed: `recompile`.

A handler that only calls `TODO_NAMED` is never `stub` without a written justification (see Debug console).

Handler paths are relative to `third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/`.

## Graphics (GS / GIF / VIF1)

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| `sceGsSyncV`, `sceGsSyncVCallback` | 2 | stub | Vblank wait and callback belong to the host scheduler (Measured). | `Stubs/GS.cpp:1281`, `:1289` |
| other `sceGs*` | 15 | review (Q-GS) | Handlers write through `runtime->gs()`, which D4 discards, and share host-side `g_gparam`; must be decided as one set (Measured). | `Stubs/GS.cpp:670-1319` |
| `sceGifPk*`, `sceVif1Pk*` builders | 4 | recompile | The rest of the packet-builder family is not in the list; stubbing four members would mix two state layouts (Measured). | `Stubs/GS.cpp:373`, `:402`, `:1453`, `:1499` |

## DMA / video

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| `sceDma*` (send/sync/reset family) | 8 | stub | Real handlers; DMA is modelled by the runtime (Measured). | `Stubs/DMA.cpp:52-232` |
| `sceDevVif0Reset` | 1 | stub | Real handler. | `Stubs/System.cpp:51` |
| `sceDmaRecv`, `sceDmaRecvN`, `sceDmaRecvI`, `sceDmaWatch` | 4 | recompile | Handlers are TODO-only and there are no callers (Measured). | `Stubs/DMA.cpp` |
| `sceDmaPause` | 1 | review (Q-DMAPAUSE) | TODO-only, but called by `WaitDma`/`WatchDma` (Measured). | `Stubs/DMA.cpp` |
| libmpeg | 8 | stub | Real handlers for the decoder API (Measured). | `Stubs/MPEG.cpp:1876-2502` |
| IPU | 4 | stub | The IPU is not modelled; the handlers stand in for it (Declared). | `Stubs/IPU.cpp:53-104` |
| `sceMpegGetPictureRAW8` | 1 | review (Q-RAW8) | TODO-only, 1 caller at 0x35D860 (Measured). | `Stubs/MPEG.cpp:2421` |
| `sceMpegGetPictureRAW8xy`, `_sceMpegFlush` | 2 | recompile | No callers / no handler (Measured). | `Stubs/MPEG.cpp:2426` |

## Kernel and libc

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| libc math/mem (`memclr`, `memcmp`, `rand`, `floor`, `log10`, `__kernel_sinf`, `__kernel_cosf`, `__ieee754_rem_pio2f`, `__divdi3`) | 9 | recompile | Handlers use host libm / `std::rand`, which is not bit-identical to the game's (Measured). | `Stubs/LibC.cpp:170,246,931,945,952,1007,1019,1083,1178` |
| `_sceSDC`, `_sceIDC` | 2 | recompile | Cache ops; no reason to replace (Declared). | n/a |
| `InitTLB`, `InitAlarm` | 2 | stub | Patch the kernel area at 0x800xxxxx, meaningless on host (Measured, see Kernel patches). | `Syscalls/System.cpp:602`, `Syscalls/Sync.cpp:344` |
| `iWakeupThread`, `iRotateThreadReadyQueue`, `StartThread` | 3 | stub | Real syscall handlers (Measured). | `Syscalls/Thread.cpp:387`, `:423`, `:253` |
| `InitThread` | 1 | review (Q-THREAD) | Proposal: recompile (see Review). | `Syscalls/Thread.cpp:183` |

## Debug console

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| DECI2 | 8 | stub | Handlers are TODO-only. Justification: the only callers are `sceTtyInit` and the TTY shims, so nothing the game's logic depends on is lost (Measured). | `Stubs/Deci2.cpp:6-46` |
| TTY | 5 | stub | Same justification as DECI2. | `Stubs/TTY.cpp:40-55` |
| `write`, `read` | 2 | review (Q-TTYIO) | At 0x10C868 / 0x10C8E8 they are TTY shims, not file I/O (Measured). | `Stubs/FileIO.cpp:136`, `:58` |

## SIF

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| RPC, cmd, heap, addr, reset | most | stub | Real handlers backed by the runtime transport (Measured). | `Stubs/SIF.cpp:292-843`, `Syscalls/RPC.cpp` |
| `sceSifRpcLoop` | 1 | review (Q-RPCLOOP) | See Review. | `Stubs/SIF.cpp:595` |
| `_sceSifLoadModuleBuffer`, `_sceSifLoadModule`, `_sceSifLoadElfPart` | 3 | review (Q-LOAD) | See Review. | `Syscalls/System.cpp:269-300` |
| `_sceSifSendCmd`, `_sceSifCmdIntrHdlr`, `_sceRpcGetFPacket`, `_sceRpcGetFPacket2` | 4 | recompile | SDK internals only reached from functions that are themselves stubbed (Measured for `sceSifSendCmd`; Unverified for the others). | `Syscalls/RPC.cpp` |
| `sceSifLoadIopHeap` | 1 | recompile | No-op handler, no callers (Measured). | `Stubs/SIF.cpp` |

## File I/O, pad, sound, disc, memory card

| Group | Functions | Decision | Reason | Handler (file:line) |
|---|---|---|---|---|
| `sceFs*`, `sceOpen` | 8 | stub | Real handlers. | `Stubs/FileIO.cpp:63-116` |
| `_sceFsSemInit` | 1 | recompile | No handler. | n/a |
| `scePad*` | 28 | stub | Replaced by host input (D9). The handlers read raylib directly, so a host patch to SDL3 is needed (Measured). | `Stubs/Pad.cpp:343-766` |
| `sceSdRemoteInit`, `sceSdRemote`, `sceSdTransToIOP` | 3 | recompile | Audio goes through the SPU2 bridge (D6). | `Stubs/Audio.cpp:77,197,208` |
| `sceCd*` | 14 | stub | Disc access is served by the host (Declared). | `Stubs/CD.cpp:207-975` |
| `sceMc*`, `mce*` | 24 | stub | Memory card is host-backed (Declared). | `Stubs/MemoryCard.cpp:512-1399` |

## Kernel patches

Measured. `_InitSys` (0x116618, called once from entry 0x1001E8) calls these installers:

- 0x1163C8: creates `SceKernelLibc` semaphores.
- 0x116508 `InitSystemCallTableAddress`: scans `0x80000000-0x80080000`.
- 0x116AF8 `InitAlarm`: copies 0x740 bytes from 0x3BD260 to `0x80076000`.
- 0x116C70 `InitTimer`.
- 0x116740 `InitExecPS2`: copies 0x7A8 bytes from 0x3BCA98 to `0x80074000`.
- 0x115C30 `InitTLBFunctions`: copies 0x330 bytes from 0x3BC428 to `0x80075000`.

`InitTLB` (0x115DB8) has no direct caller; it is reached through the thunk at 0x116958. Only `InitTLB` and `InitAlarm` are in the 197. `InitTLBFunctions`, `InitSystemCallTableAddress` and `InitExecPS2` are not in the list and have no runtime handler name. Proposal: add them to the config `[general] skip` list if they prove harmful; do not skip `_InitSys` itself.

`kCopy` is `li v1,0x5A; syscall`; the runtime implements syscall 0x5A as `Copy` (`Syscalls/Dispatcher.cpp:225`) and `SetSyscall` as an override table (`Syscalls/System.cpp:462`).

## Review

| Id | Entries | Question |
|---|---|---|
| Q-GS | 15 `sceGs*` | Do the handlers feed the D4 bridge, or only the PS2Recomp GS? Decide the whole set together. |
| Q-LOAD | `_sceSifLoadModuleBuffer`, `_sceSifLoadModule`, `_sceSifLoadElfPart` | The game's loader reaches 0x114C68 through unnamed wrappers at 0x115238 and 0x115258, which call `sceSifCallRpc` with SID 6. Nothing in `ps2xIOP`/`ps2xRuntime` serves SID `0x80000006` (grep, Measured). Module loading works only through the named handlers. Either bind the wrappers to the handlers (add `sceSifLoadModuleBuffer@0x00115238` etc. to the stubs) or add a LOADFILE service to the transport. Confirm signatures with the caller at 0x136710. |
| Q-THREAD | `InitThread` | The task card calls it a kernel-patch installer; that is false (Measured). It creates the `SceKerneltopThread` helper. Proposal: recompile. Note `iSuspendThread` (0x10D280, recompiled) checks `DAT_003bc358`, which only the original `InitThread` sets. |
| Q-RPCLOOP | `sceSifRpcLoop` | The game runs an EE-side RPC server thread at 0x3258B8 (handler 0x325808) and `sceSifRpcLoop` returns 0. Does the runtime dispatch EE servers without a loop? |
| Q-DMAPAUSE | `sceDmaPause` | TODO-only handler, called by `WaitDma`/`WatchDma`. Is returning -1 acceptable? |
| Q-RAW8 | `sceMpegGetPictureRAW8` | TODO-only handler with one caller at 0x35D860. Which output format does the caller expect? |
| Q-TTYIO | `write`, `read` | TTY shims; callers include 0x125034, 0x1261E0, 0x12AB00 (`read`: 0x124F74). Decide whether to route to the TTY handlers or recompile. |

### Decisions on the review entries (orchestrator, 2026-10-11)

These are decisions for the first boot (phase 0). Each one names the task that must look at it again.

| Id | Decision | Reason | Revisit in |
|---|---|---|---|
| Q-GS | `stub` (all 15) | Phase 0 runs on PS2Recomp's own GS, and the handlers are the path PS2Recomp is tested with. When the PCSX2 GS replaces it (D4), either `runtime->gs()` is routed to the bridge or these 15 go back to `recompile`, as one set. | T-108 |
| Q-LOAD | `stub` (all 3) | With `recompile` the load request goes to `sceSifCallRpc` with a service nobody serves, so no module would load. The handlers load the module into the IOP emulator. **Unverified**: that the internal functions take the same first three arguments as the public ones the handlers were written for, and what the game does with the result pointer in `$a3`, which the handler does not write. | T-102 |
| Q-THREAD | `stub` | The handler returns the main thread id and creates no helper thread. That is coherent with `iWakeupThread` and `iRotateThreadReadyQueue` being replaced by direct handlers: the flag at `0x3BC358` stays 0 and the recompiled `iSuspendThread` takes its direct path. | T-016 if thread errors appear |
| Q-RPCLOOP | `stub` | Upstream default. Whether the runtime serves the game's EE-side RPC server (thread at `0x3258B8`) without the loop is **Unverified**; it matters for sound. | T-302 |
| Q-DMAPAUSE | `recompile` | The handler does nothing (TODO); the original only writes DMA control registers. | T-016 if it hangs |
| Q-RAW8 | `stub` | The rest of libmpeg is replaced, so the recompiled original would read state that the handlers do not keep. The TODO handler logs every call, which is what we want to see. | T-305 |
| Q-TTYIO | `recompile` | The runtime handlers are file I/O and would fail on descriptors 1 and 2. The original shims are kept and the game's own text output is captured by a game override (task T-014). | T-014 |

**Q-GS, settled on 2026-10-12.** The 15 handlers stay replaced; with a host GS installed they now write what the game's own code and the console write (patch `0021-host-gs-display-registers`, measured against the reference dump of the game's first screen: [18-display-registers.md](18-display-registers.md)). Going back to the game's own code for part of the set (stage 2 there) is not needed so far. The history follows.

**Q-GS, open again (2026-10-11, T-110 part 2).** The paragraph and table below were the answer on paper. A test with the real PCSX2 GS then showed that reaching the host is not enough: the values the handlers send are made for the runtime's own GS (two stand-in addresses that are real GS registers, DISPLAY and SMODE1 values that give a picture four times too narrow), and the game does call them. See "The replaced `sceGs*` handlers against a real GS" in [13-runtime-host.md](13-runtime-host.md). The choice between correcting the handlers and going back to the game's own code is being worked out.

*Corrected on 2026-10-11 (T-108 follow-up).* The game's own `sceGsResetGraph` sends no GIF packet at all; it only writes CSR and IMR and calls the SetGsCrt syscall, which is the one that sets SMODE1/SRFSH/SYNCH/SYNCV. The runtime's stand-in packet is therefore the wrong place to set the display, and a host-side filter of that packet is not a fix. Proposal (not decided): a small patch so the handlers write what the game's code writes in host-GS mode (no stand-in packet, SetGsCrt writes SMODE1 and the sync registers, SetDefDispEnv/SetDefDBuff use the SDK formulas), and recompile the pure builders only later. Whole-set recompilation is not recommended because `sceGsSyncV` spins on INTC_STAT, which the runtime does not model. Details, reference register values and the list of differences: [18-display-registers.md](18-display-registers.md).

**Q-GS, as first answered on 2026-10-11 (T-108): the 15 stay `stub`.** With a host GS installed (patch
`0005-host-gs-hook`), `GS::writeRegister` becomes an A+D packet on path 3 and direct writes of
privileged registers reach the host at the next vertical blank, so the effect of every handler
reaches the host GS. **Declared** from reading `Stubs/GS.cpp`; not yet run with the real GS
library.

| Handler | What it does to the GS | How it reaches a host GS |
|---|---|---|
| `sceGsResetGraph` | GIF packet with A+D writes to `0x41`, `0x42`, `0x59`-`0x5C`, `0x5F` | As a GIF packet; the hook's scanner applies `0x59`-`0x5F` to the privileged registers |
| `sceGsPutDispEnv` | Privileged registers, written directly | Through `vsync` |
| `sceGsSwapDBuff`, `sceGsSwapDBuffDc` | Display: privileged registers. Drawing: `writeRegister` | `vsync` and the routed `writeRegister` |
| `sceGsPutDrawEnv` | `writeRegister` | Routed as an A+D packet |
| `sceGsExecStoreImage` | GIF packet and a read of the FIFO | The arbiter and `read_fifo` |
| `sceGsSyncPath` | Polls DMA registers | Nothing to forward |
| `sceGsSetDefDispEnv`, `sceGsSetDefDrawEnv`, `sceGsSetDefDrawEnv2`, `sceGsSetDefDBuff`, `sceGsSetDefClear`, `sceGsSetDefStoreImage`, `sceGszbufaddr`, `sceGsResetPath` | Memory only, or a computation | No effect on the GS |

Open (**Unverified**): `sceGsResetGraph` sends A+D writes to `0x59`-`0x5F`, which are not GS
registers; what the PCSX2 GS does with them is not tested. If it misbehaves, the host program
filters them out of the packet, since the scanner already applies them.

Not in the list but likely to break (Unverified): `scePadSetDmaCallback` (0x119B68), `sceMtapInit` (0x255CA0), `sceMtapChangeThreadPriority` (0x255FF0), `sceMtapGetModVersion` (0x256060), `DelayThread`, `iSuspendThread`.

## Analyzer names that look wrong

- `InitThread@0x10D078` is the libkernel top-thread creator, not the syscall wrapper.
- 12 `_InitSys@0x0031xxxx` entries in game code are a false positive; they are in `entry_points`, not in the 197.
- `kCopy@` addresses (0x115BD8, 0x116448, 0x116690, 0x116800, 0x116AB0) are 0x10 too high; real wrappers are at 0x115BC8, 0x116438, 0x116680, 0x1167F0, 0x116AA0.
- `write`/`read` are TTY shims.
- `sceSifLoadElf` and `sceSifLoadIopHeap` have no call sites.
- `sceSifSendCmd` returns 1 without delivering to the IOP; `sceFsInit` binds SID `0x80000001` (FILEIO), which the transport does not serve.

## How this file is used

T-011 reads `config/stub_policy.toml` and keeps only the `stub` entries when it builds the recompiler config. `review` is treated as `recompile` until the question is resolved. Names that appear in a later analyzer run and are not in the file fall to `default = "recompile"`.

Replaced by name, beyond the 164 of the policy (**Measured** on 2026-10-11 in the first
generation, T-012: the recompiler reports 172 replaced functions): `sceSifStopDma`
(`0x10C570`), `sceSifDmaStat` (`0x10C640`), `sceSifSetDma` (`0x10C660`), `isceSifSetDma`
(`0x10C670`), `sceSifSetDChain` (`0x10C680`), `isceSifSetDChain` (`0x10C690`), `sceSifSetReg`
(`0x10C6A0`) and `sceSifGetReg` (`0x10C6B0`). Ghidra names these rows itself (they are
system call wrappers of a few instructions), the names equal runtime handler names, and the
recompiler replaces any function whose name matches. The handler is the one the system call
would reach anyway, so the effect is the same; they are listed here so that the count adds up.

Recompiler behaviour to keep in mind (Measured): a stub name with no handler is recompiled anyway, with a warning (`ps2_recompiler.cpp:1091-1117`); a TODO-only handler counts as resolved, so the function is replaced by one that logs and returns -1; any function whose map name equals a handler name is also treated as a stub (`ps2_recompiler.cpp:2222-2234`).
