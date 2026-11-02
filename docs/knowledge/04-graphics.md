# 04 · Graphics: RenderWare, VU1 and GS

## DON uses RenderWare 3.7.0.2

**Measured.** It is not a graphics engine of Eighting's own. Evidence:

- Names of nodes of RenderWare's PS2 pipeline in the executable: `PS2All.csl`,
  `PS2AllMat.csl`, `PS2DMAChain.csl`, `PS2LightsBlock.csl`, `PS2DMASessionRecord.csl`,
  `PS2Mesh.csl`, `PS2xyz.csl`, `PS2xyzw.csl`, `PS2uv.csl`, `PS2uv2.csl`, `PS2rgba.csl`,
  `PS2normal.csl`, `PS2user1..4.csl`, `PS2Im3DFastTransform.csl`, `PS2PTank.csl` (particles),
  `PVSData.csl`, `PVSWorldSector.csl` (precomputed visibility).
- File formats `.dff`, `.txd`, `.bsp`, `.anm`, `.uva` with version stamp 3.7.0.2
  (see [03-data-formats.md](03-data-formats.md)).
- Debug strings with RenderWare vocabulary: `ClumpsInFrustum`, `SectorsInFrustum`,
  `Raster  %08x %08x`, `ZRaster %08x %08x`, `ViewWindow %4.2f,%4.2f`.

On top of RenderWare there is the game's own code (task system, `.seq` scripts,
`.par` particles, `silhouette` string).

For **stage 1 this is not exploited**: the hardware below RenderWare is emulated. It matters
for future stages (own renderer, true widescreen) and for knowing what to expect from the
microcode.

## The path of a frame on PS2

Terms, once:

- **EE**: the main CPU. Its code is what gets recompiled.
- **VU0 / VU1**: two vector units. VU0 is used from CPU code ("macro mode");
  VU1 runs independent microprograms that transform vertices.
- **VIF1**: the interface that feeds VU1: it unpacks data, loads microprograms and starts them.
- **GIF**: the interface that feeds the graphics chip. It has three inputs: PATH1 (what VU1
  emits), PATH2 (direct data that goes through VIF1) and PATH3 (data sent by DMA to the GIF).
- **GS**: the graphics chip (rasterizer). It does not transform or light.
- **DMA**: the mechanism that moves blocks of memory to VIF1 and the GIF.

```
game code ─▶ RenderWare ─▶ DMA chains ─▶ VIF1 ─▶ VU1 (microprogram) ─PATH1─▶ GIF ─▶ GS
                                          └────────── textures, state ────PATH2/3─▶ GIF ─▶ GS
```

## VU microcode in the executable

**Measured** (heuristic detection of microprogram load headers in the data area;
the order of magnitude is reliable, the exact number may vary by a few units).

| | |
|---|---|
| Load blocks (`MPG`) | 500 |
| Contiguous programs | 141 |
| VU instructions | 113,944 (911 KB) |
| Range in the ELF | `0x0039030C`–`0x004B2878` |
| By destination address | 121 at `0x000`, 11 at `0x300`, 8 at `0x400`, 1 at `0x100` |
| By size | 32 of up to 256 instructions, 58 of 257 to 1,024, 51 of more than 1,024 |

VU1's microcode memory holds 2,048 instructions, so the large programs
(up to 1,950) fill almost all of it. The full list is in
[data/vu_programs.tsv](data/vu_programs.tsv).

What it means:

- All of them are **static in the executable**: they can be extracted without running the game.
  That allows compiling them ahead of time if one day a JIT has to be avoided.
- The number fits RenderWare, which ships variants of its microcode per combination of
  primitive type, lighting, fog, clipping, skinning and material effects. **Unverified**
  which ones are really used in a match (task T-201).
- VU0 in micro mode is not used in practice: there is a single `vcallmsr`, at `0x102B14`, inside
  the SDK.

## Sony graphics libraries present

Recognized by the PS2Recomp analyzer (name and address in `work/analyzer/config.toml`):

- `libgraph`: `sceGsResetGraph` (`0x100350`), `sceGsResetPath`, `sceGsSetDefDispEnv`,
  `sceGsPutDispEnv`, `sceGsSetDefDrawEnv`, `sceGsPutDrawEnv`, `sceGsSetDefDBuff`,
  `sceGsSwapDBuff`, `sceGsSyncV` (`0x1011D8`), `sceGsSyncPath` (`0x101270`),
  `sceGsExecStoreImage`, `sceGsSyncVCallback`, `sceGsSwapDBuffDc`.
- `libdma`: `sceDmaReset`, `sceDmaSend`, `sceDmaSendN`, `sceDmaSendI`, `sceDmaSync`...
- Packets: `sceVif1PkOpenDirectCode`, `sceVif1PkCloseGifTag`, `sceGifPkCloseGifTag`, `sceGifPkAddGsAD`.
- `sceDevVif0Reset` and `sceDevVu0*` functions.

`sceGsSyncV` is the natural point to detect "the game has reached its frame loop".

The game contains `sceGsExecStoreImage`, which reads pixels from the GS back to memory.
**Unverified** whether it uses it in a match; if it does, the GS bridge has to support reading
back (task T-120).

## What this implies for the port

1. **GS**: PCSX2's renderer will be used as a library. It receives the GIF byte stream
   and handles everything else. See [08-external-references.md](08-external-references.md).
2. **VU1**: first the interpreter that PS2Recomp already ships (slow, good enough to validate the
   image); then, to reach 60 fps, PCSX2's VU recompiler (microVU) as a library.
3. **GS signals to the game** (`SIGNAL`, `FINISH`, `LABEL`, vsync interrupt): the embedded
   PCSX2 renderer does not deliver them by itself; they have to be forwarded to the runtime.
4. **Scratchpad and direct access to VU memory**: the game uses the scratchpad in 401 places and
   mapped VU memory in 12. The PS2Recomp runtime declares scratchpad support; direct
   access to `0x1100xxxx` is **unverified** (task T-121).

## Direct accesses to vector unit memory

On the PS2 the memory of the two vector units is mapped in the main processor's address space
(VU0 microcode `0x11000000`, VU0 data `0x11004000`, VU1 microcode `0x11008000`, VU1 data
`0x1100C000`). A program can read and write those addresses directly instead of going through
VIF packets. Task T-121 looked for such accesses in the executable.

**Measured** (scan of the main text `0x100000`–`0x3902D4`, decoded as R5900, every instruction
`lui rt, 0x1100`; also `lui rt, 0x1101`, which would build `0x1100C000` with a negative
`addiu`/`ori`: none).

**Result: there are 12 `lui 0x1100`, as the instruction census said, and none of them is an
access to vector unit memory.** None is followed by a load or a store through the register it
builds. In all 12 the `0x1100` is the upper half of a **VIF code constant**: command `0x11`
(`FLUSH`) in the top byte, or an engine id. Direct reads and writes of `0x1100xxxx` by the game
are therefore **0** (Measured: no other `lui` with that upper half exists in the text, and the
low half of every use is shown below).

| `lui` address | Value built | Use | Function (CSV name) | Called from | What it is for |
|---|---|---|---|---|---|
| `0x15D5B0` | `0x110000C8` | argument `a1` of `FUN_00257700` | `FUN_0015d150` (`sub_0015D150`) | `0x1A6888` (inside `entry_001a6884`) | Not an address: an engine resource id passed to a lookup (`FUN_00257700(obj, id, flag)`) |
| `0x15D5CC` | `0x110000E5` | same call, other branch (flag 2) | `FUN_0015d150` | same | same |
| `0x2AE424` | `0x1100000011000000` (`lui`, `dsll 16`, `ori 0x1100`, `dsll 16`) | 64-bit constant stored in a packet | `FUN_002ae320` | `FUN_0025b8f0`, `FUN_0025d660`, `FUN_002b0210` | Two `FLUSH` VIF codes in a packet being built (the code is conditional: `movn`) |
| `0x2AEA80` | same | stored with `sd` next to a DMA tag word (`0x6000....`) | `FUN_002ae320` | same | same, other path of the same function |
| `0x2D61B0` | `0x11000000` as high half of a `pcpyld` | quadword stored with `sq` | `FUN_002d6038` (`sub_002D6038`) | `FUN_002df720` | `FLUSH` in a VIF packet; next to it `0x13000000` (`FLUSHA`) is built at `0x2D61C8` |
| `0x2EA3C0` | `0x11000000` high half of `pcpyld` with `0x01000404` | `sq` into the packet buffer | `entry_002ea350` | none found | `STCYCL` (`0x01000404`) + `FLUSH` quadword of a VIF1 packet |
| `0x2EA568` | `0x11000000`, duplicated by `pcpyld` | `sq` into the packet buffer | `FUN_002ea500` | none found | `FLUSH` quadword |
| `0x2EAB98` | `0x11000000` high half of `pcpyld` | `sq` into the packet buffer | `FUN_002eab78` | 8+ callers in `0x2EB0BC`–`0x2EC4A8` | `FLUSH` quadword |
| `0x2EAD28` | `0x11000000` high half of `pcpyld` | `pcpyld` result kept in a register | `FUN_002eabe0` | `FUN_002ed420` (`0x2ED45C`) | `FLUSH` quadword |
| `0x2EAEC0` | `0x1100000011000000` (`dsll` sequence) | `pcpyld` | `FUN_002eabe0` | same | two `FLUSH` codes |
| `0x2EAED0` | `0x11000000` | `pcpyld` | `FUN_002eabe0` | same | `FLUSH` |
| `0x2EAEDC` | `0x11000000` | `pcpyld` | `FUN_002eabe0` | same | `FLUSH` |

Notes on the table:

- All the functions from `0x2AE320` to `0x2EABE0` appear in `work/analyzer/config.toml` under
  the comment "Uses SIMD instructions" (they use `pcpyld`/`sq`); `FUN_0015d150` under "Contains
  heavy loops". **Declared** by the analyzer configuration, not checked here.
- The vector unit is not touched by these instructions. They write VIF code words into packet
  buffers that are later sent by DMA to VIF1. That path is the one `ps2_vif1_interpreter.cpp`
  handles (see below).
- Callers are from `tools/re/callers.py` (`jal` only). A function with no `jal` caller
  (`entry_002ea350`, `FUN_002ea500`) is reached some other way (indirect call or a jump
  table); **Unverified** which.
- No SDK name (`sceDevVu0*`, `sceVu0*`...) is attached to any of these functions in
  `work/analyzer/config.toml`, and `config/stub_policy.toml` has no entry for them: they are
  recompiled as ordinary game code. **Measured** (grep of the nine function addresses in both
  files).
- A direct access could also be built without a `lui 0x1100`: with a base address loaded from
  data, or from a pointer table. This scan cannot see it. **Unverified**; the cheap check at
  run time is a diagnostic in the runtime on the mapped range (see the conclusion).

### How the runtime handles the range

**Measured** (`third_party/PS2Recomp/ps2xRuntime`, pinned clone):

- The four areas are defined in `include/runtime/ps2_memory.h:37-46`: VU0 code `0x11000000`
  (4 KB), VU0 data `0x11004000` (4 KB), VU1 code `0x11008000` (16 KB), VU1 data `0x1100C000`
  (16 KB).
- The memory is allocated and zeroed in `src/lib/ps2_memory.cpp:373-381` (`m_vu0Code`,
  `m_vu0Data`, `m_vu1Code`, `m_vu1Data`).
- `PS2Memory::mapVuMemory` (`ps2_memory.cpp:535-566`) maps a physical address to one of the four
  arrays and rejects an access that does not fit entirely inside one area.
- **Reads** go through it in `read8`, `read16`, `read32`, `read64` and `read128`
  (`ps2_memory.cpp:687, 723, 774, 817, 855`). **Writes** the same in `write8`, `write16`,
  `write32`, `write64`, `write128` (`ps2_memory.cpp:883, 927, 991, 1046, 1103`). All four areas
  are readable and writable, at every access width.
- A write to VU0 or VU1 **code** calls `markVU0CodeModified()` / `markVU1CodeModified()`
  (for example `ps2_memory.cpp:887-890`), which bumps a generation counter
  (`getVU1CodeGeneration`, `ps2_memory.h:284`). A write to VU data does not need it.
- **It is the same memory the VU1 interpreter runs from.** The MSCAL callback passes
  `m_memory.getVU1Code()` and `m_memory.getVU1Data()` to `m_vu1.execute` and `m_vu1.resume`
  (`src/lib/ps2_runtime.cpp:682-683, 700-701`), and the VIF1 `MPG` handler copies microcode
  into `m_vu1Code` with `memcpy` (`src/lib/ps2_vif1_interpreter.cpp:472-480`). A direct write to
  `0x11008000` or `0x1100C000` is therefore seen by the interpreter, with no extra step.
- **How recompiled code reaches it:** through the generic `PS2Memory` read and write helpers
  (the `read*`/`write*` above), by address. The `[mmio]` table of the recompiler configuration
  (`config_manager.cpp:150-165`, applied at `ps2xRecomp/src/lib/ps2_recompiler.cpp:2171-2175`,
  which marks one instruction address as an MMIO access) is not needed for this range. The
  repository's `config/*.toml` has no `[mmio]` entry (grep finds none). **Measured.**

### Conclusion

- **For the port, none of the 12 matters as a vector unit access.** They are VIF `FLUSH`
  constants and two engine ids; nothing in them reads or writes `0x1100xxxx`. VU1 code and data
  reach the interpreter only through VIF1 packets (`MPG`, `UNPACK`...), which is what D5
  assumes.
- The runtime would support a direct access if one existed: all four areas are mapped for all
  widths, and the mapped VU1 arrays are the ones the interpreter executes. For the later
  microVU backend (D5), a direct write to `0x11008000`/`0x1100C000` would have to be forwarded
  to microVU's memory as well; the current runtime stores it only in its own arrays. That bridge
  is not needed unless a direct access is found.
- **Unverified:** that the game never reaches the area through a computed base (see the notes
  above). A run-time check is the safe test: log, with a `DON_`-prefixed variable and no cost
  when off, any `read*`/`write*` whose address falls in `0x11000000`–`0x11010000` and run a
  match. Not done here (read only task).

## Reading pixels back from the GS

Task T-120. Question: does the game ever copy pixels from GS local memory back to main memory
(a "local to host" transfer)? If it does, the GS bridge (D4) needs a read-back path; if it
does not, the bridge can leave it out at first.

**Conclusion.** There is exactly one read-back routine in the executable, `sceGsExecStoreImage`
(`0x00101728`), and exactly one caller of it, the RenderWare PS2 raster-lock handler
`FUN_002dd1e8`. That handler is never called with `jal`: it is only reachable through
RenderWare's device function table (a data word at `0x003CA40C`). No game code was found that
locks the frame buffer for reading. The only game-side code that goes through the generic
raster-to-image path (`FUN_00292298`, a texture compositing routine) works on loaded textures,
not on the frame buffer. So: **no direct calls from game code; the read-back path exists inside
RenderWare and cannot be ruled out as used at run time.** Whether it is ever executed, and
when, is **Unverified** and has to be settled at run time (see the last subsection).

### How a read-back is done on PS2 (terms, once)

- **BUSDIR** (privileged GS register, `0x12001040`): selects the direction of the GS bus. Value
  1 = GS to host (read-back), 0 = host to GS (normal).
- **TRXDIR** (GS register `0x53`, set through a GIF packet): direction of an image transfer.
  0 = host to local (texture upload), 1 = local to host (read-back), 2 = local to local.
- **VIF1 FIFO** (`0x10005000`) and **DMA channel 1** (`0x10009000`): where the data comes back.

### Direct search in the main text (`0x100000`-`0x3902D4`)

**Measured.** A scan of every `lui` + `addiu/ori/load/store` pair that forms an address in the
privileged GS block (`0x1200xxxx`), the VIF1 FIFO (`0x10005000`) and DMA channel 1
(`0x1000900x`):

| Register | Places that touch it | Functions |
|---|---|---|
| BUSDIR `0x12001040` | `0x101A88`/`0x101A90` (writes 1), `0x101B44`/`0x101B48` (writes 0), `0x101D4C`/`0x101D5C` (writes 0) | all inside `sceGsExecStoreImage` (`0x00101728`-`0x00101DB4`) |
| VIF1 FIFO `0x10005000` | `0x100534` (store), `0x101AFC`, `0x101BF4`, `0x101C9C`, `0x101CEC`, `0x101D80` (stores of a quadword), `0x1039CC`/`0x103A2C` (register dump), `0x2D3B4C`/`0x2D3C88` (store) | a VIF1 push helper (`0x1004F0`), `sceGsExecStoreImage`, a VIF1 register save function (`0x1037D8`), and a GIF/FIFO packet routine (`FUN_002d3a60`) |
| DMA channel 1 `0x1000900x` | many places in `0x101270`-`0x101BC4` and `0x2D3B6C`-`0x2D3E84` | `sceGsSyncPath` (`0x101270`), `sceDmaSync`/`sceDmaSend` family, `sceGsExecStoreImage`, `FUN_002d3a60` |

No other function writes BUSDIR, and nothing outside `0x101728` writes it with 1. Inside the
function, 1 is written first (`0x101A90`) and 0 is written on every exit path (`0x101B48`,
`0x101D5C`), plus a write of `0x100`/`2` to CSR (`0x12001000`) to reset the FIFO and clear
FINISH. The accesses to the VIF1 FIFO that were decoded are `sq` (writes), not reads. How the data
comes back (DMA channel 1 with the "from device" direction) was not decoded bit by bit
(**Unverified**); the function does program DMA channel 1 (`0x1000900x` accesses inside it).

**TRXDIR.** Measured: `sceGsSetDefStoreImage` (`0x001015E8`) is the function that fills the GIF
packet for a store (it loads `0x50`, `0x51`, `0x52`, `0x53` = BITBLTBUF, TRXPOS, TRXREG, TRXDIR
and a value of 1 at `0x1016CC`). Other places in the executable that load the constant `0x53`
as an A+D register number: `0x2DB2F0`, `0x2DB56C`, `0x2EEF04`, `0x36A56C` (and `0x178524`,
`0x380228`, which are not GS packets). In the four packet builders the data paired with `0x53`
is zero (`TRXDIR = 0`, host to local, the normal texture upload), so none is a read-back.
**What static reading cannot settle:** a TRXDIR value that is computed (not a constant), or a
packet stored in the data area ready-made by the asset pipeline. It does not rule out a
read-back that is built from a data block; the BUSDIR search above does cover this, because the
BUSDIR write must be in code, and there is only one.

### The one caller chain

**Measured** (`tools/re/callers.py`):

| Function | Callers (`jal`) |
|---|---|
| `sceGsExecStoreImage` `0x00101728` | `FUN_002dd1e8`: `0x2DD44C`, `0x2DD464`, `0x2DD6A4`, `0x2DD6C0` |
| `sceGsSetDefStoreImage` `0x001015E8` | `FUN_002dd1e8`: `0x2DD3F0`, `0x2DD64C` |
| `FUN_002dd1e8` | none (`0 calls`); referenced by a data word at `0x003CA40C` |

`FUN_002dd1e8` is the PS2 implementation of **`RwRasterLock`** (RenderWare standard function
`rasterLock`). Evidence: it takes a raster (`param_2`) and a lock mode (`param_3`, mip level in
the high byte, as `RwRasterLock` does), reads the raster type from byte `+0x20` of the raster,
returns a pixel pointer in `*param_1`, sets the "locked" flags in byte `+0x22`, and the table
at `0x003CA3A0` that holds it is RenderWare's list of (function id, handler) pairs for the
device standard functions (ids 1, 0xA, 0x15, 0x14 ... are camera begin/end update, camera
clear, show raster; id 0xF is lock and id 0x10 the next entry, `0x2DDAB8`, is unlock).
The numbering is that of RenderWare 3.7's `rwSTANDARD...` enumeration written from memory:
**Declared**, not verified against the RenderWare headers; the behaviour of the function is
Measured.

What it does with the raster type:

- Type 2 (camera, the frame buffer; path at `0x2DD618`-`0x2DD6D4`) and type 5 (camera texture,
  a texture that is rendered to; path at `0x2DD3A8`-`0x2DD6D4`): **always downloads the raster
  from the GS** with `sceGsSetDefStoreImage` + `sceGsExecStoreImage`, in strips of at most
  `0xFFFF0` bytes, waiting on DMA channels 1 and 2 before each strip (`FUN_00102278`) and
  calling `sceGsSyncPath` (`0x101270`) first. It does not check whether the lock mode asks for
  reading: any lock of such a raster, even for writing, does the transfer. The destination is
  the raster's host buffer (`raster+4`), made coherent with `FUN_0010cb98` (cache writeback)
  before the transfer. The GIF packet for the transfer is built in a fixed buffer at
  `0x545D80`.
- Other types (normal textures, z buffer): no GS access; the lock computes the mip level
  address in the raster's host copy and returns it. (Measured in the same function; this is the
  "no cost" case.)

Wrappers (Measured): `RwRasterLock` = `FUN_002c9d28`, which calls the function pointer stored at
`0x005406C4`; `RwRasterUnlock` = `FUN_002c9d68` (pointer at `0x005406C8`).

### Who locks rasters

**Measured.** `jal` callers of `FUN_002c9d28` (lock) and `FUN_002c9d68` (unlock):

| Caller | What it is | Where it is used |
|---|---|---|
| `FUN_002cb818`, `FUN_002cbef8` | RenderWare image/raster file functions (take file names of up to 255 characters, build images, lock/unlock rasters in several places) | `FUN_002cb818` is called only by `FUN_002cd400`; `FUN_002cbef8` and `FUN_002cd400` have no callers and no data references. Looks like library code that the game does not use (**Unverified**, a static "no callers" result does not prove it is dead) |
| `FUN_002e3328`, `FUN_002e4ee0`, `FUN_002e6540` | device handlers for ids `0x11`, `0x12`, `0x13` (the raster render family: copy a raster onto another) | call lock/unlock at several points (switch cases of the handler; what they lock was not traced: **Unverified**). A read-back happens there only if the raster is of camera or camera-texture type |
| `FUN_002e8210`, `FUN_002ea240` | device handlers for ids 6 and 7 (raster to image, image to raster) | called through the table |

The raster-to-image call that game code does reach is `FUN_002c94c8` (`RwImageSetFromRaster`,
calls the id 6 handler):

- `FUN_00292298` (twice, `0x292304` and `0x292344`) builds a new image from the raster of a
  texture, optionally combines it with a second one (mask: it fills a white 32-bit image when
  no second texture is given), and creates a new texture. It is reached from
  `FUN_002929a0`, a texture read callback that concatenates two texture names and uses two slots
  (`slot` structure of `0x40` bytes), registered in the initialization of the game
  (`FUN_00292708` is called from `FUN_00136d40`, which also installs the first interrupt handler;
  so it is part of start-up, not of a frame loop). The rasters involved are loaded
  textures, type 4, which take the no-GS branch above. So this route does not read the frame
  buffer, as far as static reading can show.
- `FUN_002cbef8` also calls it (`0x2CBF50`), see the table above.

**Not found:** any function that calls the lock wrapper outside the RenderWare range
(`0x2B9000`-`0x2F0000`, where all the callers above are), that is, no game function that locks
the screen raster or a render-target raster directly, and none that saves a screenshot or a
memory card icon from the GS. Strings searched: `Screen`, `screen`, `capture`, `Capture`,
`snap`, `shot` (no match in the main text). Calls through the function pointer at
`0x005406C4` without going through the wrapper were not searched beyond the one load found
(`0x2C9D38`).

### What the port needs

- During a match: **no direct use found.** The bridge can leave GS reads (local to host
  transfers) out of the first version, as long as it logs when one is requested. The residual risk
  is a RenderWare-internal lock of a camera or camera-texture raster (the raster render
  handlers, or the image functions) that this analysis cannot rule out; no game function
  outside RenderWare calls the lock wrapper.
- If it does happen, the recompiled code will reach `sceGsExecStoreImage`, which writes BUSDIR,
  starts DMA channel 1 and waits for it. With the PCSX2 GS behind a bridge, the minimum
  support is: accept the `BUSDIR` write (`0x12001040`) and the TRXDIR = 1 packet, run the
  transfer in the GS and hand the bytes to the DMA channel 1 destination. This is the costly
  path (full GPU synchronisation); a stub that does nothing would make `FUN_002dd1e8` loop or
  time out (it waits on the DMA and, on failure, returns 0).

### Unverified and how to settle it

1. **Is `FUN_002dd1e8` ever executed?** Static reading cannot say, because the call goes through
   a function pointer. Settle it at run time: in the recompiled build, count calls to
   `0x2DD1E8` and `0x101728` with a diagnostic (`DON_TRACE_...`) during a short run that covers
   boot, the menus and a match; log the raster type and the mode. In PCSX2 (the reference),
   a breakpoint on `0x101728` does the same.
2. **Is the BUSDIR path used by something that is not in the main text** (an IOP module, or
   code in the data islands listed in `config/layout.toml`)? The scan covered only
   `0x100000`-`0x3902D4`. Islands were not scanned.
3. **TRXDIR = 1 built from data.** Not covered by the static search; the run-time count above
   (BUSDIR writes of 1) covers it.
4. **The numbering of the standard functions** (id 0xF = lock) is from memory of the RenderWare
   3.7 headers; the identification of `FUN_002dd1e8` as a raster lock does not depend on it.

## The path of a 3D object and the three defects that broke it (journals 008, 010, 013, 019, 021; added 2026-10-13)

Everything here is **Measured** by the journals named, in the port against the console's own frames
(`tools/verify/image_parity.py`: SSIM and mean absolute difference, MAD; the console's side is the
replay of the reference GS dumps, or its screenshot). The 2D art (logos, portraits, text) was right from
the first run; the 3D scene (the stadium behind the title, the menu and the character select; the
stage and the fighters of the match) was not, and it took three defects in three different stages of
the path to get it right, plus a fourth on the English menu.

### The path

1. The game's own code (recompiled, on the main CPU) builds a display list in RAM: per object a
   matrix, per-object constants (colour and light data) and the geometry. Some of it is vector maths
   done with the MMI instructions and the VU0 macro mode.
2. VIF1 `UNPACK` writes that data into the **VU1 data memory** (the per-object constant block goes to
   address `0x3FB`, the matrix area at the top of the memory is `0x3D00`).
3. The VU1 microprogram transforms it and `XGKICK`s the result to the GS: **path 1** (the console's
   3D geometry is triangle strips with the textures at the top of VRAM, TBP0 `0x2bc000`-`0x314000`;
   the 2D art is triangle fans; journal 013 section 1).
4. Textures and palettes go to the GS through the GIF channel (**path 3**) or VIF1 `DIRECT` (path 2).
5. The GS draws (the PCSX2 GS library, [12](12-building-pcsx2.md)).

(PCSX2's GS dumps label every transfer as index 3, so only the port's own capture, `DON_GS_PACKETS`,
says which path a packet took; [15](15-gs-dumps.md).)

### Defect 1: the VU1 engine (journal 008, T-114 and T-202)

The runtime's own VU1 interpreter stopped at `pc=0x168` with the word `0xfffffff8` (a data word, not an
instruction) about 17 times per frame on the title and the menu (74,854 lines in the menu run, 0 in any
run that ends at the memory-card dialog), ending the microprogram early. The main menu was a mosaic of
magenta and blue triangles: menu SSIM **0.0804 / MAD 95.24** against the console's replay. With
`DON_VU1=microvu` the menu is a menu again: **0.5162 / 52.23**, and the `0x168` lines disappear with both
library engines. That is why microVU is the default today ([13](13-runtime-host.md), "Vector unit and
sound"). The title's background was **not** fixed by it (0.5326 / 40.92 against 0.5344 before).

### Defect 2: `PS2_PEXEW` moved the wrong lanes (journal 013, patch 0037)

Journal 013 went down the path stage by stage and found the first stage where the port differs from the
console: **the game's own vector data, before VIF1**. At the title, the display list in guest RAM is 99.5 %
identical to the console's and the VU1's matrix is byte-identical, but 1,080 sixteen-byte vectors the
console writes are wrong in the port; the two VU1 engines carry them through identically (their data
memories agree to 99.6 %), the microprogram overflows, its output saturates (Z `0xffffdf`, positions at
±2048 px) and the GS clamps it to its guard band: the flat magenta triangles.

The writer is `FUN_0025b8f0` (`0x25b8f0-0x25be44`), which builds the per-object constant block
(`mfc1`, `pexew`, `ppacw`). The runtime's `PS2_PEXEW` swapped the lane **pairs** (0,1) and (2,3); the console's
PEXEW (PCSX2 `MMI.cpp`) swaps lanes 0 and 2 and keeps 1 and 3, so the block came out `a 0 a <stale>` instead of
`a a a a`. **116 sites in 32 files** of the generated code use it. `PS2_PEXEH` had the same defect (0
sites today). Patch 0037 changes one line each; the test `tests/runtime/mmi_lanes_test.cpp` checks the
MMI macros the game emits against a reference written from PCSX2 (26 checks; 6 fail without the patch).
No other `PS2_P*` macro the generated code emits was wrong (1,538 PCPYLD, 202 PPACW, ... in the test's table).
With 0037: title SSIM **0.965 / MAD 3.6** against the console's screenshot (0.959 / 5.3 against the replay;
the console's own screenshot against its replay is 0.98), menu 0.857 / 15.9, character select 0.896 / 9.5.

Journal 010's "match stops on the first GET READY" was not traced to a defect: with 0037 the same route
goes to "FIGHT" at blank 6600 and the fight runs to the end (journal 013, one run); journal 010's own
cause was **not determined** (one of its four runs had passed GET READY).

### Defect 3: the sign branches tested 32 bits (journal 019, patch 0039)

In the match the 3D view flickered between upright and rolled by exactly 90 degrees (the HUD stayed
upright). The camera's look-at (`FUN_0013a850`) computes `1.0 - |at.y|` in the game's soft-float doubles
(`FUN_00130598`) and takes the "looking straight up or down" branch when it is below 0.0001. The
soft-float subtract does `dsubu v0,t2,t3 ; bltz v0` on a 64-bit fraction that is positive but has bit 31
set, and the recompiler emitted `GPR_S32(ctx, rs) < 0`, so the branch was taken wrongly and 1.0 - 0.0007
came out near -7.1. The MIPS `bltz`, `bgez`, `blez` and `bgtz` (and their likely and link forms) compare the
whole 64-bit register: patch 0039 emits `GPR_S64`, **7,538 sites** (2,976 `<`, 2,144 `<=`, 858 `>`, 1,560
`>=`); `tests/runtime/test_branch_sign.py` fails on the unpatched recompiler. On the console the camera was
never rolled (0 of 137 grabs). In the game after the fix the roll detector flags **1 shot of 161** (the
"FIGHT" banner, which scored the same before) against **32 of 161** before (journal 019, follow-up). The same
defect made the **music quiet**: the sound level went from a peak of about 0.02 to 0.53 (about 15 to 25
times), see [05](05-audio-video-input.md), "Sound".

### A fourth, on the English menu: path 3 released the whole queue at once (journal 021, patch 0042)

The English main menu was garbled (the Japanese menu on the same build was right). The English data uploads
66 textures (7 strips of 512x64 in CT32) in one GIF-channel block while path 3 is masked; the game then
runs a VIF1 stream of `lift / mask / FLUSHA / DIRECT <draw>`, so on the console each lifting releases one
packet and the FLUSHA waits for it to end. The runtime handed the GS the whole queue on the first lifting, so
later uploads overwrote earlier textures: textured vertices drawn after an overlapping upload were
**3,405 of 5,608** in the port, **0 of 5,599** on the console. Patch 0042 releases one
EOP-terminated packet per lifting (tags walked: PACKED, REGLIST, IMAGE) and drains the queue at FLUSHA and at
the end of a VIF1 transfer; after it the counter is **0 of 5,608** and the English menu is drawn right
(journal 021, follow-up 3). Nine more textured vertices than the console (5,608 against 5,599) were not
investigated.

### The comparison with the console after patches 0039 and 0042 (journal 013, last section)

| Screen | Against the replay (SSIM / MAD) | Against the screenshot | Earlier figure |
|---|---|---|---|
| title | 0.959 / 5.3 | 0.965 / 3.6 | 0.96 |
| main menu (frame 3060, panels still sliding) | 0.854 / 16.3 | 0.857 / 15.9 | 0.85-0.90 |
| main menu, settled (frame 3300) | 0.882 / 12.6 | | |
| character select | 0.833 / 14.3 | 0.896 / 9.5 | 0.90 |
| practice select | 0.688 / 25.4 | 0.755 / 20.2 | |
| stage list | no console frame | | |
| match | 0.27-0.32 / 53-65 | 0.30 / 61 | another fight |

What is left, by eye (journal 013): at the title the lamps now match (mean colour equal) and the only
thing left is the **crowd**: bottom left lighter in the port (mean 71,74,83 against 56,61,79; SSIM 0.891),
bottom right darker (46,52,72 against 52,67,90; SSIM 0.868), the opposite signs not explained by animation
phase. The menu differences are the panels' slide phase. The match and the practice select are not
comparable by the figure (another moment of an animated scene). In the match the fighters' colours, the
sea, the HUD and the effects look the same. **Not verified:** the stage list against the console, the match
at the same instant, the crowd's difference separated from the animation phase; the judgement is by eye from
single frames, one run per route. The constant `0x3F009A34` appears 1,086 times in the port against 1,087 on
the console ([13](13-runtime-host.md), "The game thread's floating-point mode").

**A correction recorded here:** journal 008 read the word at `0x1717BC` (a heap pointer where the console
has an instruction) as something overwriting the game's float code and left its writer undetermined;
journal 015 found it: the `sceMpeg*` stubs, patch 0036 ([19](19-videos.md) section 3). It was never the
cause of the 3D defects.
