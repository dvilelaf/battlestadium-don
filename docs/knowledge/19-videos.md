# 19 · Videos: how the game plays its movies, how the port plays them now, and what is open

Tasks T-305 (videos) and T-113 (the skip). Sources: reading `disc/SLPS_256.75` (load base `0x100000`),
the PS2Recomp runtime, PCSX2 v2.8.2 in `third_party/pcsx2/`, the disc's `MOVIE.AFS`, reference-emulator
runs of 2026-10-13 (section 4), and the port's measurements in
[docs/journal/007](../journal/007-first-screen-and-the-black-video.md) and
[docs/journal/034](../journal/034-videos-through-ffmpeg.md) (follow-ups 1-22, to 2026-10-15).

Markers: **Measured** (I ran something and saw it, or read the exact bytes or source line),
**Inferred** (a conclusion from measured facts, not seen directly), **Declared** (stated by a project
document or a third project), **Unverified** (not settled; the entry says what would settle it).
A statement this document made earlier that the journals proved false is listed in section 7.

Addresses are game (EE) addresses. Runtime references are `file:line` under
`third_party/PS2Recomp/ps2xRuntime/src/lib/`.

## 0. Status (2026-10-15)

- **Opening and ending movies are decoded by the game's own player** when the build has FFmpeg
  (`-DDON_ENABLE_FFMPEG=ON`, on in the `linux-release` and `linux-dev` presets). Pictures are in the
  right colours and the sound plays. The movie ends by itself, or the player's Start skips it; the title
  follows. **Measured**, journal 034 follow-ups 7 and 14 (one run each).
- **Without FFmpeg** (the default build) the player gets black pictures and the port presses Start for it
  after 240 blanks. **Measured** for the two routes of `tests/regression` (follow-up 16).
- **Ending (Circle):** never run. **Unverified.**
- **Full regression suite on the final series:** not run (follow-up 22 ran two slow routes; the rest is at
  integration). **Unverified.**

## 1. The player (V3, V6)

The chain is: script opcode `0x50` → `0x36AC70(index)` → `0x36A820(name)`.

| Fact | Address | Marker |
|---|---|---|
| Opcode `0x50` parses two operands and calls `FUN_0036ac70(*puStack_4, *puStack_8)` | `0x1BC45C`-`0x1BC468` in `0x1BC420` | Measured |
| `0x36AC70`: `if (index < 2) 0x36A820(table[index]); return 1;` | `0x36AC70` | Measured |
| Table of names, 4 entries: OP, ED, ED, OP | `0x3F23C0` | Measured |
| Index 0 `snd/movie/ps2/OP.sfd`, index 1 `snd/movie/ps2/ED.sfd` | strings `0x4C7410`, `0x4C7430` | Measured |
| The second operand becomes `0x36A820`'s `param_2` (1 = opening, 2 = ending) | `0x36AC98`-`0x36AC9C` | Measured |

`0x36A820` in order (**Measured**):

1. GS reset and a 640x224 video mode (`sceGsResetGraph`, `sceDmaReset`, `FUN_00100ee0`, `FUN_0036a640`),
   the VBLANK callback `0x36A130`, a parameter block at `0x55A910`.
2. CRI set-up (`FUN_00344a58`), a pool (`FUN_001386d0`), the player object (`FUN_003459b0`, doc
   [16](16-boot-sequence.md) §7). A null result has no error branch the compiler kept (what the game does
   then: **Unverified**).
3. Probes the two names (ED, then OP, table `0x3F23C8`) and calls `FUN_00348750` or `FUN_00348c50`.
4. The frame loop (below).

### The loop

Each iteration: CRI/ADX service calls, a snapshot of the VBLANK counter, the SNDF/RPC tick, one decode
step `FUN_00346908(player, 0x55A870)` (status byte at `0x55A870`), then the status check.

| Fact | Marker |
|---|---|
| The loop ends when the status byte is **3**; 4 keeps it going, 2 pushes the other buffer, 0/1 continue | Measured |
| With a null player or a failed decode step, `FUN_00346908` prints an error, zeroes its status and returns 0. The loop then never ends by itself: the only exits are status 3 and the skip buttons | Measured (reading) |
| No blocking wait in the loop (no `sceGsSyncV`, no time comparison). Pacing comes from the VBLANK counter (`0x4D6A08`) | Measured |
| The only clock-like input is at start (`FUN_0034a140(player, FUN_00150ab0())`); what it does is **Unverified** | Measured / Unverified |
| The video is not paced by an audio clock: a sound system that never advances does not stall the loop | Measured (no wait in the loop) |

### The skip buttons

`0x4D6A40` is the "buttons pressed this frame" word, filled by `FUN_00138fb0` for every open port:

- It reads the pad buffer at `0x4D6FC0` (bytes 2 and 3) and computes `G = 0xFFFF ^ ((data[2] << 8) | data[3])`
  (**Measured**).
- A real pad sends bits 0-7 of the SDK button word in `data[2]` and bits 8-15 in `data[3]` (**Measured** in
  PCSX2 `PadDualshock2.cpp:213`, `:259`; the runtime writes the same order, `Kernel/Stubs/Pad.cpp:297-298`).
- So `G` bit 11 (mask `0x800`) = **START**, and bit 5 (mask `0x20`) = **CIRCLE**.

The opening (`param_2 == 1`) is skipped with **Start**: **Measured** on the console (section 4) and in the
port (follow-ups 7, 13). The ending (`param_2 == 2`) with **Circle**: **Unverified**.

## 2. How the decoding works (CRI Sofdec)

The game does not call Sony's libmpeg for the movie directly: it uses CRI's Sofdec, which sits on Sony's
`sceMpeg` API, with CRI callbacks that poke the IPU and DMA registers.

| Fact | Address | Marker |
|---|---|---|
| CRI decoder open: `sceMpegCreate` + 4 x `sceMpegAddCallback` (callbacks `0x35D238`, `0x35D340`, `0x35D3E8`, `0x35D410`), or CRI's own path if `FUN_0035f810() == 1` | `0x35D010` | Measured |
| `FUN_0035f810` returns `DAT_003f1ab4` (initial 0; its only writer `FUN_0035f800` has no callers found) | `0x35F810`, `0x3F1AB4` | Measured; the writer's run-time use is **Unverified** |
| `sceMpegGetPicture` is called from the frame step `FUN_0035d768` | `0x35D84C` | Measured |
| `sceMpegReset` from `FUN_0035d438`; `sceMpegInit` from `FUN_0035cf60` | `0x35D490`, `0x35CF74` | Measured |
| `sceIpuInit` is called from libmpeg's own `sceMpegInit` | `0x109BE8` | Measured |

The register-level functions (a scan of every `lui 0x1000` base; `work/video/scan_io.py`):

| Function | What it does | Registers |
|---|---|---|
| `FUN_0035c410` / `FUN_0035c478` | start/stop IPU_FROM / IPU_TO through DMA3 / DMA4 | `0x1000B000` / `0x1000B400` |
| `FUN_0035c4e0` | build a 64-bit DMA tag in memory | none |
| `FUN_0035c580` (`entry_0035c640`) | build the DMA4 transfer list and start it | `0x1000B400`-`0x1000B430` |
| `FUN_0035ca80` | WaitSema; start DMA4; save DMA4/DMA3/IPU state; **poll `IPU_CTRL` OFC until 0**; SignalSema | `0x10002010`, `0x1000B0xx`, `0x1000B4xx` |
| `FUN_0035cb80` (`entry_0035cd88`) | the decode kick: build descriptors, write `IPU_CMD`, poll `IPU_CTRL` and BUSY, start DMA4 with CHCR `0x100`, write `IPU_CTRL` last | `0x10002000`, `0x1000B0xx`, `0x1000B4xx` |

- `IPU_CTRL`: bits 0-3 IFC, bits 4-7 OFC, bit 30 RST, bit 31 BUSY (`third_party/pcsx2/pcsx2/IPU/IPU.h`).
- The IPU FIFOs (`0x10007000`, `0x10007010`) are touched only by Sony's libipu. The only code in the
  executable that touches IPU or DMA3/4 registers is Sony's libmpeg/libipu (`0x105000`-`0x10C000`)
  (**Measured**, `work/video/io_scan.txt`).
- The CRI entry points of this cluster have no direct callers; they are registered with
  `sceMpegAddCallback` or reached indirectly. A `jal`/`j` closure from the player reaches 793 functions and none of
  them (**Measured**), which is why doc [16](16-boot-sequence.md) §7 once said "no `sceMpeg*`": that was wrong
  as a description (CRI calls `sceMpegInit/Create/AddCallback/GetPicture/Reset`).
- `sceMpegAddBs` has no callers: the game feeds the stream through CRI's own file layer and DMA4
  (section 4). **Measured.**

### What the runtime does with the IPU (unchanged)

| What | Where | Behaviour | Marker |
|---|---|---|---|
| IPU registers `0x10002000`-`0x10002030` | `ps2_memory.cpp:1194-1211`, `:2332-2352` | stored words; `IPU_CTRL` reads with bit 31 = 0 | Measured |
| IPU FIFOs `0x10007000`/`0x10007010` | `ps2_memory.cpp:1231`, `:2401-2407` | plain stored words | Measured |
| DMA channel 3 (`0x1000B000`) | `ps2_memory.cpp:1305-1326` | no transfer; the CHCR write completes with no cause | Measured |
| DMA channel 4 (`0x1000B400`) | patch 0064 | **the game's stream is taken from its chain** (section 4); no IPU is modelled | Measured |
| `Kernel/Stubs/IPU.cpp` | `sceIpuInit` | "pretend the Sony init happened", no decoding | Measured |
| `Kernel/Stubs/MPEG.cpp` | `sceMpegCreate`, `sceMpegGetPicture`, ... | replaces the `sceMpeg` family by name (`config/stub_policy.toml:69-85`); decodes with **FFmpeg** (`MpegFfmpegDecoder`) when built with it; without it, black pictures (patch 0070) | Measured |

Because no IPU is modelled, none of CRI's IPU poll loops can hang the port: the registers are stored
and the DMA3 start does nothing (**Inferred** from the code, consistent with the runs).

## 3. The build options and the presets

| Item | Where | State |
|---|---|---|
| Option `DON_ENABLE_FFMPEG` (default OFF) → `PS2X_ENABLE_FFMPEG` and `DON_WITH_FFMPEG` in `don` and `don_host` | `CMakeLists.txt:93-156` | Measured |
| Prefix `DON_FFMPEG_PREFIX` (default `third_party/ffmpeg/linux`), put on `PKG_CONFIG_PATH` | `CMakeLists.txt:94-97` | Measured |
| Presets `linux-release` and `linux-dev` set `DON_ENABLE_FFMPEG=ON` | `CMakePresets.json` | Measured |
| Linux build of FFmpeg 7.1.1, static, LGPL (no `--enable-gpl`, no `--enable-nonfree`), 6.2 MB, decoders `mpeg2video`/`mpeg1video`, demuxer `mpegps` | `tools/setup/build_ffmpeg.sh` (inside the build container) | Measured |
| Windows: static FFmpeg from a prefix (`PS2X_FFMPEG_PREFIX`, patch 0069); the runtime's download of a shared prebuilt is kept when the prefix is empty | `tools/setup/build_ffmpeg_windows.sh`, patch 0069 | Cross-build only (see section 6) |

## 4. The data path the port uses now (CRI through the runtime)

**The server thread.** The movie is decoded by CRI's server thread (entry `0x314850`, the thread that runs
the callbacks of `sceMpegAddCallback`'s class 6). Its loop calls `0x331800` → `0x331610`, which runs six
(function, argument) pairs; the first is the player's server, `0x34DFE0(0x4C4D90)`. That server runs the
player's sub-players (`0x34E248` → `0x34E310` → `0x34E3C8` → `0x35E038`) and the exec step `0x35E0E8`,
which fills the output ring that the player loop `0x346908` takes pictures from. The frame step `0x35D768`
calls `sceMpegGetPicture`. **Measured** (follow-ups 1-5, gdb and reading).

**Wake-ups.** The vertical-blank threads wake the server thread through the game's pattern
`if (ResumeThread(t) == t) WakeupThread(t)` (`0x315940`, called from `0x315A70` every loop) and
`if (WakeupThread(t) == t)` (`0x3159C8`). The kernel answers the thread id; the runtime answered 0, so the
server never woke (fixed by patch 0063). Its status is read with `ReferThreadStatus` (`0x314AB0`: status 4 or
0xC means wake it), and a wait the runtime makes up for pacing was reported as a kernel wait (fixed by 0068).

**The files** (`MOVIE.AFS`, **Measured** from the bytes; the AFS name table is empty, so members are
identified by content): member 0 is the ending (`ED_PS2~1.SFD`, 41,127,936 B), member 1 the opening
(`OP_PS2~1.SFD`, 69,896,192 B). Both are MPEG-2 program streams, 640x448, 4:3, 29.97 fps, about 6.3 Mbit/s;
video stream `0xE0`, a second stream `0xC0`. Durations from the bit rate: OP about 89 s, ED about 52 s
(**Inferred**; the OP figure fits the ~85 s measured on the console). Which AFS member `OP.sfd` maps to at run
time is **Unverified**; the port shows the opening's content.

**The stream.** The file is read with `sceCdRead` (from `0x3244E8`) into CRI's own ring of 524,288 bytes
(**Measured**; the reads are the file layer's, not `sceCdSt*`, so the runtime's CD-stream hooks never see
them). CRI sends the ring to the decoder through **DMA channel 4** (IPU_TO), in source-chain mode, one `ref`
tag per 2048-byte sector: the first start is `MADR=0x01121040`, `TADR=0x011A1040`, `CHCR=0x30000105`. The
ring holds a raw MPEG-2 video elementary stream (it starts with a `00 00 01 B3` sequence header, 640x448).
CRI rewrites the tags after each stop so that the chain ends at the last valid sector (**Measured**,
journal 034 follow-up 6). The game only ever sends `IPU_CMD = 0` (BCLR).

**The decoder.** `sceMpegGetPicture` (the runtime stub) pulls 2048 bytes at a time from the channel-4 chain
until a picture is ready (patch 0064). Each picture goes into the 640x448 field buffers; the display buffer
setup is patch 0065.

**The end.** When the stub has no picture to make, channel 4 has delivered its bytes and is idle, the stub
calls the Nodata callback the game registered (`sceMpegAddCallback` type 1, `0x35D340`). That callback reads
channel 4 and, when the chain has been started and the channel is idle, sends one quadword (the sequence
end) on channel 4 (**Inferred** from reading `0x35D340`; **Measured** that the movie then ends). The
player's state machine then reaches status 3 and the loop exits. A skip that only drops the decoder's bytes
does not end the movie: CRI's own ring keeps refilling (follow-up 11).

**Console timeline** (reference emulator, 2026-10-13; **Measured**, 5-second grabs):

- About 110 s from the memory-card answer to the title: the BANDAI logo, then the opening movie (about 85 s).
- One Start per second from the answer reaches the main menu in about 25 s.
- A single Start 2 s after the answer does nothing (the timeline matches the no-press run to the frame).
- With a savestate inside the movie (follow-up 12): one Start press clears the player (`0x55A860` to 0)
  within 45 vertical-sync counts.

## 5. The port's state per build

| Build | Pictures | Sound | Skip | Measured in |
|---|---|---|---|---|
| FFmpeg (`DON_ENABLE_FFMPEG=ON`) | the movie in the right colours, from blank ~1830 (follow-up 7) | from ~34 s, RMS 1.7k-4.8k, one run; the source (the movie's own audio or the title music) is not separated | the player's Start (or Circle, unverified) ends it; the title follows | follow-ups 7, 13, 14 |
| FFmpeg, `DON_MOVIE_SKIP_AFTER=N` | as above | as above | the port presses Start after N blanks | follow-up 11 (draft), 14 (in `src/`) |
| No FFmpeg (default) | black pictures (patch 0070) | none | the port presses Start after 240 blanks (the default when FFmpeg is off) | follow-up 16 (`default_title_idle`, `fight_practice` pass) |
| `DON_PLAY_MOVIES=1`, either build | the player runs; the port presses nothing | as the build | the player's own Start | `src/game/don_overrides.cpp:478`; the no-FFmpeg case of this row is **Inferred** from the code, not run after 0070 |

The movie's end time differs between runs: ~6930 blank in follow-up 7 (before 0068), before blank 4000 in
follow-up 14 (after 0068, the title is in the shot), and blank 2220 in the `save_and_load` load step
(follow-up 15, a different route). **Unverified** why; not compared with the console's end in the same run.

Opening and skip are the same code path in both builds: `src/host/video_skip.h` (the default by build) and
`src/host/host.cpp:262-271` (`DON_MOVIE_SKIP_AFTER`). `tests/regression/harness.py:68` sets `240` unless the
caller sets it.

## 6. Patches that make it work (what each fixed and why it was needed)

Each patch is in `patches/ps2recomp/series` (0063-0072). A header in each patch names its test.

- **0063 thread resume/wakeup return the id.** The game compares `ResumeThread(t)` and `WakeupThread(t)`
  with `t`; the runtime answered 0, so the server thread never woke (follow-up 3: class-6 loop ran 4 times,
  then stopped). The non-nesting part of `SuspendThread` changes nothing in this game (follow-up 4, counted at the
  syscalls: balanced); it is kept, but not needed.
- **0064 channel 4 feeds the stub.** The decoder had no bitstream: CRI sends it over DMA4, and the runtime's
  stub waited for pictures that could never come (follow-up 5). The stub now pulls from the chain and runs the
  Nodata callback at the end (follow-up 6, 7).
- **0065 GS second-buffer page in field mode.** The scramble on screen was the display, not the decoder: the
  second 640x224 buffer was at page 35 instead of 70 (`FRAME_1 = 0xA0046` on the console), so the buffers overlapped.
  With 70, 0 bad blocks of 2240 sampled (follow-up 7).
- **0068 `ReferThreadStatus` for a pacing wait.** The stub's own vertical-sync wait inside the server thread read
  as a kernel wait, so the game woke a running thread 157 times and the player never finished on a Start press
  (follow-up 13). Now a VSync, Mpeg or External wait reads as READY. At Start the port's thread table matches the
  console's, and the player is gone by blank 2740 (console: gone within 45 counts).
- **0070 black pictures without FFmpeg.** After 0064, a build without FFmpeg never produced a picture, so the
  auto-skip stalled (follow-up 16). It now outputs black pictures of the stream's size, one per picture start code.
  The FFmpeg build is unchanged.
- **0071 SPU2 DMA end time (fake path).** The sound-off path ended a 2048-byte SPU2 transfer after 1,024 IOP
  cycles; PCSX2 takes 24,576. The transfer thread then walked its stream objects 32,000 times per IOP second,
  and an RPC destroyed one mid-walk: `E2004090216 : Specified handle is invalid`, and the sound stopped
  (follow-up 20: `save_and_load` fails before, passes after). The host SPU2 path already uses PCSX2's timing.
- **0072 RPC handler after higher-priority IOP threads.** The port ran the ADX RPC handler at once, while the
  transfer thread (priority 39, Ready, mid-walk) had not finished its round; the console runs it in the server
  thread (priority 48), which only runs when the higher threads block (follow-up 22: `fight_practice` and
  `survival_result` fail before, pass after). **Inferred:** that the console's order is the reason; the console's
  priorities were read from the port's table, not from the console.
- **0069 Windows static FFmpeg prefix.** Build only: cross-built with clang-cl (patch header). **Not run on
  Windows.**

Tests (each fails without its patch): `tests/runtime/mpeg_ipu_dma_test.cpp` (FFmpeg on),
`mpeg_black_decoder_test.cpp` (FFmpeg off), `thread_wake_return_test.cpp`,
`thread_status_pacing_wait_test.cpp`, `iop_spu2_dma_time_test.cpp`, `iop_rpc_server_priority_test.cpp`,
`gs_dbuff_frame_mode_test.cpp`, and `tests/host/video_skip_test.cpp`. The runtime suite was run by the
task that wrote each patch (27 and 32 passed at the time); the full series is at integration.

## 7. Corrections (statements in earlier versions of this document that the journals proved false or superseded)

1. **§3 "FFmpeg is off in this repository's builds, forced in `CMakeLists.txt:59`"** — false since commit
   `028d3f6`: the option is `DON_ENABLE_FFMPEG`, and the Linux presets set it on (journal 034, follow-up 21).
2. **"The stub is fed by the runtime's own CD streaming hooks (`sceCdStStart`/`sceCdStRead`)"** — false for
   this game. The game reads with `sceCdRead` and sends the stream over DMA4; `sceCdSt*` and `notifyMpeg*` were
   never called (journal 034, follow-ups 1 and 5). The stub is fed by patch 0064.
3. **"The 16 KiB blocks on `0x1000D400` are the stream the player feeds its decoder"** — false: that is DMA
   channel 9 (scratchpad) and what it carries is not identified. The stream is on DMA channel 4 (follow-up 6).
4. **"Without FFmpeg, `sceMpegGetPicture` writes a blank frame"** — true before patch 0064 (journal 007).
   With 0064 the no-FFmpeg decoder never produced a picture; patch 0070 now makes black pictures.
5. **"The video never ends by itself in the port"** (journal 007, §6 item 3) — true in the build of journal 007,
   which had no channel-4 feed. With 0064 and FFmpeg the movie ends by itself (follow-up 7).
6. **"The frame layout the stub writes must match what CRI's callbacks expect"** (a1 risks) — the scramble was
   the GS display buffer (patch 0065), not the decoder's layout.
7. **"The video's audio would still not play (the sound is not connected)"** — with pictures, sound was measured
   (follow-up 7). Its source is not separated from the title music.
8. **§7(c) and the Recommendation (bind `0x36AC70` to "return 1")** — not what was built. The port runs the
   player and presses the button for it (`src/game/don_overrides.cpp:107-112`; a first cut that replaced the
   handler froze the title, journals 023-024). Kept here as history.

## 8. Unverified or open

- **The ending (Circle, `param_2 == 2`):** never run. The skip is the same mechanism as the opening, by reading.
- **Early Start** (follow-up 21's question): the console's behaviour when Start comes 60 blanks into the movie
  was not read (follow-up 22 did not run the oracle). The premise that the console's ADX server runs at priority 48
  is from the port's table.
- **Patches 0071 and 0072:** the full regression suite passed with both (6 of 6, 2026-10-15), and save and load with a saved card passed with sound on and off. The console's priority of the ADX RPC server thread (0072's premise) was not read.
  0072 adds up to 200,000 IOP cycles of IOP time to an RPC when a higher thread is Ready; its effect on other routes
  is not measured.
- **Movie sound:** its level is measured (one run); whether it is the movie's MPEG audio (the second stream of
  each member, `0xC0`, whose payload starts with an MPEG audio sync: **Inferred** from the bytes) or the title's
  music, and its sync, are not measured.
- **The movie's end time** versus the console's, in the same run (section 5).
- **`FUN_0035f800`** (the writer of `DAT_003F1AB4`): whether it runs at run time, and so whether CRI's internal decode
  path is ever chosen instead of `sceMpeg`. Not determined.
- **The 16 KiB blocks on `0x1000D400`** (channel 9): what they carry.
- **Windows:** patch 0069 builds; the game was not run on Windows.
- **Section 1's "Circle mask 0x20"**: from the pad's wire order, not tested.

## How to repeat

```bash
# the archive and its streams (reading)
uv run --no-project python -I tools/afs_survey.py disc/MOVIE.AFS
uv run --no-project python -I work/video/ps_scan.py disc/MOVIE.AFS --bytes 4000000

# the register scan (IPU, FIFOs, DMA3/4, DMAC) and the call graph
uv run --no-project python -I work/video/scan_io.py --elf disc/SLPS_256.75 --csv config/functions.ghidra.csv
uv run --no-project python -I work/video/closure.py --elf disc/SLPS_256.75 --csv config/functions.ghidra.csv --root 0x36A820

# the functions quoted above
bash tools/re/decompile.sh 0x36AC70 0x36A820 0x35D010 0x35CB80 0x35CA80 0x138FB0

# the FFmpeg build and the Linux build with it
bash tools/setup/build_ffmpeg.sh
cmake --preset linux-dev && cmake --build build/linux-dev --target don
```

Runtime tests: section 6. Journal 034 keeps the runs' routes and logs; the T-305 work files under `work/` are not
versioned.
