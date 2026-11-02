# 08 · External references and reusable pieces

Status as of **2026-10-11**. This field changes every week: before relying on a fact from
here, check the date of the project's last commit.

Except for PS2Recomp, which has been built and run here, **everything else is "declared"**: it
comes from reading the repositories, not from running them. The read-only clones are in
`/tmp/don-refs/` (they are lost on reboot; clone them again with `git clone --depth 1`).

## Overview (recomp.fyi)

- 1,771 projects listed; 91 for PS2 (55 decomp, 33 recomp, 3 source port). No PS2 project
  declares "playable" in the table, but 80 of 91 declare no status; several READMEs do say it.
- DON does not appear: nobody is porting it.
- Only 1 row of the 1,771 declares running in a browser (a GBA port).

## PS2 projects that matter for DON

| Project | Architecture | Declared status | Platforms | License |
|---|---|---|---|---|
| `ran-j/PS2Recomp` | Recompiler + runtime + IOP emulator | Base tool | Windows (tested), Linux | GPL-3.0 |
| `danisandoval/fukami` (Ridge Racer V) | PS2Recomp + PCSX2 GS and SPU2 + ahead-of-time compiled VU | v1.0.0, 60 fps | Linux, macOS | GPL-3.0 |
| `drakolordx7/KillzoneRecomp` | PS2Recomp + PCSX2 GS, microVU, IPU and SPU2 as static libraries | Test build, 117-140 fps, first mission | Windows | GPL-3.0 |
| `z3xox/BT3-Recomp` (Budokai Tenkaichi 3) | PS2Recomp fork with its own OpenGL GS; paraLLEl-GS experimental | "Playable", ~30 fps | Linux, Windows | GPL-3.0 |
| `KIexster/god-of-war-recomp` | PS2Recomp + 95 patches, own GS and VU1 | Gameplay at ~12 fps | Windows | GPL-3.0 |
| `idiazraz/ufe3-recomp` (Ultraman FE3) | PS2Recomp + 1 patch, on Linux with clang | Stops before the title | Linux | GPL-3.0 |
| `yanm1103/War_of_the_Monsters_recomp` | Decomp with symbols + PS2Recomp | Interface loop, no validated image | Windows | GPL-3.0 |
| `literalynn/BurnoutDominatorReversed` | PS2Recomp, RenderWare game | Black screen | Windows, Linux | GPL-3.0 |
| `nicolasserpa/shadow-the-hedgehog-recomp` | PS2Recomp, RenderWare 3.7 game | Boots | Windows | GPL-3.0 |

Reading: the projects that run at full speed **do not use the PS2Recomp GS or VU1**; they link
the PCSX2 ones. The ones that use the PS2Recomp ones are stuck or slow. Nobody has yet
solved a PS2 RenderWare game on the PS2Recomp GS.

## PS2Recomp (`third_party/PS2Recomp`)

- Pinned at commit `2c5fbb9389e11dd95693385969490c9e8e6f57b4` (2026-10-10).
- **Measured** on this machine: the tools (`ps2_analyzer`, `ps2_recomp`) and the runtime
  (`ps2EntryRunner`) build with GCC 11.4. Exact conditions in
  [09-environment-and-tools.md](09-environment-and-tools.md).
- Recommended flow for games without symbols: Ghidra + `ps2xRecomp/tools/ghidra/ExportPS2Functions.java`
  → function CSV (`Name,Start,End,Size`) + TOML → `ps2_recomp config.toml`.
- TOML keys: `[general]` with `input`, `output`, `ghidra_output`, `stubs`
  (`name@0xADDRESS`, binds an address to a runtime handler; also `ret0@`,
  `ret1@`, `reta0@`), `entry_points`, `skip`; `[patches] instructions`; `[jump_tables]`.
- The generated code is built inside the `ps2EntryRunner` target, which takes all the
  `.cpp` files in `ps2xRuntime/src/runner/`. The executable receives the ELF path as its first argument.
- Per-game modifications: `PS2_REGISTER_GAME_OVERRIDE(name, "SLPS_256.75", entry, crc32, fn)`
  in `ps2xRuntime/include/game_overrides.h`; inside it, `runtime.replaceFunction(...)` or
  `ps2_game_overrides::bindAddressHandler(runtime, addr, "handler")`.
- Disc: `PS2Runtime::IoPaths` with `cdRoot` (folder) and `cdImage` (ISO); memory card in `mcRoot`.
- The internal graphics interface (`GSRasterBackend` in `ps2xRuntime/include/runtime/gs/gs_backend.h`)
  works with already interpreted primitives. It is **not** the hook point for the PCSX2 GS,
  which wants the raw GIF byte stream.
- FFmpeg is optional (`-DPS2X_ENABLE_FFMPEG=OFF`).
- PR #277 (fix for `SQRT.S`/`RSQRT.S`) is still open; it applies cleanly on the pinned commit and
  is stored in `patches/ps2recomp/0002-fpu-sqrt-rsqrt-operands-pr277.patch`. In DON it fixes
  47 of the 96 `sqrt.s` instructions.

## Fukami: what can be reused

Repository at commit `e881812`. PCSX2 pinned at **v2.8.2**, commit
`fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`.

| Piece | Where | Useful for DON? |
|---|---|---|
| PCSX2 GS bridge | `tools/pcsx2-gs-bridge/` (`bridge.cpp` 3,238 lines, header `rrv_pcsx2_gs_bridge.h`) | Yes as an API model. Not as a direct copy: it ships a default Ridge Racer V graphics profile (384 lines mention it) and only builds for macOS and Linux |
| PCSX2 patches | `tools/patches/pcsx2/` | Generic: `0001` (CMake hook and local memory read), `0004` (fix for `GSState::Transfer` with split data), `0005` (texture cache), plus `pcsx2-gs-bridge-linux.patch`. The rest are for Ridge Racer V or macOS |
| PCSX2 SPU2 core behind a C API | `tools/pcsx2-spu2/` | Yes, it is generic: `write16`, `read16`, `dma_write`, `dma_read`, `advance`, `sync`, `pull_output` |
| Ahead-of-time compiled VU | `tools/vu-aot/`, `src/vu-aot/` | Only as a reference. It extracts the microprograms statically from the ELF, as could be done in DON. It is tied to its modified copy of PS2Recomp |
| Sound driver | `src/audio/` | No: it is a rewrite of the Ridge Racer V driver |
| Testing method | `docs/TESTING.md`, `scripts/pcsx2_ref.sh`, `scripts/pine.py`, `tools/gs-replay/` | Yes as an idea: suite without game data + comparison with PCSX2 by first divergence |

How its bridge works (C API, version 5):

- `submit(path_id, bytes, size)`: already arbitrated GIF bytes; inside it calls `GSgifTransfer`.
  No PCSX2 thread of its own: it is called synchronously from a single "GS owner" thread.
- `vsync(regs[19], field_index, field_parity)`: passes the 19 privileged registers and presents.
- `readback(&csr, &siglblid)`: returns `SIGNAL`/`FINISH`/`LABEL`; the caller must mirror them
  in its own registers visible to the game.
- `read_local_memory(...)`: GS read back to the CPU.
- The window is SDL; the native handle (X11 or Wayland on Linux) is passed to it and PCSX2
  creates the Vulkan device and presents.

Building the PCSX2 GS on Linux according to its script: `ENABLE_QT_UI=OFF`, `ENABLE_TESTS=OFF`,
`USE_VULKAN=ON`, `USE_OPENGL=OFF`, `X11_API=ON`, `WAYLAND_API=ON`, `DISABLE_ADVANCE_SIMD=ON`,
with clang. Dependencies built from source: Vulkan-Headers 1.4.328.1, SDL3 3.4.12,
shaderc 2026.2, libpng, libjpeg-turbo, libwebp, lz4, zstd, rapidyaml, plutovg/plutosvg,
libbacktrace. Qt and FFmpeg are not needed. About 4 GB of disk.

## Killzone: what can be reused

Repository at commit `5f07000`. PCSX2 at `646df006a4` (main branch, unmodified).
PS2Recomp base `75d729c`.

| Piece | Where | Useful for DON? |
|---|---|---|
| GS as a static library | `ext/kzgs` (~2.3k lines of glue) | Very clear minimal API: `kzgsOpen(hwnd, cfg, privRegs)`, `kzgsGifTransfer(path, data, qwords)`, `kzgsVsync(field)`, `kzgsReadback(...)`. MSVC/Windows only |
| microVU as a library | `ext/kzvu` (~1.9k lines) | It is the path to 60 fps. `kzvuExecute`, `kzvuWriteMicro`, `XGKICK` callback. Windows only; VU memory must live in the executable image |
| IPU as a library | `ext/kzipu` | Option for the videos |
| SPU2 as a library | `ext/kzspu2` + patch `0006` | Patch `0006` is the key piece for DON: a hook in the IOP emulator that routes SPU2 registers (`0x1F900000`-`0x1F9007FF`), DMA 4 and 7 and interrupts to an external SPU2, **with the original IRX modules running** |
| PS2Recomp patches | `patches/ps2recomp/` (26) | Almost all generic; see the catalog below |
| Test harness | `tools/scripts/run_headless.ps1`, `parity_pcsx2.py`, `parity_compare.py` | Headless mode with periodic screenshots and scripted input; comparison with PCSX2 by PINE and savestate |
| Function discovery | `tools/scripts/regen.py`, `find_code_pointers.py`, `config/code_pointers_reject.txt` | Same problem as DON: functions reachable only by pointer |

Killzone figures as an order of magnitude: 16,931 functions and 65,472 Ghidra labels;
~20,000 generated C++ files; building takes 15 min with MSVC and 100 s with clang-cl.

## Catalog of known fixes, by symptom

When the port gets stuck, look here before investigating from scratch. The "Source" column says
where to read the solution; **none is applied yet** unless stated.

| Symptom | Known cause | Source |
|---|---|---|
| Wrong distance or normalization calculations | `SQRT.S` reads the wrong register | PR #277. **Applied** (`patches/ps2recomp/0002`) |
| A thread that was suspended inside an `entry_*` block does not resume | Resume points were missing | Killzone `0001`. Already upstream (#271) and therefore in our commit |
| Disc reads from the IOP land on wrong sectors | File lookup used the folder and not the image | Killzone `0003` |
| Corrupted text and noisy sprite edges | A PATH2 image upload that continues in another `DIRECT` was shifted by 8 bytes | Killzone `0005` |
| No sound with the real IRX modules | SPU2 emulation is missing | Killzone `0006` + `ext/kzspu2`; Fukami `tools/pcsx2-spu2` |
| Random hangs | A thread yield in a recursive call was mistaken for a return | Killzone `0008`. Reproduced with `PS2X_STRESS_YIELD=50` |
| The game corrupts runtime memory (or the other way round) | Runtime allocations overlap the game heap | Killzone `0008` (move the arena); God of War `heap` |
| Geometry that disappears | `LQ` on an unaligned address (`lq`/`qfsrv` idiom) | Killzone `0010` |
| A match frame is cut off | VIF1 DMA chains were truncated at 4,096 tags | Killzone `0010`; Ultraman FE3 |
| `NaN`/`Inf` values and a huge performance drop | Floating-point semantics different from the PS2 | Killzone `0009`: build with `-ffp-contract=off`, never `-ffast-math`; enable FTZ/DAZ on the game thread |
| Hang at startup waiting for the IOP | An RPC wait outside a thread did not block | Killzone `0013` |
| The sound driver loses its wakeup | The `cdvdman` completion arrived too early (128 cycles) | Ultraman FE3 (raise to 36,864 cycles) |
| The game stalls at the memory card check | `sceMcSync` must return "running" once | Ultraman FE3 |
| `memalign` frees the whole block | Heap implementation | Ultraman FE3 |
| Textures uploaded with `LoadImage` misplaced | `libgraph` block pointer multiplied by 8 | Ultraman FE3 |
| `switch` table not detected | Index with sign extension | Ultraman FE3 |
| Analog buttons crossed | Order of the pressure bytes: L1, R1, L2, R2 | Killzone `0026` |
| The game runs slow even with spare CPU | Frame timer and cycle clock | Killzone, `docs/findings.md` lines 697-806 |
| The GS status query does not advance | The external GS does not deliver `SIGNAL`/`FINISH`/`LABEL` | Killzone `0012`: the runtime software GS processes only the packets that write those registers |

## Other resources

| Resource | What for |
|---|---|
| `aap/librw` (MIT) | RenderWare reimplementation; reads PS2 `.dff` and `.txd`. For future stages |
| `chaoticgd/ghidra-emotionengine-reloaded` (Apache-2.0) | Ghidra extension for the PS2 processor. v2.1.38 ships a build for Ghidra 12.1.4 |
| `morluto/rea` (MIT) | MCP server that gives an agent access to Ghidra. Its documented MIPS path does not support PS2 ELF files (`docs/ghidra-mips.md`) |
| `Nemesis-driod/decompile-and-verify` (MIT) | Method: every rewritten function needs a verdict from an oracle. Its scripts were only tested on x86-64 |
| `doldecomp/gnt4` | Partial decomp of another Eighting game (GameCube), stopped since 2022 |
| `LivewireCB/RW-3.5_Decomp`, `bfbbdecomp/bfbb` | RenderWare function names (GameCube, version 3.5) |
| decomp.me, repository `decompme/compilers` | 23 CodeWarrior versions for PS2 (`mwcps2-*`) and 30 for GameCube |
