# 15 · GS dumps (PCSX2 `.gs` / `.gs.zst`)

How the repository reads PCSX2 GS dumps, and how a replay of one is compared. Each fact is
labelled **Declared** (source file and line in `third_party/pcsx2/pcsx2/`) or **Measured**
(observed here, with the dumps in `work/ref/pcsx2/`). The dumps are game data and are not
versioned (AGENTS.md rule 1).

## 1. Container

- **Declared** `GS/GSLzma.cpp:625-640`: the extension picks the reader: `.xz` is LZMA, `.zst` is
  zstd, anything else is raw. `gsreplay` reads `.zst` and raw `.gs`; `.xz` is rejected with an
  error.
- **Measured**: `01-title.gs.zst` decompresses to 7,344,495 bytes.

## 2. File layout

**Declared** `GS/GSDump.h:13-26`:

```
[0xFFFFFFFF] [Header] [state size/4] [state data] [PMODE regs 0x2000] ( [id/1] [data] )*
```

### Header (`GS/GSDump.h:30-41`, nine little-endian u32, packed 4)

| Field | Meaning |
|---|---|
| `state_version` | GS state version; always first so old versions fail safely |
| `state_size` | size of the state block |
| `serial_offset`, `serial_size` | the serial string, stored after the header |
| `crc` | game CRC |
| `screenshot_width`, `screenshot_height` | preview size |
| `screenshot_offset`, `screenshot_size` | preview RGBA, stored after the serial |

The header size written is `sizeof(header) + serial + screenshot` (`GS/GSDump.cpp:46-52`).
**Measured** on 01-title: header block 0x12c02e bytes, `state_version` 9, `state_size`
0x4001fd (4,194,813), crc `0x82ea02b7`, serial `SLPS-25675`, preview 640x480 RGBA.

### State block and privileged registers

- The state block is the GS freeze data (VRAM, registers, texture state); it is loaded as is
  with `don_gs_load_state`. **Measured**: about 4 MiB (4 MiB VRAM plus register state).
- Then comes one block of `0x2000` bytes with the privileged registers (PMODE, DISPFB, ...).

## 3. Packets

**Declared** `GS/GSDump.h:15-26`, `GS/GSDumpReplayer.cpp:283-340`:

| id | Packet | Layout |
|---|---|---|
| 0 | Transfer | `[0][path u8][size u32][data]` |
| 1 | VSync | `[1][field u8]` |
| 2 | ReadFIFO2 | `[2][qwc u32]` |
| 3 | Registers | `[3][0x2000 bytes]` |

- **Path**: written from `Transfer<index>`: 0 `GSgifTransfer1` (Path1Old), 1 `GSgifTransfer2`
  (Path2), 2 `GSgifTransfer3` (Path3), 3 `GSgifTransfer` (Path1New); the replayer maps `path-1`
  to `GIF_PATH`. Path1Old uses a 16 KiB buffer layout.
  **Measured**: every game transfer in the six dumps uses 3 (Path1New); no ReadFIFO2.
  (Corrected on 2026-10-11.) That index does **not** say which GIF path the game used. In
  PCSX2 2.8.2 the GIF unit merges the three paths before the GS: the packets of any path reach
  it through the same call (`pcsx2/MTGS.cpp:446-455`, `Command::GSPacket`, path taken from
  `tag.data[2]` and then dropped; `GSgifTransfer` → `Transfer<3>`, `pcsx2/GS/GS.cpp:419-421`).
  So every dump written by this PCSX2 labels all its transfers 3, and the dumps cannot tell how
  much of a frame comes from the VU1 (path 1), from VIF1 DIRECT (path 2) or from the GIF channel
  (path 3). The `path1=` figure in `gsreplay`'s summary line is that index, not the game's path.
- **VSync**: at each vsync the writer emits a Registers packet and then a VSync packet. The
  replayer ignores the packet's field byte; `gsreplay` uses CSR bit 13 of the latest registers.
- **Payload alignment**: packets are not 16-byte aligned in the file. PCSX2's SIMD GIF
  handlers need alignment, so `don_gs_gif_transfer` copies unaligned data to an aligned
  staging buffer. **Measured**: without it, dumps 04 and 05 crash in
  `GIFPackedRegHandlerXYZF2`.
- A truncated last packet: PCSX2 drops it; `gsreplay` treats it as an error (exit 1).

## 4. Single or multi-frame

The writer ends the dump once the frame count is even after the extra frames
(`GS/GSDump.cpp:91`). **Measured**: all six dumps have 4 vsyncs and 4 registers packets; a
"frame" replay stops after the N-th vsync (`gsreplay --frame N`, counted from 1; default last).

## 5. Producing a dump

- PCSX2: Shift+F8 (or the F8 shortcut in the GS menu) while running the game, or
  `tools/ref/pcsx2_auto.sh`, which does it along the route in `tools/ref/route.txt`.
  Results land in `work/ref/pcsx2/NN-name.gs.zst`.

## 6. Replaying

```bash
# bridge replay (renders the last vsync into a 640x448 image)
build/gs/gsreplay --dump work/ref/pcsx2/01-title.gs.zst out.png --renderer software|vulkan [--frame N]

# PCSX2's own runner (built unmodified by tools/pipeline/build_gs.sh into build/gs/)
XDG_CONFIG_HOME=work/gsdump/xdg PCSX2_NOCONSOLE=1 build/gs/pcsx2-gsrunner \
  -renderer sw|vulkan -ini work/gsdump/runner.ini -surfaceless -dumpdir DIR -- DUMP

# everything, with SSIM and MAD
bash tools/verify/replay_all.sh
```

`runner.ini` has `[EmuCore/GS]` `ScreenshotSize = 2` so the runner writes the internal-resolution
image (640x448), like `don_gs_snapshot_rgba`. The runner writes `<name>_frameNNNNN.png` per
presented frame; frame N is the picture composed at vsync N+1 (**Measured**: with 4 vsyncs it
writes frames 1-3, the last one equals gsreplay's last vsync).

## 7. Results (Measured)

Replay against the runner, same renderer, dumps 01-06 (06 retaken, see below): SSIM 1.0000,
MAD 0.00 on software and Vulkan (bit-identical).

Against PCSX2's screenshot of the same moment (information only; differences are the
screenshot's scaling and the video-out stage):

| Dump | Software SSIM / MAD | Vulkan SSIM / MAD |
|---|---|---|
| 01-title | 0.9811 / 3.37 | 0.9835 / 2.55 |
| 02-menu | 0.9456 / 5.44 | 0.9476 / 5.10 |
| 03-charselect | 0.8930 / 8.51 | 0.9543 / 3.56 |
| 04-match-1v1 | 0.7526 / 9.89 | 0.7602 / 9.25 |
| 05-match-4p | 0.5907 / 18.56 | 0.6088 / 16.91 |

Packets and time (software / Vulkan replay, open takes 45-70 ms more; runner about 1.6 s):

| Dump | Transfers | GIF bytes (all with path index 3, see section 3) | Replay ms |
|---|---|---|---|
| 01 | 2328 | 1,865,888 | 39 / 14 |
| 02 | 2540 | 1,767,232 | 40 / 15 |
| 03 | 2620 | 1,799,616 | 37 / 15 |
| 04 | 3896 | 8,688,288 | 40 / 25 |
| 05 | 6735 | 16,603,936 | 54 / 40 |

### 06-results (solved on 2026-10-11)

The first `06-results` dump was poor: 140 transfers, 14,400 GIF bytes, black on the software
renderer through the bridge and through the runner alike, and nothing to show on Vulkan.
**Measured** afterwards (T-017 follow-up), by taking consecutive dumps from the results screen
onwards: the winner's pose is drawn normally (about 2,460 transfers per frame) but stays on screen
only 2 to 3 seconds; then come a nearly empty fade frame (the 140 transfers), black frames with
no transfer at all, and the character select. There is no alternation of full and empty frames:
the old route waited one second and then spent a few more on the capture, so the dump fell in
the fade, while the screenshot, one frame earlier, still looked right.

The route now captures at once when the results screen is detected, and every capture replays
its own dump with `gsreplay` and is retaken if the picture has fewer than 5,000 distinct colours
(good dumps give 30,000 to 120,000). The dump in use has 2,485 transfers and 3,651,216 GIF bytes,
and `tools/verify/replay_all.sh` gives SSIM 1.0000, MAD 0.00 against the runner for the six
dumps on both renderers (**Measured**, 12 of 12).

Two things to remember from the first dump: `replay_all.sh` reports two black images as equal,
so an "OK" there is not proof of a picture; and a savestate taken in a fade shows black for its
first frames after loading.

### Patches 0002 / 0003

**Measured**: gsreplay images of 01-05 (both renderers) are byte-identical with and without
patches 0002 and 0003, so these dumps do not exercise them. The runner was not compared
without patches.
