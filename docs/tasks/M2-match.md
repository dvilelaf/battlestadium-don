# Milestone M2 · Match at full speed

Goal: a 4-player match at stable 60 fps with a correct image. Deliverables E2.1 to
E2.6 of [../PLAN.md](../PLAN.md).

Before any task: read [../../AGENTS.md](../../AGENTS.md).

> The cards in this milestone fix scope and verification. Their internal steps are made concrete when
> M1 closes (see "Level of detail" in [../TASKS.md](../TASKS.md)).

---

### T-201 · Census of the VU1 microprograms the game uses

**Model:** Sonnet · **Depends on:** T-018, T-114 · **Time box:** 4 h

**Read first:** [../knowledge/04-graphics.md](../knowledge/04-graphics.md); `docs/knowledge/data/vu_programs.tsv`

**Output:** `docs/knowledge/data/vu_usage.tsv`, new section in `docs/knowledge/04-graphics.md`

**Steps**

1. From the 6 PCSX2 savestates (`tools/ref/p2s_extract.py vu1-program`): which microprograms
   of the ELF are loaded at each moment.
2. From the port: an optional trace (`DON_TRACE_MPG=1`) that records each microprogram load
   through VIF1 with the destination address, the number of instructions and the ELF address it
   comes from (looking for the bytes in the `[vu_microcode]` range).
3. `vu_usage.tsv`: `elf_addr`, `seen_in` (title, menu, character select, 1v1 match, 4-player match, results).

**Done when** the file exists and the document says how many of the 141 programs have been
seen in use and which ones appear in a match.

**Limits.** The trace is off by default.

---

### T-202 · Loop up to a match with a correct image

**Model:** Orchestrator (with delegated iterations) · **Depends on:** T-114

**Output:** patches, settings, journal, `tests/scripts/match_1v1.txt` (script: from boot to a 1v1 match, with screenshots `start`, `mid`)

**What to expect:** failures of the VU1 interpreter with the RenderWare microcode, long DMA
chains, texture uploads through PATH2, read-backs from the GS, missing functions.
Key tool: compare the GIF stream of the port with the PCSX2 GS dump of the same moment.

**Done when**

```bash
build/linux-release/don --headless --iso "<Japanese ISO>" --shots work/shots/m2 \
    --input-script tests/scripts/match_1v1.txt disc/SLPS_256.75; echo "exit=$?"     # exit=0
uv run --no-project --with pillow --with numpy --with scikit-image python -I tools/verify/image_parity.py \
    work/shots/m2/start.png work/ref/pcsx2/04-match-1v1.png --min-ssim 0.80
```

and the user confirms, by looking at the screenshots, that characters, stage and scoreboards are
present and well formed. (The match is not deterministic: the 0.80 threshold only discards
broken images; human review is what decides.)

---

### T-203 · Per-subsystem performance measurement

**Model:** Sonnet · **Depends on:** T-110 · **Time box:** 5 h

**Output:** `src/host/profile.h/.cpp`, `tools/verify/profile_summary.py`, `tests/test_profile_summary.py`

**Steps**

1. With `DON_PROFILE=1`, the program measures per frame the time in: game code, VIF1 and
   VU1, sending to the GS, presentation, IOP emulator, and the total. Every 600 frames it writes
   a line `[profile] <field>=<ms> ...` with averages and the 99th percentile.
2. `tools/verify/profile_summary.py <log>`: summary table and average, minimum and 1st-percentile fps.
   Exits with 1 if the 1st-percentile fps is lower than `--min-fps` (default 57).

**Done when** the summary tests pass and a 60 s run with `DON_PROFILE=1`
produces at least 5 `[profile]` lines.

**Limits.** With `DON_PROFILE` undefined, zero cost (no clock calls).

---

### Decision gate D5

With the T-203 report on a 4-player match (orchestrator task):

- If 60 fps is already met with the interpreter: skip T-204 and T-205.
- If VU1 dominates the frame time: continue with T-204 and T-205 (microVU).
- If something else dominates (game code, IOP, GS): open specific tasks (see T-209).

---

### T-204 · PCSX2 microVU as a library

**Model:** Sonnet + review · **Depends on:** T-104, gate D5 · **Time box:** 12 h

**Read first:** in Killzone, `ext/kzvu/README.md`, `ext/kzvu/CMakeLists.txt`, `ext/kzvu/include/`; [../DECISIONS.md](../DECISIONS.md) D5 and D11

**Output:** `src/vubridge/don_vu.h`, `src/vubridge/don_vu.cpp`, `src/vubridge/CMakeLists.txt`, `tools/pipeline/build_vu.sh`, `tests/vu/`

**API to implement**

```c
typedef void (*DonVuXgkick)(void* user, const uint8_t* packet, uint32_t bytes);

int      don_vu_version(void);
int      don_vu1_open(DonVuXgkick cb, void* user);
void     don_vu1_close(void);
void     don_vu1_reset(void);
uint8_t* don_vu1_data(void);                                  /* 16 KB of data memory */
void     don_vu1_write_micro(uint32_t addr_bytes, const uint8_t* data, uint32_t bytes);
void     don_vu1_set_regs(const void* vf, const void* vi);    /* optional: initial state */
uint32_t don_vu1_execute(uint32_t start_pc_bytes, uint32_t max_cycles);   /* returns cycles used */
```

**Steps**

1. Build microVU and whatever it needs (x86 emitter, VU operations) in the container, with
   the minimal replacement files. Take into account the restrictions that Killzone
   documents: a single instance per process; VU memory must stay within reach of 32-bit
   offsets; floating point saturation mode; the PS2's own floating point
   control register.
2. A test that does not need the game: a hand-written 4-instruction microprogram (loads
   a vector from data memory, adds it to itself, stores it, ends) and a check of the
   result in data memory.

**Done when**

```bash
bash tools/pipeline/build_vu.sh; echo "exit=$?"                # exit=0
bash tools/container/run.sh build/vu/vu_selftest; echo "exit=$?"   # exit=0
```

**Limits.** x86-64 only. No PCSX2 header crosses the API.

---

### T-205 · Connect the runtime VU1 to microVU

**Model:** Sonnet + review · **Depends on:** T-204, T-202 · **Time box:** 10 h

**Read first:** in Killzone, the patches that touch `ps2_vif1_interpreter.cpp` and the VU hook (`patches/README.md`, patches `0007`, `0010`, `0016`, `0022`); `third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp` and `src/lib/vu/`

**Output:** `patches/ps2recomp/000N-host-vu1-hook.patch`, changes in `src/host/`, `tests/runtime/`

**Steps**

1. A "host VU1" hook in the runtime: loading microcode, launching and continuing a
   microprogram are delegated to an installable function table. VIF1 unpacking stays in the
   runtime.
2. The output of VU1 to the GS (`XGKICK`) reaches the host GS through PATH1.
3. Selection at run time: `DON_VU1=interp` (runtime interpreter) or `DON_VU1=microvu`
   (default when the library is available).
4. Recording for tests: `DON_RECORD_VIF=<folder>` saves the VIF1 chain of one frame.
   A test replays a recorded chain in both modes and compares the GIF bytes emitted.

**Done when**

```bash
DON_VU1=microvu build/linux-release/don --headless --iso "<Japanese ISO>" --shots work/shots/m2v \
    --input-script tests/scripts/match_1v1.txt disc/SLPS_256.75; echo "exit=$?"     # exit=0
```

with the same image checks as T-202 and no regression compared with `DON_VU1=interp`
(same screenshots, reviewed by the user).

**Limits.** The interpreter mode must keep working: it is the reference for debugging.

---

### T-206 · Floating point semantics

**Model:** Sonnet · **Depends on:** T-014 · **Time box:** 3 h

**Read first:** catalog of fixes in [../knowledge/08-external-references.md](../knowledge/08-external-references.md) (floating point rows); in Killzone, patch `0009`

**Output:** changes in `CMakeLists.txt` and `src/host/`, test in `tests/host/`

**Steps**

1. Guarantee in all own targets and in the generated code: `-ffp-contract=off`,
   no `-ffast-math` or equivalents (and their MSVC counterparts: `/fp:precise`).
2. In each thread that runs game code, set the SSE control register to the console's
   semantics: round toward zero (the console's FPU and vector units only truncate; measured
   2026-10-13, journal 013: a value the console stores as `0x3F009A34` comes out as
   `0x3F009A35` in the port, which rounds to nearest), "flush to zero" and "denormals are
   zero" on. Find where the runtime creates those threads and do it there (as a patch) or
   from the host program. Code of host libraries that is called from that thread and expects
   the default mode (the GS library: its replays must stay identical to the reference
   emulator's) gets the default mode for the duration of the call. (Corrected 2026-10-13: this
   step said "round toward zero off", taken from another port; the measurement says otherwise.)
3. Test: a function compiled with the project options computes `a*b+c` with values
   chosen so that an FMA fusion changes the result; check that there is no fusion.

**Done when** the test passes and `grep -R "ffast-math" CMakeLists.txt src/` returns nothing.

**Limits.** Do not change options inside `third_party/` except through a patch.

---

### T-207 · Frame pacing and game speed

**Model:** Sonnet · **Depends on:** T-202, T-203 · **Time box:** 6 h

**Read first:** in Killzone, `docs/findings.md` lines 697–806 (timer and cycle clock)

**Output:** changes in `src/host/`, journal entry, `tests/scripts/clock.txt`

**Steps**

1. Pace the presentation at 59.94 Hz independently of the monitor frequency.
2. Measure the game speed: with a script, enter a match with a time limit and
   capture the time counter at the start and 20 real seconds later; compare with what was
   noted for PCSX2 in `work/ref/pcsx2/NOTES.md`.
3. If the game runs slow or fast even at 60 fps, diagnose (CPU timers,
   vsync counter) and fix.

**Done when** the match clock advances at the same rate as in PCSX2 with an error ≤ 1 %,
documented with the two screenshots.

**Limits.** Do not speed up or slow down with tricks on the game's own counter.

---

### T-208 · Coverage runs

**Model:** Haiku (run and collect) + Sonnet (fix) · **Depends on:** T-202 · **Time box:** 2 h per round

**Output:** `tests/scripts/coverage_*.txt`, new addresses in `config/extra_entry_points.txt`

**Steps per round**

1. (Haiku) Run each script in `tests/scripts/coverage_*.txt` (one per game mode and
   per group of characters and stages) with `--headless`, save the logs and pass them through
   `tools/run/classify_log.py`.
2. (Haiku) Gather all the `MISSING_FUNCTION` addresses into a list without duplicates.
3. (Sonnet) For each address: check with `tools/re/decompile.sh` that it is code, add it
   to `config/extra_entry_points.txt`, regenerate (`tools/pipeline/generate.sh`), rebuild and
   repeat the script that failed.

**Done when** a full round of all the scripts (30 minutes of play in total)
ends with no `MISSING_FUNCTION`.

**Limits.** The scripts must cover the 20 characters and the 11 stages at least once.

---

### T-209 · Performance: extra measures if 60 fps is not reached

**Model:** Orchestrator · **Depends on:** T-205, T-203

Only if E2.4 is not met with microVU. Known options, in order of cost, all with a
precedent in Killzone (`patches/README.md`): a dedicated thread for VIF1 and VU1 (`0016`, `0022`);
a dedicated thread for the IOP (`0015`); batched IOP advance (`0011`, `0012`); improvements to the
generated code (`0017`, `0023`). Each one is opened as its own task with its card when the
profile shows it is needed.

**Done when**

```bash
DON_PROFILE=1 build/linux-release/don --headless --iso "<Japanese ISO>" \
    --input-script tests/scripts/match_4p_10min.txt disc/SLPS_256.75 > work/logs/profile_4.log 2>&1
uv run --no-project python -I tools/verify/profile_summary.py work/logs/profile_4.log --min-fps 57; echo "exit=$?"   # exit=0
```

---

### T-210 · Instruction conformance tests of the recompiler and the runtime

**Model:** Sonnet · **Depends on:** T-014 · **Time box:** 8 h (in two parts)

**Why.** The three defects that kept the game from being drawn and fought on 2026-10-13 were all
single instructions translated wrongly (the runtime's PEXEW macro, journal 013; the recompiler's
sign branches testing 32 bits, journal 019) or a replaced function writing where it should not
(journal 015). Each was found from its symptom, one screen at a time. A test that runs every
instruction the game uses through the port's translation and compares the result with the
reference emulator's definition finds the rest of that family at once.

**Read first:** journals 013 (its table of multimedia instructions and `tests/runtime/mmi_lanes_test.cpp`)
and 019 (`tests/runtime/test_branch_sign.py`: a tiny MIPS program built and recompiled by the test);
the reference definitions in `third_party/pcsx2/pcsx2/` (`R5900OpcodeImpl.cpp`, `MMI.cpp`, `FPU.cpp`,
`COP2.cpp`, `VUops.cpp`).

**Output:** `tests/conformance/` (the generator of the test programs, the reference model, the
runner, a pytest file), `docs/knowledge/data/instruction_conformance.tsv`, a journal entry, patch
files for the defects found (not added to the series by the agent)

**Steps**

1. Census: every instruction form the game's code uses, with its number of sites (the ELF's
   code range, `rabbitizer`).
2. For each form, a tiny guest function that executes it, recompiled by the port's own recompiler
   and compiled against the runtime's headers exactly as the game's code is, run on ordinary and
   edge-case operands (values that are not sign-extended 32-bit numbers, zero, the largest and
   smallest values, unaligned addresses, division by zero, shifts by 0 and by 32 or more, results
   that overflow, denormal and out-of-range floats), and compared with a reference model written
   from the reference emulator's interpreter. Part 1: integer arithmetic and logic, shifts,
   multiplication and division with their result registers, comparisons, every branch with its
   delay slot (likely and link forms), loads and stores including the unaligned ones, the
   128-bit multimedia set. Part 2: the floating-point coprocessor and the VU0 instructions used
   inline.
3. The table: instruction, sites in the game, equal to the reference or not, and for each
   difference the smallest failing case. Each difference gets a patch file with the failing test.

**Done when** `uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/conformance -q`
runs the whole table without the game's files and passes for every instruction except the ones
listed as open defects, each with its patch file.

**Limits.** The tests need no game file. A difference is reported and patched as a file; adding
patches to the series, regenerating and rebuilding are the orchestrator's to schedule.
