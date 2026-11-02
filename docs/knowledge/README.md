# Knowledge base

Everything that is known about *Battle Stadium D.O.N* (hereafter **DON**) and about the tools
to port it, as of **2026-10-13** (journals 007 to 024; journal 025 only by title). It exists so that nobody (person or agent) has to measure
again what has already been measured.

## How to read these documents

Each fact carries one of these three marks, explicit or by context:

- **Measured**: I obtained it by running a tool on the game files on this machine. The command
  to repeat it is given.
- **Declared**: stated by the README or documentation of another project. It has not been run
  or checked here.
- **Unverified**: a reasonable hypothesis that nobody has verified. There is a task to do it.

Addresses are always PS2 memory addresses (hexadecimal, `0x...`) unless stated otherwise.

## Index

| Document | Contents |
|---|---|
| [01-game-and-discs.md](01-game-and-discs.md) | Identity of the game, available disc images, contents and hashes, and running from a variant's folder |
| [02-ps2-executable.md](02-ps2-executable.md) | The `SLPS_256.75` ELF: structure, compiler, instruction census, functions |
| [03-data-formats.md](03-data-formats.md) | AFS and FPK containers (compression solved), file types, RenderWare |
| [04-graphics.md](04-graphics.md) | RenderWare 3.7, VU1 microcode, data path to the GS, and the path of a 3D object with the defects that broke it (journals 008, 010, 013, 019, 021) |
| [05-audio-video-input.md](05-audio-video-input.md) | I/O processor modules, sound (what was measured with the SPU2 library), videos, gamepad and keyboard, the multitap wrappers, memory card |
| [06-gamecube-version.md](06-gamecube-version.md) | What the GameCube version offers as a reference |
| [07-english-patch.md](07-english-patch.md) | What exactly the fan translation changes, and what running the English variants measured |
| [08-external-references.md](08-external-references.md) | Other projects, which pieces can be reused, and a catalog of known fixes |
| [09-environment-and-tools.md](09-environment-and-tools.md) | State of this machine, verified commands, every repo script (verify, run, re, disc, launcher, pipeline), the build-safety rule of journal 018 |
| [10-pcsx2-savestates.md](10-pcsx2-savestates.md) | What a PCSX2 savestate (`.p2s`) contains and the tool that extracts it |
| [11-stub-policy.md](11-stub-policy.md) | Which SDK functions are replaced by runtime handlers and which are recompiled, and why |
| [12-building-pcsx2.md](12-building-pcsx2.md) | How to build pieces of PCSX2 v2.8.2 as libraries: compiler, dependencies, the two integration styles |
| [13-runtime-host.md](13-runtime-host.md) | How the runtime and our host program work: command-line options, `DON_` variables, floating-point mode, pacing, vector unit and sound engines, memory card, and the patch series |
| [14-iop-modules.md](14-iop-modules.md) | The 16 I/O processor modules of the game: what each offers and imports, what the IOP emulator lacks, the CRI audio thread and the memory-card modules |
| [15-gs-dumps.md](15-gs-dumps.md) | The format of a PCSX2 GS dump, how to replay one through the bridge and through PCSX2's own runner, and the results per dump |
| [16-boot-sequence.md](16-boot-sequence.md) | What the game does from its entry point to its frame loop: modules, files, threads, system calls, remote services, and what was found when it ran (section 7) |
| [17-windows-toolchain.md](17-windows-toolchain.md) | Building for Windows from Linux: what builds with each toolchain, what runs under Wine, and the problems found |
| [18-display-registers.md](18-display-registers.md) | The video output registers the game sets on the console (from the reference dumps), what the game's SDK code does to set them, what the runtime's replaced handlers do instead, and the route chosen to make them agree, and the comparison at the title and the menu (journal 008) |
| [19-videos.md](19-videos.md) | How the game plays its videos (Sony's MPEG library with the console's image decompression hardware, driven by the CRI middleware), what the runtime does for them today, the timeline on the console, that Start skips the opening, and the options for the port; the open questions answered by journals 015, 023 and 024 |
| [20-execution-model-and-the-stop.md](20-execution-model-and-the-stop.md) | How the port executes the game's code (generated functions and fragments, the table of resume addresses, checkpoints and the unwinding to the scheduler, interrupts as invocations), the game's frame loop and packet queue with the words to watch, and the stop: its signature, where it was seen, the readings that were wrong, its cause, and how to investigate one |
| [21-instruction-conformance.md](21-instruction-conformance.md) | The instruction conformance suite: what it proves, how to run it, the result by family (331 instructions), the differences this game can see with their patches, the standing decisions, and the wrongly translated instructions another game on this runtime would meet |

Tabular data (generated, can be regenerated):

| File | Contents |
|---|---|
| [data/vu_programs.tsv](data/vu_programs.tsv) | The 141 VU microprograms embedded in the ELF: address, target, size |
| [data/data_afs_ps2.tsv](data/data_afs_ps2.tsv) | The 135 packs of `DATA.AFS`: index, size and folders it contains |
| [data/data_afs_names.tsv](data/data_afs_names.tsv) | Proposed name of each pack of `DATA.AFS` (alphabetical order of the names in the executable; 133 of 135 consistent, not yet confirmed at run time) |
| [data/english_patch_changes.tsv](data/english_patch_changes.tsv) | The 51 files that the English patch changes |
| [data/irx_imports.tsv](data/irx_imports.tsv) | The 709 imports of the I/O processor modules, with whether the emulator supports each |

The decisions made and their reasons are in [../DECISIONS.md](../DECISIONS.md); the plan, in
[../PLAN.md](../PLAN.md) and [../TASKS.md](../TASKS.md).
