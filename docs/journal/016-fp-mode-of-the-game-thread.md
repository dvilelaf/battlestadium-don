# 016 · The floating-point mode of the game thread (T-206)

- **Date:** 2026-10-13
- **Task:** T-206
- **Status:** resolved (in the host program; see "what is not done")

## Symptom

Journal 013 found that the game's own vector data differs from the console's before VIF1: a constant the
game computes and uploads to the VU1 (0x3F009A34 on the console, 0x3F009A35 in the port, in 1080/544
places) is one unit in the last place up, and the summary of that entry could not say why. The
orchestrator's reading: **the console's units truncate (round toward zero), the host's default rounds to
nearest**, and the port ran the game's code with the host's default. PCSX2's default for the EE and for
both VUs is the same truncation; the port's runtime switched only inside its own VU1 interpreter
(`std::fesetround(FE_TOWARDZERO)` in `ps2_vu1_core.cpp`).

## Fix

New `src/host/fp_env.h/.cpp`: the game thread's mode, applied once when that thread starts and restored at
its end, with `DON_FP_MODE=console|host` (default `console`):

* `console` = the SSE control register 0xFFC0: exceptions masked, round toward zero, flush-to-zero and
  denormals-are-zero on; `host` = leave the thread as it is, so the two can be compared;
* the calls that enter the GS library run with the **host's** value (`fp_env::HostCsrScope`), because in
  the reference emulator that library runs on a thread with the default mode: `gs_glue.cpp`'s
  `gif_transfer`, `on_vsync` (the resize and the vsync call), `reset` and `read_fifo`;
* the vector-unit library switches for itself (`don_vu.h` and `tests/vuglue`), and the sound library does
  integer mixing on the calling thread, as in the reference: both left alone;
* on architectures without that register the functions do nothing and print one `[don]` line.

## Verification

* The unit of the change, measured with a probe compiled with the project's flags
  (`work/orchestrator/t3d/fp_probe.cpp`, `fp_candidates.cpp`): `255.0f * 0.00197001174f` is 0x3F009A34
  with 0xFFC0 and 0x3F009A35 with the host's 0x1FA0 — the pair of journal 013, exactly.
* **With the game** (`DON_VU1=microvu`, the route stopped at the title, `DON_DUMP_RAM`
  `work/orchestrator/t3d/to_title.txt`), counting the two words in the 32 MiB dump:

  | dump | 0x3F009A34 | 0x3F009A35 |
  |---|---|---|
  | console, `work/ref/x/01/ee_ram.bin` | 1087 | 0 |
  | port, `DON_FP_MODE=console` | **544** | **0** |
  | port, `DON_FP_MODE=host` | 0 | 544 |
  | port, before the change (`work/ramdump/t114-title.bin`) | 0 | 544 |

  544 is half of the console's 1087: the game's constant has four lanes and the port writes two of them,
  which is journal 013's other defect (PS2_PEXEW, patch 0037) — the two counts confirm each other.
  The 2D screens are essentially untouched (the title's logo and text band: SSIM 0.9104 against the
  earlier microVU shot; the menu shot 0.9524), while the 3D background changes (the same crops over the
  background: SSIM 0.16-0.58), as expected when positions are computed with the new rounding; the 3D is
  **not** fixed by this alone (the lanes are still wrong until 0037 is built and the code regenerated).
  **The reference md5s of the route's shots change**: they are the other agents' regression values today.
* A guest test in `tests/host` (the synthetic guest, which runs on the game thread): the guest computes
  five operations with the thread's own mode and paints the low byte of each result as a 1x1 sprite at
  y = 440,
    multiply 255.0f * 0.00197001174f -> console 0x3F009A34, host 0x3F009A35
    add      1.0f + 8.9e-8f          -> console 0x3F800000, host 0x3F800001
    divide   1.0f / 3.0f             -> console 0x3EAAAAAA, host 0x3EAAAAAB
    multiply 1e-38f * 0.5f (denormal)-> console 0x00000000, host 0x003671F7
    a*b+c chosen so that fusion changes the low byte (T-206 step 3, -ffp-contract=off): 0x00 in both
  `uv run --no-project --with pillow --with pytest python -I -m pytest tests/host -q` -> **18 passed**,
  including the new case, which reads the pixels and compares the two sets above.
* The GS side, as a sanity check: `build/gs/gsreplay --dump work/gsdump/01-title.gs.zst … --renderer
  software` is **byte-identical** to the stored `work/gsdump/01-title-replay.png` (that tool does not go
  through the game thread at all).

## A defect in my first version, and how it was found

The first `kConsoleCsr` was 0x9FC0 — FTZ, DAZ and the masks, but **bits 13-14 clear, i.e. round to
nearest** — so the mode changed nothing. The probe printed `csr=0x9fe0` and the *same* value for both
modes, which is how it was caught; the constant is 0xFFC0 (`0x1F80 | 3 << 13 | 1 << 15 | 1 << 6`). Worth
remembering: a mode is not applied until something *measures* a value that depends on it.

## The guest-thread question

`DON_FP_MODE` is applied once, on the game thread, and every guest thread runs on that same host thread:
the scheduler keeps a `R5900Context` per guest thread (`runtime/ee_scheduler.h:105-116`) and switches them
as *data*, there is no per-guest-thread floating-point state and nothing else in the runtime touches the
control register except the VU1 interpreter's own scope (`ps2_vu1_core.cpp:1632-1837`). So a guest thread
created later runs with the mode too, and once per host thread is enough.

## What is not done (honest list)

* The task asked for the guest test "on its main thread and on a second guest thread": only the main one is
  tested. The argument above (one host thread, a data switch) is why the second is expected to behave the
  same, but it is an argument, not a measurement.
* The FMA check runs in the guest's own host-side code with the project's flags, not in *generated* code
  (there is no generated code in tests/host); the flags are the same for both (`compile_commands.json`
  shows `-ffp-contract=off -march=x86-64-v3` on the generated code, the runtime, `src/game/` and
  `src/host/`).
* The port still draws the 3D world wrong until patch 0037 is applied and the generated code is rebuilt;
  the two fixes are independent and both were needed.
* Reference shot md5s used by other agents change with this on by default; `DON_FP_MODE=host` restores the
  old arithmetic bit for bit (544 occurrences of the old pattern, the same count as before).
