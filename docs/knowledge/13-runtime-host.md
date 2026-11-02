# 13 · How the runtime wires window, gamepad and audio today

Audit for task T-109. Nothing was built or run. Everything below comes from reading the pinned
PS2Recomp tree.

**Conventions.** **Measured** = read in the source by this audit (file:line cited).
**Declared** = stated by this project's documentation or by the PS2Recomp READMEs, not
re-checked. **Unverified** = a conclusion drawn here, not tested.

Paths are relative to `third_party/PS2Recomp/ps2xRuntime/` unless they start with `../`
(`third_party/PS2Recomp/`). Commit `2c5fbb9389e11dd95693385969490c9e8e6f57b4`; no patch of this
repository touches the runtime.

## The five questions of the card

| # | Question | Answer | Source |
|---|---|---|---|
| 1 | Who creates the window, on which thread does the main loop run? | `PS2Runtime::initialize()` calls `SetConfigFlags(FLAG_WINDOW_RESIZABLE)`, `InitWindow(...)` (640x448) and `InitAudioDevice()` on the **process main thread**, called from `main()`. `PS2Runtime::run()` then starts a second thread (`GameThread`) for the guest and keeps the main thread as the **window/presentation loop**. **Measured** | `src/main.cpp:217,229`; `src/lib/ps2_runtime.cpp:735-739`, `:2367-2389`, `:46-52` |
| 2 | Where is each frame presented? | In the `while` loop of `PS2Runtime::run()` on the main thread: `UploadFrame()` copies the CPU GS's latched frame into a raylib texture (`UpdateTexture`), then `BeginDrawing`/`DrawTexturePro`/`EndDrawing` letterbox it. Pacing is raylib's `SetTargetFPS(60)` (inside `EndDrawing`), **not** the guest's vertical blank. **Measured** | `src/lib/ps2_runtime.cpp:2389-2449` (loop), `:376-475` (`UploadFrame`), `:739` |
| 3 | How does the gamepad state reach the game? | `scePadRead` (an HLE handler run on the guest thread) fills the 32-byte buffer the game passed in `$a2`, from a `PadInputState` obtained from `PSPadBackend::readState()`, which calls raylib. 2 ports x 1 slot, port and slot ignored when reading. Details in the gamepad section. **Measured** | `src/lib/Kernel/Stubs/Pad.cpp:269-327`, `:627-660`; `src/lib/ps2_pad.cpp:29-123` |
| 4 | How is audio output today? | There is no SPU2 model. The sound path is a heuristic: the IOP's `libsd` HLE service forwards RPC `0x80000701` to `PS2AudioBackend::onSoundCommand`, which decodes VAG samples seen in guest RAM or in `fio` reads to PCM, wraps them in a WAV, and plays them with raylib `Sound` objects (max 4 at once). The device is opened by `InitAudioDevice()`. Details in the audio section. **Measured** | `../ps2xIOP/src/modules/libsd.cpp:35-55`; `src/lib/ps2_iop_host.cpp:247-266`; `src/lib/ps2_audio.cpp:110-328`; `src/lib/ps2_runtime.cpp:736-737` |
| 5 | What has to change so a program of our own provides window, gamepads and audio with SDL3? | Seven changes in the last section. Window, pad input and audio output **cannot** be moved to SDL3 without patching the runtime, because raylib is called directly from five source files and the main loop is inside `PS2Runtime::run()`. The only existing hook (`PSPadBackend`) is a concrete class, not an interface. **Measured** | section "What a host program of our own needs" |

## Where the runtime calls raylib

**Measured.** There is **no interface** that confines raylib. `include/ps2_host_backend.h` is a
two-line header whose only content is `#include "raylib.h"` (`include/ps2_host_backend.h:3`);
it is the "backend" only in the sense that CMake links `raylib` into an INTERFACE target
named `ps2_host_backend` (`CMakeLists.txt:132,238`), and `ps2_runtime` links that target
publicly (`CMakeLists.txt:509`). Every file that includes it, or `Kernel/Stubs/Common.h` (which
includes it, `src/lib/Kernel/Stubs/Common.h:29`), sees the whole raylib API and calls it
directly.

**Build option to compile without raylib: none.** The CMake options are runner unity build,
PCH, sccache, logs, IOP RPC trace, strict returns, Windows console and `PS2X_ENABLE_DEBUG_UI`
(`CMakeLists.txt:8-18`). `PS2X_ENABLE_DEBUG_UI=OFF` removes only the ImGui panel and rlImGui
(`CMakeLists.txt:86,515-521`), not raylib. The only other switches are `PLATFORM_VITA` and
`__ANDROID__`, which swap raylib for Vita raylib/SDL2 and skip sound and ImGui, not remove the
dependency (`CMakeLists.txt:142-239`). raylib is fetched with `FetchContent` from GitHub
(`CMakeLists.txt:79-84`).

| File | raylib calls | What the group does | Source |
|---|---|---|---|
| `src/lib/ps2_runtime.cpp` | `SetConfigFlags`, `InitWindow`, `InitAudioDevice`, `IsAudioDeviceReady`, `SetTargetFPS` | Window and audio device creation in `initialize()` | `:732-739` |
| | `GenImageColor`, `LoadTextureFromImage`, `UnloadImage`, `UnloadTexture` | Frame texture (640x512 RGBA) | `:417,2361-2364`, `:2468` |
| | `UpdateTexture` | Upload the latched GS frame, or a magenta image when there is none | `:418,474` (inside `UploadFrame`) |
| | `BeginDrawing`, `ClearBackground`, `GetScreenWidth/Height`, `DrawTexturePro`, `EndDrawing`, `WindowShouldClose` | Presentation and close request | `:2427-2449` |
| | `IsAudioDeviceReady`, `CloseAudioDevice`, `IsWindowReady`, `CloseWindow` | Teardown (destructor and end of `run()`) | `:538-552`, `:2469` |
| `src/lib/ps2_pad.cpp` | `IsGamepadAvailable(0)`, `IsGamepadButtonDown`, `GetGamepadAxisMovement`, `IsKeyDown` | Pad state for `PSPadBackend::readState` | `:42-118` |
| `src/lib/Kernel/Stubs/Pad.cpp` | `IsGamepadAvailable(0..3)`, `IsWindowReady`, `GetGamepadAxisMovement`, `IsGamepadButtonDown`, `IsKeyDown` | Same, as a second copy (see "Pad: two copies"); also a log line | `:89-185`, `:651-661` |
| `src/lib/ps2_audio.cpp` | `LoadWaveFromMemory`, `LoadSoundFromWave`, `UnloadWave`, `SetSoundPitch`, `SetSoundVolume`, `PlaySound`, `StopSound`, `UnloadSound`, `IsSoundPlaying` | Whole-sample playback | `:240-324` |
| `src/lib/ps2_debug_panel.cpp` (ImGui debug UI, optional) | `IsKeyPressed(KEY_F1)`, `rlImGuiSetup/Begin/End/Shutdown` | Debug panel inside the raylib window | `:2184,2195,2209,2219,2306` |
| `src/lib/gs/*`, `src/lib/Kernel/EeScheduler.cpp`, `../ps2xIOP/` | none | No raylib call (grep for the raylib header and function prefixes found nothing) | **Measured** |
| `src/lib/ps2_vita_runtime.cpp`, `vita/` | Vita/Android variants | Out of scope on desktop | `src/lib/ps2_vita_runtime.cpp:22` |

Files that include `Kernel/Stubs/Common.h` (all of `Kernel/Stubs/`) compile with raylib's
global names (`Color`, `Rectangle`, `CloseWindow`...) in scope; `CMakeLists.txt:575` already
needs a workaround for the `CloseWindow` clash on MSVC. This is a compile-time coupling, not a
call; only `Pad.cpp` actually calls raylib among them.

## Threading model

| Concern | Answer | Source |
|---|---|---|
| Process main thread | `main()` builds `PS2Runtime`, calls `initialize()`, `loadELF()`, `run()`. `run()` becomes the **window and presentation loop** and blocks until the game thread ends or the window closes. | `src/main.cpp:196-229`; `src/lib/ps2_runtime.cpp:2389-2466` |
| Thread that runs the game | `GameThread`, a `std::thread` created in `run()`. It runs `EeScheduler::run()`, which is the single EE executor. | `src/lib/ps2_runtime.cpp:2367-2373`; `src/lib/Kernel/EeScheduler.cpp:155` |
| Thread that owns the window | The main thread (the one that called `InitWindow`). All raylib window, texture and `Sound` calls except the pad reads happen there. | `src/lib/ps2_runtime.cpp:735,2425-2447` |
| Guest threads | **Not host threads and not fibers.** Every guest thread is an `EeScheduler::GuestThread` record scheduled by one executor on `GameThread`. A generated function runs to completion (or to a checkpoint) and a thread switch is a C++ exception, `EeDispatcherTransfer`, "the EE equivalent of a longjmp to the dispatcher" caught in `run()`. `assertExecutor()` checks that kernel-object calls come from that one thread. | `include/runtime/ee_scheduler.h:17-23,286,372,430`; `src/lib/Kernel/EeScheduler.cpp:87,155-257,269-283` |
| IOP | Not a thread. The R3000 interpreter is advanced from EE cycle accounting on the EE executor (`accountCycles` -> `advanceIopEeCycles` -> `runEeCycles`). **Declared** by `../ps2xIOP/README.md` ("advances the IOP from EE cycle accounting"); the call chain is **Measured**. | `src/lib/Kernel/EeScheduler.cpp:391-396`; `src/lib/ps2_runtime.cpp:608-611` |
| Vertical blank | Generated by `EeScheduler`, on `GameThread`, from a host-time schedule: a `VBlankStart` event every 16,667 us (`kVBlankPeriod`), a `VBlankEnd` 500 us later, paced against `std::chrono::steady_clock` (`processDueDeadlines` sleeps with `m_eventCv.wait_until`). It bumps `m_vsyncTick`, toggles the GS `CSR` field bit (`0x2000`), writes the guest vsync flag, wakes `WaitVSync` waiters, queues the guest GS vsync callback and raises the VBLANK-start/end interrupts (INTC 2 and 3). **It is independent of the window**: the window loop does not signal it. | `src/lib/Kernel/EeScheduler.cpp:34-35,53-54,149-151`, `:1783-1815`, `:1848-1856`, `:1869-1907,1908-1910` |
| Link between the two loops | The presentation loop reads `eeScheduler().currentVSyncTick()` to decide when to latch a new GS frame (`latchHostPresentationFrame`), then copies the result. It runs as fast as `SetTargetFPS(60)` allows. | `src/lib/ps2_runtime.cpp:386-394`; `src/lib/gs/gs_frontend.cpp:527-575` |
| Pad reads | `scePadRead` is an HLE call made by the guest, so it runs on `GameThread`, **not** on the thread that owns the window. It calls `IsKeyDown`/`IsGamepadButtonDown` from there. raylib's input state is only updated by `EndDrawing` (`PollInputEvents`) on the main thread, so these are cross-thread reads of raylib's global state. **Unverified** that this is a bug in practice; it is relevant because an SDL3 host must poll events on the thread that owns the window and publish the result for the guest thread. | `src/lib/Kernel/Stubs/Pad.cpp:627-640`; `src/lib/ps2_runtime.cpp:2447` |
| Stop | `requestStop()` sets the runtime flag and the scheduler's; the main loop leaves, joins `GameThread` and calls `CloseWindow()`. Then `main()` calls `std::_Exit(0)`. | `src/lib/ps2_runtime.cpp:2183-2190,2450-2469`; `src/main.cpp:236` |

**How a guest busy loop yields (Measured; corrected on 2026-10-11 after the first run, journal
002).** A host backtrace that shows `GameThread` inside `EeScheduler::processPendingEvents` (in
`processDueDeadlines`, waiting on `m_eventCv`, `EeScheduler.cpp:1776-1815`) does **not** mean
that every guest thread is blocked. Generated code calls `runtime->eeCheckpointDue()` at every
backward branch (for example the delay loop at `0x11A85C` in `generated/FUN_0011a7a8_0x11a7a8.cpp`)
and every inter-function transfer charges 8 cycles the same way (`ps2_runtime.cpp:1357-1359`,
`:2212`). `EeScheduler::checkpointDue` (`EeScheduler.cpp:357-389`) adds the cycles to the EE
clock, to the EE timers and to the IOP interpreter (`accountCycles`, `:391-400`), and returns
true when an event is pending, a scheduled deadline (the next vertical blank) has been reached,
or the time slice (65,536 cycles, `ee_scheduler.h:269`) is over with an equal or higher priority
thread ready. The generated function then returns to `EeScheduler::run()` (`:155-330`), which
calls `processPendingEvents` (`:1743`); there `processDueDeadlines` sleeps in host time until the
vblank is due (16,667 us), so a single guest thread spinning in a delay loop spends most of the
host time in that wait and still advances. A guest thread that really has nothing to run goes
through `waitForEvent` instead (`:168-177`, wait at about `:2005-2036`), which looks the same in
a backtrace. What tells them apart is not the backtrace but the guest side: the program counter
the scheduler publishes (`publishIdleDebugContext`) or a counter of calls in the loop (journal
002 counted 514 `SifBindRpc` calls).

A host that owns the window on the main thread, as SDL3 and the PCSX2 GS want (D4, D9), matches
the existing split: the guest already runs on a separate thread. What has to be replaced is
what the main-thread loop does, not which thread it is on.

## Gamepad

### Pad: from the device to the bytes the game reads

| Step | What happens | Source |
|---|---|---|
| 1 | The guest calls `scePadPortOpen(port, slot, buf)`. The handler stores `buf` in the port's `dmaAddr` and zeroes 32 bytes there. | `src/lib/Kernel/Stubs/Pad.cpp:598-625` |
| 2 | `scePadRead(port, slot, buf)` calls `readPadPortData`. | `:627-640` |
| 3 | If an override is set (`setPadOverrideState`, a test/debug hook exposed through `ps2_call_list.h`), its state is used. | `:280-296`, `:846-855`; `include/ps2_call_list.h:691-693` |
| 4 | Otherwise `runtime->padBackend().readState(port, slot, tmp, 32)` is called (`PSPadBackend`, a concrete class). Its result is read as buttons (bytes 2-3) and sticks (bytes 4-7). | `:300-310`; `include/runtime/ps2_pad.h:7-14` |
| 5 | `fillPadStatus` writes the 32-byte buffer. | `:245-267` |

The buffer the handlers fill (`fillPadStatus`, **Measured**):

| Offset | Content |
|---|---|
| 0 | `0` (the PS2 "status OK" byte) |
| 1 | Mode byte: `0x41` digital or `0x73` DualShock (analog). A port opens digital (`analogMode=false`); `scePadSetMainMode` switches |
| 2-3 | Buttons, active-low, little-endian: Select `1<<0`, L3 `1<<1`, R3 `1<<2`, Start `1<<3`, Up `1<<4`, Right `1<<5`, Down `1<<6`, Left `1<<7`, L2 `1<<8`, R2 `1<<9`, L1 `1<<10`, R1 `1<<11`, Triangle `1<<12`, Circle `1<<13`, Cross `1<<14`, Square `1<<15` |
| 4-7 | Right X, right Y, left X, left Y (0..255, 0x80 centre) |
| 8-19 | Pressure bytes, only when `scePadEnterPressMode` was called and the button mask allows it; digital 0 or 255 (Right, Left, Up, Down, Triangle, Circle, Cross, Square, L1, L2, R1, R2). No real analog pressure. |
| 20-31 | Zero |

Source: `src/lib/Kernel/Stubs/Pad.cpp:245-267`, button masks `:20-35`.

### Ports, slots, multitap

| Question | Answer | Source |
|---|---|---|
| Ports supported | 2 (`kPadPortCount = 2`). `scePadGetPortMax` returns 2. A port number outside 0-1 makes `lookupPadPortStateLocked` return `nullptr`, so `scePadPortOpen` returns 0 and `scePadRead` returns 0. | `src/lib/Kernel/Stubs/Pad.cpp:16,203-213,428-434` |
| Slots supported | 1 (`kPadSlotCount = 1`). `scePadGetSlotMax` returns 1 with the comment "Most games use one slot unless multitap is active". A slot other than 0 is rejected the same way. | `:17,203-213,445-451` |
| Does the device depend on port/slot? | **No.** `PSPadBackend::readState(int, int, ...)` ignores both arguments and always reads gamepad index 0 or the keyboard. Both ports would return the same input. The second copy in `Pad.cpp` always picks the first available gamepad of 4. | `src/lib/ps2_pad.cpp:29,40-42`; `src/lib/Kernel/Stubs/Pad.cpp:85-95` |
| Multitap functions `sceMtap*` | **Not handled by the runtime**: no `sceMtap` symbol anywhere in `src/` or `include/` (the only matches are in the analyzer's symbol database, `../ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h`). If a game function with that name is recompiled it runs as guest code that talks to `mtapman` over SIF RPC; if it is stubbed it reaches `Kernel/Stubs/Unimplemented.cpp`, which throws after printing a warning. Which of the two happens depends on `config/stub_policy.toml`, not on the runtime. **Measured** for the absence; the consequence is **Unverified** (this is task T-306). | `src/lib/Kernel/Stubs/Unimplemented.cpp:32-47`; `docs/knowledge/05-audio-video-input.md:89-91` |
| `padman`, `sio2man` | Names accepted by the IOP module manager as built-in ROM modules (no real code run). `mtapman` is not in that list. Whether `mtapman` loads is **Unverified**. | `../ps2xIOP/src/iop_module_manager.cpp:20-52` |

### Pad: two copies of the raylib mapping

`11-stub-policy.md` records that the `scePad*` handlers read raylib directly. **Confirmed**,
with a precision:

- `Pad.cpp:85-185` (`findFirstGamepad`, `applyGamepadState`, `applyKeyboardState`) calls
  `IsGamepadAvailable`, `IsWindowReady`, `GetGamepadAxisMovement` (4 axes),
  `IsGamepadButtonDown` (16 buttons) and `IsKeyDown` (arrows, Z/X/C/V, Q/E, 1/3, Enter,
  Right Shift, both Ctrl, WASD for the left stick).
- `Pad.cpp:651-661` calls `IsGamepadButtonDown` and `IsKeyDown` again, but only inside
  `PS2_IF_AGRESSIVE_LOGS`.
- `ps2_pad.cpp:42-118` has a second, slightly different mapping (keyboard Z/X/C/V, Space,
  Escape, Tab, keypad 0/1...).
- **`PSPadBackend::readState` returns `true` whenever its buffer is at least 32 bytes**
  (`src/lib/ps2_pad.cpp:31-32,122`), and `Pad.cpp:302` always passes 32. So the fallback in
  `Pad.cpp:311-315` (`applyGamepadState`/`applyKeyboardState`) is **not reached** in normal
  operation, and the mapping in force is the one in `ps2_pad.cpp`. **Measured** by reading both
  files; not executed.
- **Rumble:** *(corrected 2026-10-14, T-308)* the two calls used to return 1 and do nothing; patch 0066 keeps the
  motors per (port, slot) and reports them to the host ([05](05-audio-video-input.md), "Vibration"). `scePadInfoAct` is a query (2 actuators).

### What would have to change for four players

Four players need four pad slots on the PS2 side (a multitap on port 0 gives slots 0-3, or
the game addresses ports 0 and 1 plus a tap). In the runtime:

1. `kPadPortCount`/`kPadSlotCount` (and `scePadGetPortMax`/`GetSlotMax`) limit the pad table
   to 2x1 (`Pad.cpp:16-17,428-451`). Both constants and the two getters need to follow what the
   game's multitap code expects. **Needs a patch.**
2. `PSPadBackend::readState` has to route by `(port, slot)` to a device index. Today it
   ignores both (`ps2_pad.cpp:29`). **Needs a patch** (the class is concrete and compiled into
   the runtime).
3. `sceMtapGetConnection`/`sceMtapPortOpen`/`sceMtapGetSlotNum` and the SIF RPC side must
   return "tap present, 4 slots" (T-306). Nothing exists in the runtime for it. **Unverified**
   whether the game's own recompiled `libmtap` plus `mtapman` is enough once the IOP runs the
   real module. If it is not, the three functions need HLE handlers (`Stubs/Pad.cpp`) or an
   override in `src/game/don_overrides.cpp`.
4. `scePadInfoMode`, `scePadGetState`, `scePadInfoComb` per slot (`Pad.cpp:496-560`) already
   index by port; with `kPadSlotCount > 1` they work per slot, but the connection state of
   each slot has to come from the host.

## Audio

| Question | Answer | Source |
|---|---|---|
| Is there an SPU2 model? | **No.** The only SPU2 representation in the tree is IOP-side MMIO storage: a `std::unordered_map` of 32-bit words for the range `0x1F900000-0x1FA00000` (reads return what was written), plus a faked "transfer complete" flag and interrupt when the guest starts an SPU DMA (`CHCR` start bit). No voices, no envelopes, no reverb, no mixing, no sample memory. | `../ps2xIOP/src/emulator/core/iop_memory.h:21-22`; `../ps2xIOP/src/emulator/core/iop_memory.cpp:205-216,246-276` |
| What produces samples today? | `PS2AudioBackend`: whole-sample playback of PCM decoded from VAG data, played by raylib. Not a mixer of SPU2 voices. | `src/lib/ps2_audio.cpp:110-328` |
| Is ADPCM decoded? | Yes, on the host, one VAG file at a time: `ps2_vag::decode` reads a `VAGp` header (sample rate from header offset `0x10`, 44100 if zero) and decodes the 28-sample ADPCM blocks to mono `int16_t`. It runs when the guest writes an `.vag`-looking buffer through `fio` (`onVagTransferFromBuffer`) or when a transfer is announced (`onVagTransfer`). Loop flags and multi-voice streaming are not modelled (**Unverified** for the loop flags; the decoder file is `src/lib/ps2_audio_vag.cpp`, 27-100). | `src/lib/ps2_audio_vag.cpp:27-100`; `src/lib/Kernel/Syscalls/FileIO.cpp:60-90` |
| Where do the `sceSd*` remote calls end? | `sceSdRemote` (EE side) is a stub handler that only keeps bookkeeping (voice transfer and block-transfer state, return values for `0x80D0`/`0x80E0`/...) and does not call the audio backend. `sceSdRemoteInit` resets that state. `sceSdTransToIOP` is `TODO_NAMED`. The RPC server side is a separate path: IOP `libsd` HLE service (SID `0x80000701`) calls `IopHost::audioCommand`, which `PS2IopHostAdapter` forwards to `PS2AudioBackend::onSoundCommand`. That function only reacts to SID `0x80000701` and to command `LIBSD_CMD_SET_VOICE` or `0x81xx`, guesses a voice index and sample address by scanning the send buffer, and calls `play(...)`. | `src/lib/Kernel/Stubs/Audio.cpp:77-210`; `../ps2xIOP/src/modules/libsd.cpp:35-55`; `src/lib/ps2_iop_host.cpp:247-266`; `src/lib/ps2_audio.cpp:150-195` |
| Is the real `libsd`/`sdrdrv` IRX run? | **Declared** by `../ps2xIOP/README.md`: "A physical IRX RPC server is authoritative for its SID. Generic HLE services remain available when no loaded IRX provides an endpoint", and LIBSD is one of the HLE fallbacks (activation: "Recognized module load"). Whether the game's own sound IRX (not `libsd` itself, see `docs/knowledge/05-audio-video-input.md`) ends in this path was not traced here. **Unverified.** | `../ps2xIOP/README.md` (execution policy) |
| Where is the output stream created? | `InitAudioDevice()` in `PS2Runtime::initialize()`, on the main thread, followed by `m_audioBackend.setAudioReady(IsAudioDeviceReady())`. There is **no raylib `AudioStream`** and no callback: playback is `LoadSoundFromWave` + `PlaySound` per sample, and raylib/miniaudio does its own mixing. Maximum 4 concurrent sounds; a sample longer than 5 s is treated as BGM and stops the others. | `src/lib/ps2_runtime.cpp:736-737`; `src/lib/ps2_audio.cpp:225-306` (the `kMaxConcurrentSounds`, `isBgm` and `PlaySound` calls) |
| Other audio | MPEG/FMV audio: the MPEG stubs parse the PS2 stream (`isAudioStreamId`, private stream 1) but no raylib audio call appears in them; where that audio ends is **Unverified**. | `src/lib/Kernel/Stubs/MPEG.cpp:831,1342` |

## Where a host-provided GS, input source and audio sink would plug in

| Piece | Existing seam | Is it an interface? | Source |
|---|---|---|---|
| GS rasteriser | `GSRasterBackend` (pure virtual: `Submit`, `Flush`, `Present`, `Sync`, `UploadImage`, `ReadVram`...). `GS` owns `std::unique_ptr<GSRasterBackend> m_backend`. | The interface exists, but **the choice is hard-coded**: the constructor and `init()` both do `std::make_unique<GSCpuBackend>()` and there is no setter. | `include/runtime/gs/gs_backend.h:8-37`; `src/lib/gs/gs_frontend.cpp:108-122`; `include/runtime/gs/gs_frontend.h:244` |
| Frame to the screen | `GS::latchHostPresentationFrame()` / `copyLatchedHostPresentationFrame()` give a CPU RGBA buffer. The raylib upload is a `static` function in `ps2_runtime.cpp`. | Not an interface; a PCSX2 GS that renders to a native window never produces this buffer, so the whole `UploadFrame` + `Begin/EndDrawing` block is bypassed. | `src/lib/gs/gs_frontend.cpp:527-640`; `src/lib/ps2_runtime.cpp:376-475,2423-2447` |
| Input | `PS2Runtime::padBackend()` returns a `PSPadBackend&`; `readState(port, slot, data, size)` is the single point where the host device is read. | A concrete, non-virtual class. The `setPadOverrideState`/`clearPadOverrideState` pair is a process-wide hook that bypasses it. | `include/ps2_runtime.h:452-453,497`; `include/runtime/ps2_pad.h`; `src/lib/Kernel/Stubs/Pad.cpp:846-860` |
| Audio | `PS2Runtime::audioBackend()` returns a `PS2AudioBackend&`; the IOP side reaches it through `PS2IopHostAdapter::audioCommand`. The IOP interface `ps2x::iop::IopHost::audioCommand(...)` is virtual. | `IopHost` is a real interface (`../ps2xIOP/include/ps2x/iop/iop_host.h:97`), but `PS2IopHostAdapter` is created inside the `PS2Runtime` constructor (`src/lib/ps2_runtime.cpp:482`) and calls the concrete `PS2AudioBackend`. | `include/ps2_runtime.h:450-451,496`; `src/lib/ps2_iop_host.cpp:247-266` |
| Window / loop | None. `run()` is monolithic. The only extension points are the three `DebugUiCallback` function pointers (`setDebugUiCallbacks`), invoked from inside the raylib draw (`init` in `initialize()`, `draw` between `DrawTexturePro` and `EndDrawing`, `shutdown`). They run on the main thread and allow drawing with ImGui, not replacing the loop. | Function pointers only. | `include/ps2_runtime.h:304-309,522-524`; `src/lib/ps2_runtime.cpp:742,2445` |

## What a host program of our own needs

Ordered by dependency: each row assumes the rows above it are done. D4 gives the GS, D9 gives
SDL3 for window/pad/audio, D6 gives SPU2 for sound. "Hook" means something that exists in the
tree today and that a program linking `ps2_runtime` can use without editing it.

| # | File and function | Kind of change | Possible without patching? |
|---|---|---|---|
| 1 | `src/lib/ps2_runtime.cpp`, `PS2Runtime::initialize()` (`:732-739`) | Stop calling `SetConfigFlags`/`InitWindow`/`InitAudioDevice`/`SetTargetFPS`; add a way to say "the host owns the window" (a flag or a callback set before `initialize`). | **Needs a patch.** There is no hook; raylib is called unconditionally. Calling the existing `initialize()` from our host would open a raylib window next to ours. |
| 2 | `src/lib/ps2_runtime.cpp`, `PS2Runtime::run()` (`:2342-2469`) | Split the function: start `GameThread` and return (or call back into the host once per iteration); remove `UploadFrame`, the texture, `BeginDrawing`...`EndDrawing`, `WindowShouldClose` and `CloseWindow` from the default path, keep them for the runtime's own executable. The host's loop (SDL event pump, PCSX2 GS present) takes the place of the removed block. | **Needs a patch.** The only other way (the host re-implements the body of `run()`, using `m_eeScheduler`, `m_cpuContext`, ...) is impossible: they are private members. Public pieces that would help (`requestStop`, `eeScheduler()`, `currentVSyncTick`) already exist (`include/ps2_runtime.h:390`; `src/lib/ps2_runtime.cpp:393`). |
| 3 | `src/lib/gs/gs_frontend.cpp`, `GS::GS()` and `GS::init()` (`:108-122`) | Add `GS::setBackend(std::unique_ptr<GSRasterBackend>)` (or a factory registered before `init`), so the PCSX2 bridge can be injected. | **Needs a patch.** `GSRasterBackend` is public, but with the backend hard-coded the only workaround is to subclass `GS` (not virtual-friendly, the runtime owns it by value) or to replace `GSCpuBackend`'s source file, which would be a change outside `patches/ps2recomp/`. |
| 4 | `include/runtime/gs/gs_backend.h`, `GSRasterBackend` | Check that the interface suffices for the PCSX2 bridge: it is **primitive-batch based** (`Submit(GSPrimitiveBatch)`), not register-level. The PCSX2 GS consumes GIF packets/registers, so either the bridge converts batches (lossy) or the interface needs a register-level seam in `GS`'s write path. **Unverified**, to be settled in the GS integration task, not here. | Possibly a patch; depends on the decision just above. |
| 5 | `src/lib/ps2_pad.cpp`, `PSPadBackend::readState` (`:29-123`), `include/runtime/ps2_pad.h` | Replace the raylib body with a host-provided source: either make `readState` virtual / a stored `std::function<bool(int port, int slot, uint8_t*, size_t)>`, or add `PSPadBackend::setSource(...)`. Then the host calls SDL3 on the window thread, keeps four atomic states, and the callback copies the one for `(port, slot)`. Remove the `raylib` calls from this file. | **Partly possible without a patch:** `setPadOverrideState(buttons, lx, ly, rx, ry)` can be called (declared in `include/ps2_call_list.h:691`, defined `Pad.cpp:846`) from the host every frame, and `readPadPortData` then skips `PSPadBackend`. But it is **one global state for all ports**, so it works for one player and cannot give four. For anything beyond P1 it **needs a patch**. Also the raylib calls remain compiled in (see #7). |
| 6 | `src/lib/Kernel/Stubs/Pad.cpp`: `kPadPortCount`, `kPadSlotCount`, `scePadGetPortMax`, `scePadGetSlotMax` (`:16-17,428-451`), `applyGamepadState`/`applyKeyboardState` (`:85-185`), the log block (`:651-661`) | Port/slot counts to match the game's multitap use; per-slot connection state from the host; delete or neutralise the dead raylib copy so the file no longer needs raylib. | **Needs a patch.** Constants are in an anonymous namespace. A game-specific alternative that avoids the patch: replace `scePadRead`/`scePadGetState`... in `src/game/don_overrides.cpp` for the functions the game really calls (**Unverified** which; T-306), but that duplicates handler logic. |
| 7 | `CMakeLists.txt` (`:79-84,132,238,509`), `include/ps2_host_backend.h` | Make raylib optional: `PS2X_HOST_BACKEND=raylib\|none` that replaces the `#include "raylib.h"` by nothing. Without it, linking `ps2_runtime` always pulls raylib in (FetchContent from GitHub) and exposes its global names (`CloseWindow` on MSVC, `Color`...). | **Needs a patch.** An external project can use `ps2_runtime` as it is and ignore raylib: then raylib's `InitWindow` is never called if #1 is patched, and the only cost is a dead dependency. That is acceptable for a first host, but not for the Windows build (name clash, `CMakeLists.txt:575`). |
| 8 | `src/lib/ps2_audio.cpp`, `PS2AudioBackend` and `src/lib/ps2_iop_host.cpp:247-266` | Replace the sample-playback heuristic with the SPU2 bridge (D6): (a) the IOP's SPU2 MMIO range and the SPU DMA hook in `../ps2xIOP/src/emulator/core/iop_memory.cpp:205-276` must route reads/writes to the PCSX2 SPU2 core, (b) the SPU2 output (48 kHz stereo `int16_t`) must go to an SDL3 audio stream, (c) `onSoundCommand`/`onVagTransfer*` are left unused. For (b) alone, the existing seam is `IopHost::audioCommand`; for (a) there is none. | **(b) is possible without patching the runtime:** the host owns an SDL3 stream and pulls samples from the SPU2 bridge, and never calls `InitAudioDevice`. **(a) needs a patch to `patches/ps2recomp/`** (IOP memory hook), as the card of T-101 already assumes (D6: "connected through a hook in the emulator"). The tick that drives the SPU2 core has to come from `advanceIopEeCycles` (`src/lib/ps2_runtime.cpp:608`), already on the EE executor. |

Combined classification:

| Possible without patching the runtime | Needs a patch in `patches/ps2recomp/` |
|---|---|
| Linking `ps2_runtime` and `ps2_iop` from our own program (CMake targets exist: `CMakeLists.txt:509,519`) | #1 host owns the window and audio device |
| Reading `eeScheduler().currentVSyncTick()` and `requestStop()` from the host | #2 `run()` split / callback loop |
| Pad for **one** player through `setPadOverrideState` (global, all ports) | #3 GS backend injection |
| Pulling audio from the SPU2 bridge into an SDL3 stream (output side) | #5 per-`(port, slot)` pad source |
| Supplying `IopHost` callbacks (`../ps2xIOP/include/ps2x/iop/iop_host.h`) *only* by replacing the whole `PS2IopHostAdapter`, which is built inside the constructor and therefore is **not** possible | #6 pad counts and raylib-free `Pad.cpp` |
| | #7 optional raylib in CMake |
| | #8(a) SPU2 MMIO hook in the IOP memory |

Each of these patches (#1, #2, #3, #5, #6, #7, #8a) is one topic, as AGENTS.md asks.

## What was written (patches 0004 to 0009, 2026-10-11)

The changes of the table above exist as patches in `patches/ps2recomp/` (all in `series`). They
were written and tested in scratch worktrees; each has a test under `tests/runtime/` that needs
no window. **Measured** by the orchestrator in a scratch tree: the chain 0001 to 0009 applies to
the pinned commit, the runtime builds with `PS2X_WITH_RAYLIB=OFF` with the network disconnected,
and the five tests pass with `DISPLAY` unset.

| Change | Patch | What it adds | Not exercised yet |
|---|---|---|---|
| #1, #2 | `0007-host-owned-window-and-loop` | `PS2Runtime::setHostOwnsWindow(bool)` before `initialize()`: no window, no audio device, no frame cap. `startGameThread()`, `isGameThreadRunning()`, `gameThreadFailed()`, `stopGameThread()`; `run()` is those calls plus the old raylib loop. Fixes a race: a stop requested right after the start was cleared by `EeScheduler::reset()`. | The runtime's own raylib loop after the split (it needs a display) |
| #3, #4 | `0005-host-gs-hook` | Not a `GSRasterBackend`: the seam is at the GIF arbiter, at register level. `PS2Runtime::setHostGs(const Ps2HostGs*)` with `gif_transfer(path, data, bytes)`, `vsync(priv[19], field)`, `reset`, `read_fifo`, `thread_begin`, `thread_end`. A per-path scanner keeps SIGNAL, FINISH and LABEL working in the runtime. | The real GS library behind it; VU1 `XGKICK` on path 1 with a real microprogram; the cost of the scanner |
| #5, #6 | `0008-host-pad-source` | `PS2Runtime::setHostPad(const Ps2HostPad*)`: `port_count` (up to 2), `slots_per_port` (up to 4), `read(port, slot, out)`. Pad state is kept per `(port, slot)`; `scePadGetPortMax`/`scePadGetSlotMax` follow the source. | The bytes reported for a disconnected pad (status `0xFF`, buttons `0xFF 0xFF`, sticks `0x80`) are an assumption (**Unverified**) |
| #7 | `0009-optional-raylib` | CMake option `PS2X_WITH_RAYLIB` (default ON). OFF: nothing is fetched, no raylib, imgui or rlImGui symbol in the library, `ps2EntryRunner` is not built, and `initialize()` without `setHostOwnsWindow(true)` fails with a message. | Windows |
| #8(a) | `0006-iop-host-spu2` | `iopSetHostSpu2(const IopHostSpu2*)` in the IOP emulator: SPU2 registers, DMA channels 4 and 7, interrupts, and `advance` in IOP cycles (one emulator cycle is one cycle of the 36.864 MHz clock). The high-level sound service is not driven while a host SPU2 is installed. | The game's sound modules; streaming DMA |
| (boot) | `0004-iop-load-module-buffer-memory` | A module loaded from a buffer is read from IOP memory, where the game put it, not from main memory. | A real run with the game's fourteen modules |

Threads (**Measured** by `host_loop_test`): `thread_begin`, every `gif_transfer`, `vsync` and
`thread_end` run on the game thread (`GameThread`), in that order, never on the thread that
called `startGameThread()`. The pad `read` callback also runs on the game thread, inside the pad
calls: the host must only copy a state that its own thread already published. When the guest's
main thread ends, the scheduler keeps running and vertical blanks keep arriving, so
`isGameThreadRunning()` stays true until `stopGameThread()`.

Order of calls for the host program (**Declared** by the patches, exercised by the tests with
host functions in place of the game):

1. construct `PS2Runtime`; `setHostOwnsWindow(true)`; `initialize(...)`;
2. `setHostGs(&gs)`; `setHostPad(&pad)`; `iopSetHostSpu2(&spu2)` (before any module is loaded);
3. `loadELF(...)`; `startGameThread()`;
4. the host's loop: events, publish the pad states, present after the GS `vsync` callback, poll
   `isGameThreadRunning()` and `gameThreadFailed()`;
5. `stopGameThread()`; `iopSetHostSpu2(nullptr)` before closing the SPU2 library; destructors.

The GS library must be opened in `thread_begin` and closed in `thread_end`, because it requires
every call from the thread that opened it ([12-building-pcsx2.md](12-building-pcsx2.md)).

## The host program (T-110 part 2, 2026-10-11)

`src/host/` is the host program as a static library, `don_host`, plus `main.cpp`. Entry:
`don_host_run(argc, argv, const DonHostGame&)` (`host.h`); `DonHostGame` says how the guest is
set up, so the same program runs the real game (`needs_elf`) or a test's synthetic guest.
`tests/host/host_synthetic_test` is that program with a guest made of host functions that sets
up the display through the runtime's `sceGs*` handlers and sends GIF packets every frame;
`tools/pipeline/build_host.sh` builds it in the container (`DON_PS2RECOMP_DIR` selects the
PS2Recomp tree). Not yet linked with the real game.

**Measured** (orchestrator, 2026-10-11): the build, `--headless --seconds 5 --shots` (exit 0, 10
PNGs of 640x448 with the scene's 7 colours), exit code 2 for a bad option, 3 for a missing GS
library, and the 9 tests of `tests/host` (both renderers headless; a window on a virtual display
with Mesa's software Vulkan; exit code 4 when the guest throws).

- The GS library is loaded at run time (`SDL_LoadObject`), not linked: from `--gs-dir`, else
  `DON_GS_DIR`, else next to the executable, `gs/` or `../gs/`.
- Main thread: the SDL window, the events, the loop. Game thread: the GS is opened in
  `thread_begin` and closed in `thread_end`; every GS call, the pad read and the screenshots
  happen there. A resize crosses from the main thread through an atomic.
- First build of the runtime with clang 17 (Linux, in the container): 34 s from clean at `-j6`,
  no errors, 7 warnings, all in `runtime/gs/ps2_gs_memory.h`.
- Cost of the synthetic scene at 60 vertical blanks per second, headless: 1 % of a core with the
  Vulkan renderer, 7 % with the software renderer. In a window on the virtual display about 70 %,
  which is the software Vulkan driver, not the program.
- Not verified: the real game, a real GPU presenting to a window, Wayland, Windows, a resize
  while running, input events.

### The replaced `sceGs*` handlers against a real GS (**Measured** with the synthetic guest)

The runtime's handlers were written for the runtime's own GS. With the PCSX2 GS behind them:

1. `sceGsResetGraph` sends one A+D packet with addresses `0x41`, `0x42`, `0x59`-`0x5C` and
   `0x5F`. For the runtime these are stand-ins for privileged registers (the hook's scanner
   applies them), but `0x41` and `0x42` are real GS registers, SCISSOR_2 and ALPHA_1, and the
   PCSX2 GS applies them as such: a rectangle drawn in context 2 was clipped away and a blended
   one came out with the wrong colour until the guest set those registers again. `0x59`-`0x5F`
   are ignored by the PCSX2 GS.
2. No handler writes SMODE1, and `sceGsSetDefDispEnv`/`sceGsPutDispEnv` write DISPLAY with
   MAGH = 0 and DW = width - 1. The PCSX2 GS then shows the picture four times too narrow. The
   test's guest writes SMODE1 and DISPLAY itself to get a correct picture.
3. `sceGsResetGraph` passes its `ffmode` argument straight into SMODE2; with 1 the picture came
   out doubled vertically.

The game does go through these handlers (**Measured**, `tools/re/callers.py`): the boot
(`0x136770`) calls `sceGsResetGraph`, `sceGsSetDefDBuff` and `sceGsPutDrawEnv`; the engine's
video open (`0x2DF720`, `0x2E0760`) calls `sceGsResetGraph` and `sceGsSetDefDispEnv`, and
`0x2D3FD0` calls `sceGsPutDispEnv`; the movie player (`0x36A820`) calls `sceGsResetGraph`,
`sceGsSetDefDBuff` and `sceGsSwapDBuff`. So points 1 to 3 will show in the first picture of the
real game. Question Q-GS of [11-stub-policy.md](11-stub-policy.md) is open again.

(Resolved on 2026-10-12 by patch `0021-host-gs-display-registers`: points 1 to 3 are corrected in the handlers when a host GS is installed, and the stop-gap described next was removed; see [18-display-registers.md](18-display-registers.md).)

For point 1 the host program had a stop-gap, `filter_reset_graph` in `src/host/gs_glue.cpp`
(off with `DON_HOST_GS_FILTER=0`): it drops those entries from an A+D tag that contains one of
`0x59`-`0x5F`. It reads each transfer as if it started at a GIF tag. That is true of the
handler's packet, but **not guaranteed** for a transfer that continues the data of a previous
one (texture pixels of a DMA chain), where pixel bytes could be taken for such a tag and data
dropped. It must not stay: the fix belongs where the packet is made (the handler), or the
handlers are replaced by the game's own code.

## The runtime's own memory inside the guest's RAM (patch 0032, 2026-10-12)

The runtime needs guest *addresses* for structures of its own: the invocation stacks of queued guest
calls (`PS2Runtime::reserveAsyncCallbackStack` through `EeScheduler::invocationStackTop`), the
remote-call packet and server pools, the thread-local-storage pool, the boot-mode pool and the
buffers behind `guestMalloc`/`guestCalloc`/`guestRealloc` (the GS, font and LibC handlers allocate
through it). Until patch 0032 they lived at `0x01F00000` and above, and two collisions followed:
`guestMalloc` handed out blocks from the base of the *game's* own heap (`0x5A6800`, the same
addresses the game's recompiled C library manages, first arena `0x5A6840`), because `SetupHeap`
passed the game's range to `configureGuestHeap`; and the invocation stacks grew down from the top of
RAM (`0x02000000`) over the main thread's stack (`0x01F80000-0x02000000`). To leave room, the
handler clamped the game's heap to `0x01F00000`, ~515 KiB below the console's end.

The console's kernel occupies `0x00080000` to `0x00100000` (measured on six console memory images:
`0x00080000-0x0009FFFF` in use, `0x000A0000-0x000FFFFF` all zeroes), that kernel does not exist in
this runtime, and nothing of the game is below `0x00100000` (the ELF's first segment is there). The
runtime's own structures therefore live in that area, defined in one place,
`include/runtime/ps2_guest_memory.h`:

    0x00080000-0x000BFFFF  invocation stacks: 16 of 0x4000, bumped downwards from 0x000C0000, never freed
    0x000C0000-0x000CFFFF  remote-call packet pool (wraps, unchanged)
    0x000D0000-0x000DFFFF  remote-call server pool (wraps, unchanged)
    0x000E0000-0x000EFFFF  thread-local-storage pool (wraps, unchanged)
    0x000F0000-0x000F0FFF  boot-mode pool (does not wrap)
    0x000F1000-0x000FFFFF  the runtime's scratch allocator (guestMalloc and friends)

The rules that go with the table: the constants are in one header, with `static_assert`s that no
region overlaps another and that all of them end at or below `0x00100000`; `SetupHeap`/`EndOfHeap`
answer the console's kernel (`start + size`, or the calling thread's stack bottom for `-1`) and never
hand the game's heap to the runtime; `SetupHeap`'s return value is unchanged because the game's entry
code ignores it; a region that does not wrap prints one line naming the region and the size asked
when it cannot serve a request (the packet, server and TLS pools wrap by design). Two capacities to
keep in mind: **16 invocation stacks per process** (4 are used in the 60 s runs measured) and
**0xF000 bytes** for the runtime's allocator — a game that lets `CreateThread` allocate its stacks or
calls the font handlers (0x2010/0xc400 bytes) uses much more of the region than this one does, and
the log line is what tells the next person (the runtime's `malloc` family of
`Kernel/Stubs/LibC.cpp`/`Compatibility.cpp` allocates from the same private range, so a game that
relied on those handlers for its whole heap would need the pre-0032 behaviour).

## Command-line options and `DON_` environment variables (2026-10-13)

Source of both tables: the code named in each row (read 2026-10-13, **Measured** by reading, not by a run per row).
Diagnostics are off by default and print lines prefixed `[don]`, `[don-gs]`, `[don-vu]`, `[don-spu2]` (AGENTS.md).

### Command-line options (`src/host/options.cpp`, `don_print_usage`)

| Option | Effect | Default |
|---|---|---|
| `--iso <path>` | disc image; same as `DON_ISO` | the environment variable |
| `--mc-dir <folder>` | folder that holds the cards `mc0`, `mc1`; same as `DON_MC_DIR` | next to the ELF given |
| `--headless` | no window; the GS opens without a surface | off |
| `--seconds <n>` | end after n seconds with exit code 0 | run until closed |
| `--shots <folder>` | save PNG screenshots in this folder (created if missing) | none |
| `--shot-every <frames>` | one screenshot every n vertical blanks | 60 with `--shots` |
| `--input-script <file>` | scripted input: pad states, shots and quit at numbered frames (`docs/INPUT_SCRIPTS.md`) | none |
| `--renderer vulkan\|software` | GS renderer | vulkan |
| `--upscale <n>` | internal resolution multiplier 1..8 (hardware renderer only) | 1 |
| `--gs-dir <folder>` | folder with the GS library | `DON_GS_DIR`, else next to the executable |
| `--vu-dir <folder>` | folder with the VU1 library | `DON_VU_DIR`, else next to the executable |
| `--sound on\|off` | sound through the SPU2 library; with `off` nothing of it is loaded; a missing library or audio device is one `[don]` line and no sound, unless `--sound on` was given (then exit 3) | on (since 2026-10-14) |
| `--spu2-dir <folder>` | folder with the SPU2 library | `DON_SPU2_DIR`, else next to the executable |
| `--wav <file>` | write the SPU2 output to this WAV (an error with `--sound off`) | none |
| `--help` | the usage text | |

Exit codes: 0 normal end or end of `--seconds`; 2 argument error; 3 the GS (or the window), or the SPU2 library with
`--sound on`, could not be opened; 4 the runtime ended with an error. (The default of `--upscale` is not in the usage
text; 1 is the value in `options.h`.)

### `DON_` variables read by the port itself

"Flag" means the value must be exactly `1` (`envFlag` in `src/game/don_overrides.cpp`, `env_flag` in the host).

| Variable | Read in | Effect | Default |
|---|---|---|---|
| `DON_ISO` | `host.cpp`, `don_overrides.cpp` | disc image the runtime reads (`IoPaths::cdImage`) | unset; the launcher removes it from the environment of the port when it uses a variant's folder (journal 021) |
| `DON_MC_DIR` | `host.cpp` | folder of the cards: `mcRoot = <dir>/mc0` after `loadELF` (journal 022) | next to the ELF |
| `DON_PACING_LOG` | `gs_glue.cpp`, `host.cpp` | T-305: per vertical blank, the wall time since the previous one split into game-thread work, renderer (the GS library's vsync call) and pacing wait (patch 0056); at the end of the run the median, p99, p99.9, counts later than 20/33/50 ms and the ten worst. Any value other than empty or `0` turns it on; a number N records from blank N on (not a pure flag) | off |
| `DON_GS_DIR`, `DON_VU_DIR`, `DON_SPU2_DIR` | `gs_glue.cpp`, `vu_glue.cpp`, `spu2_glue.cpp` | folder of each library | next to the executable |
| `DON_GS_DATA_DIR` | `gsbridge/don_gs_host.cpp` | PCSX2 data directory of the GS library | `<library folder>/data` |
| `DON_GS_LOG` | `gsbridge/don_gs_host.cpp` | flag: the GS library's log at debug level on stderr | errors only |
| `DON_HOST_LOG` | `host.cpp`, `gs_glue.cpp`, `screenshots.cpp` | flag: the host says what it opens and when a screenshot is skipped | off |
| `DON_HOST_VSYNC` | `host.cpp` | `1` makes the window's present wait for the display (FIFO, the behaviour before 2026-10-14); unset or any other value presents without waiting (mailbox where the driver offers it) and the loop's own clock is the only pace (journal 031) | off (no wait) |
| `DON_VU1` | `vu_glue.cpp` | `microvu`, `runtime` (or `interp`: the runtime's own interpreter), `pcsx2interp`; anything else warns and takes `runtime` | **microvu** since T-205 (journal 008) |
| `DON_FP_MODE` | `fp_env.cpp` | `console` or `host`: the floating-point mode of the game thread (section "The game thread's floating-point mode") | console |
| `DON_PLAY_MOVIES` | `don_overrides.cpp` | flag: the game's own video player runs and the port presses nothing for it; unset means the port skips both videos (journals 023, 024) | skip |
| `DON_TTY` | `don_overrides.cpp` | flag: the game's text for descriptors 1 and 2 goes to stderr with `[don-tty] ` before each line | dropped |
| `DON_TRACE_FRAMES` | `don_overrides.cpp` | flag: installs the frame counters (with any of the three below and `DON_TRACE_STATE`) | off |
| `DON_TRACE_THREADS` | `don_overrides.cpp` (and `EeScheduler.cpp`) | flag: one `[don-threads]` dump of threads and semaphores every 60 blanks | off |
| `DON_TRACE_IOP` | `don_overrides.cpp` | flag: one `[don-iop]` line every 60 blanks with the IOP's cycles, instructions and services (T-111) | off |
| `DON_IOP_DUMP` | `don_overrides.cpp` | an IOP address: dumps 0x100 bytes of IOP RAM once | off |
| `DON_TRACE_STATE` | `don_overrides.cpp` | a number n: every n blanks one line with the hash of guest RAM and the cycle counters; also installs the frame counters (journal 014) | off |
| `DON_STATE_DUMP`, `DON_STATE_DUMP_AT` | `don_overrides.cpp` | file and blank: writes the 32 MiB of guest RAM when that blank's state line is printed (needs `DON_TRACE_STATE`; game memory, so under `work/`) | off |
| `DON_DUMP_RAM` | `host.cpp` | file: writes the guest RAM once the game thread has stopped (compare with `tools/verify/ram_diff.py`) | off |
| `DON_TRACE_GS_REGS` | `gs_glue.cpp` | flag: one `[don-gs]` line per change of the GS's privileged registers | off |
| `DON_GS_PACKETS`, `DON_GS_PACKETS_FRAME` | `gs_trace.cpp` | file and frame: records the GIF packets of that frame (without a frame, from the second blank until the run ends) | off |
| `DON_TRACE_SPU2` | `spu2_glue.cpp` | flag: what the IOP emulator asks of the chip, one `[don-spu2]` line per emulated second and a summary at the end (T-304) | off |
| `DON_AUDIO_LOG` | `audio.cpp` | flag: a summary line of the audio output at the end | off |
| `DON_AUDIO_LATENCY` | `host.cpp` | flag: every 5 s of host time one `[don-spu2] latency` line: sound queued for the device (ring + SDL stream, ms), frames produced per second, blanks per second, frames dropped | off |
| `DON_SPU2_CALLLOG` | `spu2_glue.cpp` | file: one line per advance (`A <chip cycle> <cycles>`), register write (`W <chip cycle> <addr> <value>`) and DMA (`D <chip cycle> <core> <bytes>`) of the sound chip, to compare two runs (large: about 140 MB per minute of game) | off |
| `DON_AUDIO_DEVICE` | `host.cpp` | flag: open the audio device in headless mode too (with `SDL_AUDIO_DRIVER=dummy` nothing is played; for measuring) | off |
| `DON_AUDIO_PACING` | `host.cpp` | `0` turns off the audio pacing described below (the behaviour before 2026-10-14) | on |
| `DON_AUDIO_TARGET_MS` | `host.cpp` | how much sound may be queued before the game thread waits, 10 to 1000 ms | 60 |
| `DON_VU_LOG` | `vu_glue.cpp` | flag: counts the XGKICK packets and bytes through a sink trampoline and prints a summary line | off |
| `DON_PROFILE` | `profile.cpp` | file: samples the game thread's addresses and writes them there (read with `tools/verify/profile_summary.py`) | off |
| `DON_IOP_BATCH_EE_CYCLES` | `EeScheduler.cpp` (patch 0033) | a number of EE cycles: forces the size of the batch given to the IOP; `8` restores the pre-0033 feed | one emulator slice, 2048 EE cycles |

`DON_AUDIO_SDL`, `DON_GS_BUILD`, `DON_SPU2_BUILD` and similar names found by the grep are compile definitions or
macros, not environment variables.

### `DON_` variables read by the tools

| Variable | Read in | Effect | Default |
|---|---|---|---|
| `DON_JOBS` | `tools/pipeline/build.sh` | parallel jobs of the build | 12 |
| `DON_CONTAINER_MEMORY` | `tools/container/run.sh`, `build.sh` | memory limit of the build container (`none` = no limit) | 55 % of MemTotal, whole GiB |
| `DON_BUILD_IMAGE`, `DON_BUILD_WIN_IMAGE` | `tools/container/run.sh`, `run_windows.sh` | container image | `don-build:1`, `don-build-win:1` |
| `DON_PS2RECOMP_DIR` | `tools/pipeline/build_host.sh` | PS2Recomp tree | `third_party/PS2Recomp` |
| `DON_GS_BRIDGE_SOURCE_DIR` | `tools/pipeline/build_gs.sh` | passed to CMake as the GS bridge's source | `src/gsbridge` |
| `DON_MANUAL_EXTRA` | `tools/pipeline/generate.sh` | extra manual-config file passed to `make_config.py --manual-extra` | none |
| `DON_BUILD_DIR` | `tools/run/smoke.sh`, `gdb_trace.sh` | where `don` is | `build/linux-dev` |
| `DON_GS_BUILD_DIR`, `DON_GS_DUMP_DIR` | `tools/verify/replay_all.sh` | GS library build and folder of `*.gs.zst` dumps | `build/gs`, `work/ref/pcsx2` |
| `DON_NM` | `tools/verify/profile_summary.py` | the `nm` program | `nm` |
| `DON_PYTHON` | `tools/launcher/launch.sh` | the interpreter for the launcher | `uv`, else `python3` |
| `DON_BIN`, `DON_DISC_DIR`, `DON_EN_JPVOICE`, `DON_EN_ENVOICE`, `DON_VARIANTS_DIR`, `DON_CONFIG_DIR` | `tools/launcher/launch.py` | the port's executable, the Japanese disc's folder, the two English variants' images or folders, where variant folders are made, the folder of `launcher.conf` | `build/linux-dev/don`, `disc`, `en_patch/jpvoice`, `en_patch/envoice` (see the header of `launch.py` for the rest) |
| `DON_PS2_BIOS_DIR`, `DON_PS2_BIOS_FILE`, `DON_ISO` | `tools/ref/run_pcsx2.sh`, `pcsx2_auto.sh` | the reference emulator's BIOS and disc | `local.env` |
| `DON_AUTO_RAW`, `DON_AUTO_OUT`, `DON_AUTO_AUDIO`, `DON_AUTO_FASTBOOT`, `DON_AUTO_RENDERER`, `DON_AUTO_SCREEN` | `tools/ref/pcsx2_auto.sh`, `drive.py` | output folder of the reference runs, audio backend, fast boot, renderer number, window size | `work/ref/pcsx2/raw`, `Null`, `true`, 14 (Vulkan), `1280x960` |

## The game thread's floating-point mode (journal 016, T-206)

**Measured** (journal 013, 016): the PS2's FPU and vector units truncate (round toward zero) and flush denormals to zero;
PCSX2 runs the guest thread in that mode. The port ran the game with the host default (round to nearest), and one
constant the game computes and uploads to the VU1 came out as `0x3F009A35` where the console has `0x3F009A34`
(one unit in the last place; 1,080 places). `src/host/fp_env.{h,cpp}` fixes it:

- the game thread gets the SSE control register `0xFFC0` once, when it starts (exceptions masked, round toward zero,
  flush-to-zero and denormals-are-zero on): `DON_FP_MODE=console`, the default;
- `DON_FP_MODE=host` leaves the thread as it is, so the two can be compared in one pair of runs;
- the calls that enter the GS library run with the host's value (`fp_env::HostCsrScope` in `gs_glue.cpp`), because
  the GS in the reference emulator runs on a thread with the default mode;
- on an architecture without that register nothing is done and one `[don]` warning is printed.

After it, the word `0x3F009A34` appears 1,086 times in the port against 1,087 on the console (journal 016, T-202). The
build rule that goes with it (`-ffp-contract=off`, no fast-math) is in AGENTS.md and is unchanged.

## Pacing and reproducibility (journals 014 and 017, patches 0033, 0034, 0035, 0038)

- **Speed of the host selected the guest's state** (journal 014, **Measured**): the same input script gave two different,
  each reproducible, states at vertical blank 60 depending on how much host time a blank carried (IOP cycles
  40,651,185 with `DON_TRACE_STATE` 1 or 2 against 40,645,063 with n ≥ 3, at nearly the same EE cycle). Journal 017
  corrects one figure of 014: the cycle difference was 6,122, not 6,111. 014's explanation of why a relinked build
  changed the floating-point mode (and so the state) was **confirmed** in 017.
- **Cause and patch 0038** (journal 017): in the idle path, `EeScheduler::waitForEvent` chose between the next event
  and the next EE timer interrupt by comparing **host deadlines**; with a late host the blank always won, so the
  clock jumped over the timer interrupts before it and delivered them late and merged (pending interrupts are a bit
  mask: seven became one). The comparison now uses **emulated cycles**; the host clock decides only how long the host
  thread sleeps. With 0038 the state at blank 60 is identical for `DON_TRACE_STATE` = 1, 2, 10 and 60.
- **Tools** for this kind of check: `DON_TRACE_STATE=<n>` (hash of guest RAM and the cycle counters every n blanks),
  `DON_STATE_DUMP`/`DON_STATE_DUMP_AT` (RAM at the blank where two runs first differ). Computing the hash costs
  one line at the first print ("hashing guest RAM costs ... ms per line").
- **Patch 0033** (T-108): the IOP got `eeCycles / 8` at every generated-code checkpoint (every 8 EE cycles), so
  each call paid fixed work for one IOP instruction at most; at the memory-card dialog a 60 s run gave 5 to 7 frames
  instead of 60 a second. Now the IOP is fed in batches of one emulator slice (256 IOP instructions = 2048 EE cycles);
  the totals are unchanged (the remainder is kept). Patch 0034 (T-207) removes the per-dispatch bookkeeping
  (the clock read, the event mutex, a snapshot every 4096 dispatches) with a precomputed "next cycle at which something
  is due"; patch 0035 (T-207) credits the EE timers per dispatch and advances them only when they can be observed.
  Neither changes what the guest sees; the figures are in the headers of the patches.
- **With sound** the run is paced at 60 blanks per second (journal 011). With the batch-size variable
  `DON_IOP_BATCH_EE_CYCLES` = 8, 64, 512 and 2048 the stop at the character select with sound moved on the build before
  patches 0037 and 0038; after journal 020 it no longer moves with the batch size (always blank 2280 with sound).

## Vector unit and sound: the engines and what is default today (journals 008, 011, 012)

- **VU1.** `DON_VU1` selects `microvu` (default, PCSX2's recompiler in `libdon_vu`), `pcsx2interp` (PCSX2's interpreter in
  the same library) or `runtime`/`interp` (the runtime's own interpreter, no library). Reason for the default
  (journal 008, **Measured**): the runtime's interpreter stopped on the menus' microprograms at `pc=0x168`
  (`0xfffffff8`), and the menu's picture similarity went from 0.08 to 0.52 with microVU. A missing library is not fatal:
  one `[don-vu]` warning and the runtime's interpreter runs. Journal 012: `HostVu` held a reference to `DonVuApi` that
  dangled after setup; it now owns a copy.
- **VU engine options** are set in `src/vubridge/don_vu.cpp` (`ApplyOptions`): the VU's floating-point control register
  as PCSX2 sets it (exceptions masked, denormals-are-zero, flush-to-zero, round toward zero), the vu1 instant mode on,
  the multithreaded VU off (compiled out).
- **Sound** is on unless `--sound off` (then nothing of the SPU2 library is loaded; default changed 2026-10-14). With it, the IOP's writes to the chip
  and its DMA reach `libdon_spu2` through `iopSetHostSpu2`; the output goes to SDL3 (`audio.cpp`, when built with
  `DON_AUDIO_SDL`) and/or a WAV. What the game does with the chip: [05](05-audio-video-input.md), "Sound".

## The memory card (journal 022, T-113 follow-up)

- **How the runtime keeps it** (**Measured** by reading `config/stub_policy.toml:234-259` and
  `Kernel/Stubs/MemoryCard.cpp`): 24 `libmc` functions are replaced (`stub`); a card is a **host folder**,
  `IoPaths::mcRoot`, default `<ELF folder>/mc0`, and port 1 is `mc1` next to it. The MCMAN and MCSERV modules are
  loaded on the IOP but no RPC to `0x80000400` is ever made (journal 022, 024).
- **`--mc-dir <folder>` / `DON_MC_DIR`** set `mcRoot = <folder>/mc0` after `loadELF` (which resets it). The launcher
  passes `--mc-dir`.
- **What saving needs** (journal 022): patch 0040. The console's MCMAN keeps a table of three entries of 0x30 bytes
  (at module data offset `0x262B0`) and the open returns the index of the first free one, **from 0**, and `-7`
  when none is free; the runtime handed out numbers from 1, so the game's first write answered `-4` and left a
  0-byte file. Now file numbers start at 0, the lowest free one is reused, and three open files is the limit
  (`kMcMaxOpenFiles` 32 → 3). End to end, a save and a load work with 0040 (file sizes 992, 964 and 60,840 bytes).
  The dialog logic is in the script opcodes of `FUN_001c7bc0` (0x1C7BC0), not in a library the port could stub.
- **Not done** (journal 022): free-space accounting, card states, `SetFileInfo`, what triggers the game's autosave, and
  the removal of the dead `kCv*` handlers.
- **Where the save lives** is not yet written in `docs/RUNNING.md` (not editable here; see the report).

## The patch series today (`patches/ps2recomp/series`, from 0033)

| Patch | What it fixes | Entry |
|---|---|---|
| 0033 `iop-cycles-in-batches` | the IOP is fed in batches of one emulator slice, not per 8-cycle checkpoint | T-108 (header) |
| 0034 `scheduler-fast-path` | per-dispatch bookkeeping of the EE scheduler | T-207 (header) |
| 0035 `ee-timer-batching` | EE timers advanced only when observable | T-207 (header) |
| 0036 `mpeg-stubs-do-not-write-the-games-code` | the `sceMpeg*` stubs wrote into `FUN_00171710` | journal 015 |
| 0037 `pexew-lanes` | `PS2_PEXEW` moved the wrong lanes (116 sites in 32 files) | journal 013 |
| 0038 `pacing-does-not-change-guest-state` | host clock no longer chooses between events and timers | journals 014, 017 |
| 0039 `sign-branches-test-64-bits` | `bltz`, `bgez`, `blez`, `bgtz` tested 32 bits (7,538 sites) | journal 019 |
| 0040 `mc-file-numbers-from-zero` | memory-card file numbers and the three-file limit | journal 022 |
| 0042 `path3-released-by-packet` | path 3 delivered the whole queue at once on lifting the mask | journal 021 |

`series` has no 0041. Patches 0043 and 0044 (journal 025, instruction conformance) were in the working tree but not in
`series` when this was written; they are named here by number only.

## What is not covered

- Nothing was run: the claim that `Pad.cpp`'s raylib fallback is dead rests on reading
  `ps2_pad.cpp:31-32,122` and `Pad.cpp:300-315`, not on a trace. A one-line `DON_` diagnostic
  in `readPadPortData` (`lastUsedBackend`, already stored in `PadPortState`) would confirm it
  (`Pad.cpp:51,327`).
- The multitap behaviour of the game (does it call `sceMtap*`, what does `mtapman` do under
  the IOP emulator) is task T-306.
- Which sound module the game's audio really goes through, and so whether the HLE `libsd`
  path is used at all, belongs to T-101.

### Audio pacing (T-303, 2026-10-14)

With an audio device the game thread, at every vertical blank (`HostGs::set_vblank_hook`), waits 1 ms at a time while more
than `DON_AUDIO_TARGET_MS` (60) of sound is queued (the ring plus SDL's stream). The wait only holds the game back: it
changes nothing the game computes (the state line of `DON_TRACE_STATE` is the same with it on and off). Why: the
scheduler's 60 Hz clock is absolute, so a game that fell behind (a load, a busy machine, a suspended process) runs faster
than 60 blanks per second until it has caught up, and the sound it makes then arrives faster than the device plays it: it
queued up to 1.25 s (the ring holds 1 s, then drops its oldest frames; SDL's stream holds 0.25 s) and stayed that late for as
long as the catch-up lasted, with holes where frames were dropped. With the wait the queue stays under about 80 ms. A device
that does not drain (the wait times out 8 blanks in a row) turns the wait off with one `[don-spu2]` line. Without a device
(headless) nothing waits.
