# 05 · I/O processor, sound, video, gamepad and memory card

## The I/O processor and its modules

The PS2 has a second processor (**IOP**) that runs `.IRX` modules: the drivers for
disc, gamepad, memory card and sound. The game (on the main CPU) talks to them through remote
calls (**SIF RPC**).

**Measured**: the game does not load the loose `.IRX` files from the disc, but two compressed
FPK packs that it decompresses in memory and passes to the IOP as a buffer.

| Pack | Modules it contains (uncompressed size) |
|---|---|
| `MODULES/0000.FPK` | `libsd` (28,661), `sio2man` (6,641), `mtapman` (10,853), `mcman` (96,181), `mcserv` (7,385), `padman` (45,797), `cdvdstm` (33,521), `rtfssiop` (31,872) |
| `MODULES/1000.FPK` | `sdrdrv` (9,161), `modhsyn` (63,037), `modmidi` (21,941), `modmsin` (1,929), `tsnddrv` (185,036), `cri_adxi` (69,237), `tsnddrvc` (171,580), `sndfi` (101,040), `libsd` (28,661) |

What each one is for:

| Module | Function |
|---|---|
| `sio2man`, `padman`, `mtapman` | Gamepad serial port, gamepads and multitap |
| `mcman`, `mcserv` | Memory card |
| `cdvdstm` | Streaming disc reads |
| `libsd` | Low-level access to the sound chip (SPU2) |
| `sdrdrv` | Server that exposes `libsd` to the main CPU |
| `modhsyn`, `modmidi`, `modmsin` | Sony synthesizer and sequencer |
| `cri_adxi` | IOP part of CRI's ADX player (music and voices) |
| `sndfi` | Dated March 2005; by its name, the game's sound interface. **Unverified** |
| `tsnddrv`, `tsnddrvc` | Third-party sound driver. **Unverified** whether it is ever loaded |
| `rtfssiop` | **Unverified** |

Module names that appear as a string in the executable: `sio2man`, `mtapman`, `mcman`,
`mcserv`, `padman`, `cdvdstm`, `libsd`, `modhsyn`, `modmidi`, `modmsin`, `sdrdrv`, `cri_adxi`,
`sndfi`. `tsnddrv`, `tsnddrvc` and `rtfssiop` **do not appear**, which suggests they are not
loaded (or that their name is built at run time). Confirming this is task T-102.

Before loading them, the game resets the IOP with the image `cdrom0:\MODULES\IOPRP300.IMG;1`
(string `rom0:UDNL `).

### What PS2Recomp says about this

**Declared** in `third_party/PS2Recomp/ps2xIOP/README.md`:

- The IOP emulator runs the original `.IRX` files in an R3000A interpreter with a virtual
  kernel; no BIOS is needed.
- There are generic high-level services only for `MCSERV`, `LIBSD` and `DBCMAN`, and only if
  there is no real module serving that service.
- Verbatim quote: *"Games previously using TSNDDRV, CRI DTX, CLFILE, SOUND or SDRDRV profiles now
  require their IRX modules and support for the imports and hardware they use."* That is, DON
  is in the list of games that depend on their real modules working.
- *"Unknown modules fail to load; unknown RPC SIDs remain unhandled."*
- Loading API: `loadModule(...)` and `loadModuleBuffer(...)`.

The list of IOP kernel functions that each DON module imports, and which ones the emulator
supports, is **not measured** (task T-101). It is the data that most narrows the audio risk.

## Sound

| What | Data | Planned path |
|---|---|---|
| Music and voices | `BGM.AFS`, 178 ADX tracks | ADX library on the main CPU (already recompiled) + `cri_adxi` on the IOP + SPU2 |
| Effects | `SE.AFS`, 38 `ps2_DTPK` banks; names `snd/se/ps2/*.dat` | `sndfi` and/or Sony's synthesizer on the IOP + SPU2 |

The executable has `sceSdRemoteInit` (`0x119CC0`), `sceSdRemote` and `sceSdTransToIOP`, the
`sdrdrv` interface.

The sound chip (**SPU2**) has to be emulated. PS2Recomp ships `ps2_audio.cpp` (328 lines) and
a generic `LIBSD` service. The projects with stable audio use PCSX2's SPU2 core (see
[08-external-references.md](08-external-references.md)).

### What was measured with the SPU2 library (journal 011 and its follow-ups, 2026-10-13)

The port runs the game's own sound modules on the IOP emulator and gives them PCSX2's SPU2 core
(`libdon_spu2`, `--sound on`; [13](13-runtime-host.md)). Everything below is **Measured** with
`DON_TRACE_SPU2=1` and recordings (`--wav`) unless it says otherwise.

- **The chip is initialised once.** All 1,142 register writes happen in the first emulated second (the
  `libsd`/`sdrdrv` start-up: 240 voice-register writes per core = 24 voices x 10 registers, 12
  key-on/off, 175 and 145 core-control writes, 100 reverb, 118 post-block); there are **none afterwards**,
  not even when the music starts (journal 011, T-304). Register reads continue at about 94 per second:
  the IOP polls the chip. Core 1's master volume is 0.
- **The stream.** Core 0 gets a DMA stream of about 94 transfers per second of about 2,048 bytes =
  **192 KiB/s**, exactly 48,000 x 2 channels x 2 bytes (48 kHz stereo PCM), each with its DMA-end
  event. Core 1 never gets a transfer. The DMA is started by the game's own `libsd`/`sdrdrv` on IOP
  channel 4 (the HLE `libsd` service is off when a host SPU2 is installed). In a run of 81 s the
  stream carried 15,511,552 bytes and 16 voices were keyed on (core 0 voices 0-7, core 1 voices 8-15).
- **Events used.** The driver uses the DMA-end event (`events4`: 7,573 in that 81 s run) and **never
  the chip's interrupt** (`events1` is 0 for the whole session). The music plays without it.
- **The IOP audio thread sleeps.** Before the library, the CRI ADX driver's thread (`cri_adxi`, thread
  10) kept the IOP busy at one instruction per cycle (about 9.4 M instructions/s, a 64-slot service loop
  of records of 0x24 bytes from `0x7E9E0`; `work/orchestrator/t111/README.md`, T-111). With the
  host SPU2 installed the IOP **sleeps**: about 2 % of the cycles are instructions (0.73 M per 60
  blanks), and the port runs at about 46 blanks a second at the memory-card dialog instead of about 16.
  Details of the thread: [14](14-iop-modules.md).
- **Level, before and after patch 0039.** Before 0039 the game's own DMA data peaked at 0.021 of full
  scale (recording peak 0.0207, RMS about 0.002-0.005): the quiet level was the game's data, not the
  mixer. After 0039 (journal 019, videos skipped): sound from second 35 (the title, blank about 2100),
  peak **0.53** at second 36, then 0.27-0.35; `dmapeak0` 17,883-18,296 of 32,768 at seconds 35-38. So
  the music is **about 15 to 25 times louder** with the same chip code: the quiet music was a recompiler
  defect ([04](04-graphics.md), "The path of a 3D object").
- **Pacing.** With sound the run is paced by the sound clock at 60 blanks per second.
- **Still open:** in a route with sound the game **stops at the character select** (the picture, the
  blanks, the IOP and the music go on, but no button changes anything; blank 2280-2880 depending on the
  route). The IOP's thread tables, the EE's thread tables and the chip's registers are **identical**
  before and after the stop (journal 011, T-304), the stop does not move with the IOP batch size after
  journal 020, and without sound the same route does not reach the character select (the title
  stops answering earlier, journals 023 and 024). Its cause is **not determined**; whether it is a sound
  defect is not shown either way.

## Video

Two Sofdec videos in `MOVIE.AFS`. The executable has CRI's libraries (`CRI SFD`,
`CRI MPV`, `mwPlySfdInit` messages) and also Sony's `libmpeg` and `libipu` (`sceMpegCreate`
`0x108FA8`, `sceMpegGetPicture`, `sceIpuInit` `0x10BC38`, `sceIpuSync`).

**Answered 2026-10-13 ([19](19-videos.md) sections 1 and 2): the IPU question.** The player is CRI
Sofdec; its decoding goes through Sony's `sceMpeg`, with CRI callbacks that poke the IPU registers
and DMA channels 3 and 4 themselves, and the port has no IPU model, so no picture is decoded. The
undecoded video never ends by itself; Start ends it, and since journal 023 the port skips both
videos by default ([16](16-boot-sequence.md) section 7). The original wording follows.

*(Original, 2026-10-11)* **Unverified** which part of the decoding is done by the PS2's video hardware (IPU) and which
part by CRI's code. Options, in order of cost:

1. Skip the videos (make the game believe they have finished). Enough until milestone M3.
2. Decode externally with FFmpeg, hooking at the `libmpeg` level (the PS2Recomp runtime does
   this when built with FFmpeg) or at the CRI API level.
3. Emulate the IPU with PCSX2's as a library (what the Killzone port does).

## Gamepad

- In the executable: `scePadInit` (`0x118590`), `scePadPortOpen`, `scePadRead` (`0x118E38`),
  `scePadGetState`, `scePadSetActDirect` (vibration), `scePadInfoPressMode`,
  `scePadEnterPressMode` (pressure-sensitive buttons), and `libmtap` strings.
- **Vibration (measured, T-308, journal 035).** The game calls only `scePadSetActDirect` (`0x1194C8`), every frame, with
  6 bytes `{small, large, 0, 0, 0, 0}`: small 0/1, large 0..255. It never calls `scePadSetActAlign` (`0x119590`; a
  call site exists, `0x139760`, but it did not run in a practice fight), so patch 0066 starts every opened pad with
  the table `{0, 1, FF...}` (inference: byte 0 small motor, byte 1 large). In a practice fight (Naruto vs Sasuke, port 0
  only), setting ON: small motor on for 3 to 11 blanks per hit, small 1 + large 255 for about 21 blanks twice (the ultra
  move or its hit); setting OFF: only zeros in 7229 calls. The runtime tells the host with `Ps2HostPad::actuator`;
  `src/host` turns it into `SDL_RumbleGamepad` (small on = high frequency full, large = low frequency x257), renewed
  every 400 ms (1000 ms duration). `--rumble off` disables it. Not verified: a real pad's feel.
- The PS2Recomp analyzer links the `scePad*` functions to the runtime's own handlers, that is,
  the gamepad is resolved on the main CPU without going through `padman`.
- **4 players on PS2 require a multitap.** The runtime's multitap support (reads with
  `slot` other than 0, `sceMtap*` functions) is **unverified** (task T-306).
- Lesson from another project (**declared**, Killzone): the pressure bytes arrive in the order
  L1, R1, L2, R2. **Measured** on 2026-10-11 by reading: PCSX2's DualShock 2 gives the same order
  (`pcsx2/SIO/Pad/PadDualshock2.cpp:286-294`), while the PS2Recomp runtime writes L1, L2, R1, R2
  in bytes 16 to 19 of the pad buffer (`Kernel/Stubs/Pad.cpp:263-266`), and this repository's
  patch 0008 kept that. Killzone fixed it in its patch 0026; here it is patch
  `0022-pad-pressure-order` (in progress). Whether this game reads buttons through the pressure
  bytes is **Unverified**.
- The input layer of the port (`src/host/input.*`, `input_sdl.*`, `input_script.*`; tests in
  `tests/input/`): a standard gamepad is mapped by position (bottom button Cross, right button
  Circle), the keyboard has a fixed mapping for player 1, and a script of button presses by
  frame number can drive the game ([../INPUT_SCRIPTS.md](../INPUT_SCRIPTS.md)). ~~Not yet wired
  into the host program, and not tried with a real gamepad.~~ **Corrected 2026-10-13:** the layer
  is wired into the host program (`host.cpp`; T-112, `work/orchestrator/t111/README.md` part 2):
  the game thread steps the script at the end of every blank and merges it with the devices'
  state. Whether it was tried with a real gamepad is not in the entries read (**Unverified**).
- **Keys and pad by position** (source: the comment in `src/host/input.h`; **Declared** there,
  tested in `tests/input/`). A standard gamepad is mapped by position: bottom button (Xbox A) =
  Cross, right button (B) = Circle, left (X) = Square, top (Y) = Triangle; Back = Select, Start =
  Start, stick clicks = L3/R3, shoulders = L1/R1, triggers = L2/R2, D-pad = directions. The game
  confirms with Circle and cancels with Cross, so on a gamepad right = confirm, bottom = cancel.
  The keyboard is player 1 only and fixed:

  | Keys | Button | Keys | Button |
  |---|---|---|---|
  | Arrow keys | D-pad | Q / E | L1 / R1 |
  | W A S D | left stick (full) | 1 / 3 | L2 / R2 |
  | I | Triangle | Enter | Start |
  | L | Circle (OK) | Backspace | Select |
  | K | Cross (cancel) | C / V | L3 / R3 |
  | J | Square | | |

  Opposite directions pressed together cancel; there is no right stick on the keyboard.
- **The multitap wrappers are replaced** (`src/game/don_overrides.cpp`, `applyDon`; the wrappers
  are the game's own SIF RPC functions to `mtapman`, [14](14-iop-modules.md) section E):
  `0x255CA0` (init), `0x255DC0` (port open), `0x255E30` (port close) and `0x255EA0` (connection
  query) now return 1 (the query also writes 0 to both output words). Reason, from the comment:
  `mtapman` probes the SIO2 hardware at `0x1F808200`, which the port does not model, so the real
  query answered 0 and the per-frame pad poll `0x138FB0` then skipped the whole update. With the
  replacements the poll stays in the two-port mode and reads port 0 and port 1, slot 0, through
  `scePadRead`. The four-pad mode is asked for only with `--players 4` (T-306, measured: journal 026 follow-up 2026-10-14).
- **Measured** in the reference emulator (T-017, 2026-10-11): in the menus Circle confirms and
  Cross cancels, as usual in Japanese releases; Start leaves the title screen. The route through
  the menus to a 1v1 practice match and to a four-fighter match is `tools/ref/route.txt`.

## Memory card

- In the executable: `sceMcInit` (`0x11BD58`), `sceMcOpen`, `sceMcRead`, `sceMcSeek`,
  `sceMcMkdir`, `sceMcGetSlotMax`, `sceMcChangeThreadPriority`...
- Related data: packs `fpack/mcard/0000.fpk`, icon `mcard/battle_logo.ico`.
- The PS2Recomp runtime stores the card as a folder (`mc0/` next to the executable by default).
- Lesson from other projects (**declared**, Ultraman FE3): the status query has to
  return "running" once before "finished" or the game keeps waiting.
- **How the game saves, measured 2026-10-13 (journal 022):** the `libmc` functions are replaced
  by the runtime (24 `stub` entries in `config/stub_policy.toml`); the MCMAN and MCSERV modules are
  loaded but no RPC to them is ever made. The first open must answer file number 0 and the table has
  three entries (patch 0040); saves of 992, 964 and 60,840 bytes work end to end. Details, the
  `--mc-dir` option and what is not done: [13](13-runtime-host.md), "The memory card".

## Disc

- The game reads by name the four AFS files that `DIR.DIR` lists and, inside them, by sector.
- The runtime supports two modes: extracted folder (`cdRoot`, by default the ELF's folder) and
  image (`cdImage`). **Use the ISO image**: with an extracted folder the sector numbers do not
  match those of the disc (problem **declared** by Killzone, patch 0003).
- The `sceCd*` functions (`sceCdInit` `0x11AFD0`, `sceCdRead` `0x11B6F0`, `sceCdSync`...) are
  recognized and linked to runtime handlers. CRI's reader (`ADXF`, `CVFS`) sits on top of them
  and of `cri_adxi`; its behavior in the runtime is **unverified**.
