# Milestone M3 · Playable

Goal: the game can be completed from start to finish with sound, memory card and 4 players, in
Japanese and in English. Deliverables E3.1 to E3.6 of [../PLAN.md](../PLAN.md).

Before any task: read [../../AGENTS.md](../../AGENTS.md).

> The task cards of this milestone fix scope and verification. Their inner steps are made
> concrete when M2 closes. T-301 can be brought forward: it does not depend on the game.

---

### T-301 · PCSX2 SPU2 as a library

**Model:** Sonnet + review · **Depends on:** T-104 · **Time box:** 8 h

**Read first:** in Fukami, `tools/pcsx2-spu2/README.md`, `PROVENANCE.md`, `rrv_pcsx2_spu2.h` and the `shim/` folder; in Killzone, `ext/kzspu2/`; [../DECISIONS.md](../DECISIONS.md) D6

**Output:** `src/spu2bridge/don_spu2.h`, `src/spu2bridge/don_spu2.cpp`, `src/spu2bridge/shim/`, `tools/pipeline/build_spu2.sh`, `tests/spu2/`

**API to implement** (model: Fukami's)

```c
int      don_spu2_version(void);
int      don_spu2_open(void);
void     don_spu2_close(void);
void     don_spu2_reset(void);
void     don_spu2_write16(uint32_t addr, uint16_t value);     /* addr in 0x1F900000..0x1F9007FF */
uint16_t don_spu2_read16(uint32_t addr);
void     don_spu2_dma_write(int core, const uint8_t* data, uint32_t bytes);
uint32_t don_spu2_dma_read(int core, uint8_t* dst, uint32_t bytes);
void     don_spu2_advance(uint64_t iop_cycles);
int      don_spu2_poll_event(int* type, uint64_t* iop_cycle);  /* 1 = IRQ, 4 = end of DMA core 0, 7 = end of DMA core 1 */
uint32_t don_spu2_pull_output(float* stereo, uint32_t frames); /* 48 kHz, stereo */
```

**Steps.** Build the PCSX2 SPU2 files unmodified, with replacement headers for
what they expect from the rest of the emulator. Test that does not need the game:
load a synthetic sample by DMA, program a voice with register writes, advance
cycles and check that the output is not silence and does not clip.

**Done when**

```bash
bash tools/pipeline/build_spu2.sh; echo "exit=$?"                       # exit=0
bash tools/container/run.sh build/spu2/spu2_selftest; echo "exit=$?"    # exit=0
```

**Limits.** PCSX2 files unmodified; every adjustment goes in the replacements.

---

### T-302 · Connect the IOP emulator to the SPU2

**Model:** Sonnet + review · **Depends on:** T-301, T-101 · **Time box:** 10 h

**Read first:** in Killzone, `patches/ps2recomp/0006-spu2-host-audio.patch` and its description in `patches/README.md`; `docs/knowledge/14-iop-modules.md`

**Output:** `patches/ps2recomp/000N-iop-host-spu2.patch`, `tests/runtime/`

**Steps**

1. "Host SPU2" hook in the IOP emulator. With one installed, the following are routed to it:
   - accesses to registers `0x1F900000`–`0x1F9007FF` (32-bit ones split into two 16-bit ones);
   - DMA starts on channels 4 and 7, with the end signal when the SPU2 finishes;
   - SPU2 interrupt and end-of-DMA requests, converted into the corresponding
     IOP interrupts.
2. The emulator advances the SPU2 on each execution slice.
3. Test that does not need the game: with a fake SPU2 that records accesses, a minimal IOP
   program (or direct writes through the emulator's memory interface) that writes a
   register and starts a DMA; check that the hook receives both.

**Done when** the test passes and `bash tools/setup/fetch_ps2recomp.sh --reset` applies all
the patches from clean.

**Limits.** Without a host SPU2 installed, the behavior does not change.

---

### T-303 · Audio output and capture

**Model:** Sonnet · **Depends on:** T-302, T-110 · **Time box:** 5 h

**Output:** `src/host/audio.cpp`, option `--wav <file>`, `tools/verify/wav_stats.py`, `tests/test_wav_stats.py`

**Steps**

1. The host program opens an SDL3 audio stream (48 kHz, stereo, floating point) and feeds it
   with `don_spu2_pull_output`. With `--headless` it does not open a device.
2. `--wav <file>`: also writes the output to a 16-bit WAV.
3. `tools/verify/wav_stats.py <file> [--max-silence 0.5] [--max-clip 0.001]`: prints
   duration, RMS level, fraction of silent samples (absolute value < 1e-4) and saturated
   fraction (absolute value ≥ 0.999). Exits with 1 if any limit is exceeded. Standard library
   only (`wave`, `struct`). Tests with synthetic WAVs: tone, silence and clipped wave.

**Done when** the tests pass.

**Limits.** None in particular.

---

### T-304 · Loop up to sound in the game

**Model:** Orchestrator (with delegated iterations) · **Depends on:** T-303, T-102

**Read first:** `docs/knowledge/14-iop-modules.md`; fix catalog (IOP rows)

**Output:** patches, journal

**What to expect:** unimplemented IOP core imports; completion latencies;
the sound server (`sdrdrv`) and the sequencer; the ADX player (`cri_adxi`) streaming
`bgm.afs`; `sndfi`.

**Done when**

```bash
build/linux-release/don --headless --iso "<Japanese ISO>" --wav work/audio/menu.wav \
    --input-script tests/scripts/menu_30s.txt disc/SLPS_256.75
uv run --no-project python -I tools/verify/wav_stats.py work/audio/menu.wav --max-silence 0.5; echo "exit=$?"   # exit=0
```

and the same with a match script; and the user confirms by ear that music, voices and effects
sound as in PCSX2.

---

### T-305 · Videos: investigate and decide

**Model:** Sonnet (investigation) + Orchestrator (decision) · **Depends on:** T-113 · **Time box:** 5 h

**Read first:** [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md) (section "Video"); in Killzone, patch `0004` and `ext/kzipu/README.md`

**Output:** `docs/knowledge/16-videos.md` with a recommendation

**Steps.** With `DON_PLAY_MOVIES=1`, trace what the game uses when playing the opening:
`libmpeg` functions (`sceMpeg*`), video hardware registers (`0x10002000`–`0x1000203F`
and `0x10007000`–`0x1000701F`), CRI functions. With that, assess the three options of
document 05 (skip, decode externally with FFmpeg, emulate the video hardware with PCSX2's):
estimated cost, risk and what would have to be written.

**Done when** the document has the summarized trace, the comparison table and a recommendation.

**Follow-up.** The orchestrator decides and then writes a new card, T-305b, with the
implementation (that card does not exist until then).

---

### T-306 · Four players: multitap and several gamepads

**Model:** Sonnet · **Depends on:** T-111, T-114 · **Time box:** 8 h

**Read first:** `docs/knowledge/13-runtime-host.md`; [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md) (section "Gamepad")

**Output:** changes in `src/host/input.*`, possible patch `patches/ps2recomp/000N-pad-multitap.patch`, `tests/host/`, `tests/scripts/charselect_4p.txt`

**Steps**

1. Find out with `tools/re/strxref.py` and `tools/re/decompile.sh` how the game detects the
   multitap (`sceMtap*` functions, string `[libmtap.a = %d.%d, mtapman.irx = %d.%d]`) and which
   ports and slots it queries for players 2, 3 and 4.
2. Make the runtime answer like a console with a multitap on port 1 and four
   gamepads, and make the host program assign up to four SDL gamepads in order of connection.
3. Hot connect and disconnect: a gamepad that leaves marks its slot as disconnected.
4. Test without real gamepads: with scripted input (`P2:`, `P3:`, `P4:`), four cursors
   move independently in the character select.

**Done when** the script `charselect_4p.txt` produces a screenshot in which the four
players have chosen different characters (human review of the screenshot), and the user
confirms it with four real gamepads (or as many as they have).

**Limits.** 4 players only; do not touch the game logic.

---

### T-307 · Memory card

**Model:** Sonnet · **Depends on:** T-114 · **Time box:** 6 h

**Output:** `tests/scripts/save.txt`, `tests/scripts/load.txt`, possible patches, section in the journal

**Steps**

1. Script that creates a save game from a state without a card; check that the
   content appears in the card folder.
2. Second run with another script that loads that save game; screenshot that shows it was loaded.
3. Case without a formatted card and case of a full card, if the game tells them apart.
4. The card folder goes in the user's data folder
   (`$XDG_DATA_HOME/don/` on Linux, `%APPDATA%\don\` on Windows), not next to the ELF.

**Done when** the two consecutive runs end with exit code 0 and the screenshot of the
second shows the data saved in the first.

**Limits.** Do not reuse a PCSX2 card as a shortcut to pass the test.

---

### T-308 · Button mapping, rumble and gamepad settings

**Model:** Sonnet · **Depends on:** T-306 · **Time box:** 5 h

**Output:** changes in `src/host/input.*`, section `[input]` of the settings file (see T-404)

**Steps.** Configurable mapping per player; rumble (`scePadSetActDirect`) to
`SDL_RumbleGamepad`; stick dead zone; simulated pressure-sensitive buttons
(pressed = 255).

**Done when** the translation tests cover a non-standard mapping and the user
confirms the rumble with a real gamepad.

---

### T-320 · Verify the English discs

**Model:** Haiku · **Depends on:** T-304, T-208 · **Time box:** 2 h

**Read first:** [../knowledge/07-english-patch.md](../knowledge/07-english-patch.md)

**Output:** `work/reports/english.md` (summary), screenshots in `work/shots/en-*`

**Steps.** For each English ISO (`en_patch/jpvoice/disc.iso`, `en_patch/envoice/disc.iso`):

1. `tools/disc/verify_disc.py <iso>` must identify the variant.
2. Run `tests/scripts/menu.txt` and `tests/scripts/match_1v1.txt` with `--iso <iso>` and
   `--headless`, saving screenshots and log.
3. Pass `tools/run/classify_log.py` over each log.

**Done when** the four runs end with exit code 0, without `MISSING_FUNCTION`, and the
menu screenshots show English text (human review).

**Limits.** Do not modify the scripts to make them pass: if the English menus require another
sequence of key presses, note it and hand it back.

---

### T-321 · Soak tests per game mode

**Model:** Sonnet (scripts) + Haiku (run) · **Depends on:** T-304, T-306, T-307 · **Time box:** 6 h

**Output:** `tests/scripts/soak_*.txt`, `work/reports/soak.md`

**Steps**

1. Long scripts: main single-player mode as far as a blind script gets; 30 minutes
   of chained 4-player matches; a walk through all the menus and the viewer.
2. Run them with `DON_PROFILE=1` and `--wav`; pass `classify_log.py`, `profile_summary.py` and
   `wav_stats.py`.

**Done when** all end with exit code 0, without `MISSING_FUNCTION`, with 1st percentile fps
≥ 57 and without silent stretches longer than 10 s in a match.

**Human follow-up (E3.6).** The user completes the main mode from start to credits
in Japanese and in English and plays 30 minutes with 4 players.
