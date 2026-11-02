# Phase 0 · Foundations

Goal of the phase: a reproducible environment, a reliable function map, a clean recompilation and
the game reaching its frame loop on Linux. Deliverables E0.1 to E0.8 of
[../PLAN.md](../PLAN.md).

Before any task: read [../../AGENTS.md](../../AGENTS.md). All commands are run from the
repository root. Python is always run as
`uv run --no-project [--with package] python -I ...`.

Constants that are used repeatedly:

| Name | Value |
|---|---|
| Japanese ISO | `$DON_ISO`: outside the repository; the path is set in `local.env` (not versioned; load it with `set -a; . ./local.env; set +a`) |
| Extracted ELF | `disc/SLPS_256.75` |
| ELF SHA-256 | `994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1` |
| PS2Recomp commit | `2c5fbb9389e11dd95693385969490c9e8e6f57b4` |
| Code start | `0x00100000` |
| End of the main code range | about `0x003902D4` (pinned by T-008 in `config/layout.toml`); beyond it the data area holds a few islands of code |

---

### T-001 · Environment check

**Model:** Haiku · **Depends on:** nothing · **Time box:** 30 min

**Output:** `tools/setup/check_env.sh`

**Steps.** Write a Bash script that runs the checks below and prints one line per check
(`OK <name> <version>` or `MISSING <name>: <reason>`):

1. That these exist in `PATH`: `git`, `cmake`, `ninja`, `g++`, `7z`, `uv`, `java`, `xvfb-run`,
   `pkg-config`, `vulkaninfo`, `docker`.
2. `cmake --version` ≥ 3.21.
3. `java -version` with major version ≥ 21.
4. That `g++ -std=c++20 -march=x86-64-v3` compiles a file with `int main(){}`.
5. That `/proc/cpuinfo` contains `avx2`.
6. That there are at least 20 GB free on the file system of the repository.
7. That `docker info` works without `sudo`.
8. Each name separated by spaces in the environment variable `CHECK_ENV_EXTRA` is treated
   as an additional required tool (this lets you test the failure case).

It exits with 0 if everything is present, with 1 if anything is missing. It installs nothing.

**Done when**

```bash
bash tools/setup/check_env.sh; echo "exit=$?"            # ends with: exit=0
CHECK_ENV_EXTRA=nonexistent_tool bash tools/setup/check_env.sh; echo "exit=$?"
# prints a line "MISSING nonexistent_tool..." and ends with: exit=1
```

**Limits.** Do not use `sudo`. Do not install packages.

---

### T-002 · Fetch and patch PS2Recomp

**Model:** Haiku · **Depends on:** nothing · **Time box:** 45 min

**Read first:** [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md), `patches/ps2recomp/series`

**Output:** `tools/setup/fetch_ps2recomp.sh`

**Context.** `third_party/PS2Recomp` already exists on this machine, at the right commit, with
patch `0001` applied by hand and `0002` not applied. The script must leave it correct both in
this state and in a clean clone.

**Steps.** Bash script with this behavior:

1. Variables: `PIN=2c5fbb9389e11dd95693385969490c9e8e6f57b4`, `DIR=third_party/PS2Recomp`,
   `ROOT` = repository root (absolute path).
2. If `DIR` does not exist: `git clone https://github.com/ran-j/PS2Recomp.git "$DIR"` and
   `git -C "$DIR" checkout "$PIN"`.
3. If `--reset` is passed: `git -C "$DIR" checkout -f "$PIN"` and `git -C "$DIR" clean -fd -e out/`
   (this keeps the `out/` build folders).
4. If `git -C "$DIR" rev-parse HEAD` is not `PIN`: print an error that says to use
   `--reset` and exit with 1.
5. For each non-empty line of `patches/ps2recomp/series`, with `P="$ROOT/patches/ps2recomp/<line>"`:
   - if `git -C "$DIR" apply --reverse --check "$P"` succeeds: print `already applied: <line>`;
   - otherwise, if `git -C "$DIR" apply --check "$P"` succeeds: apply it with `git -C "$DIR" apply "$P"` and print `applied: <line>`;
   - otherwise: print `DOES NOT APPLY: <line>` and exit with 1.
6. Exit with 0.

**Done when**

```bash
bash tools/setup/fetch_ps2recomp.sh; echo "exit=$?"      # exit=0
bash tools/setup/fetch_ps2recomp.sh; echo "exit=$?"      # two lines "already applied: ..." and exit=0
git -C third_party/PS2Recomp rev-parse HEAD                # 2c5fbb9389e11dd95693385969490c9e8e6f57b4
git -C third_party/PS2Recomp status --short | sort
#  M ps2xRecomp/include/ps2recomp/code_generator.h
#  M ps2xRecomp/src/lib/fpu_translator.cpp
#  M ps2xTest/src/code_generator_tests.cpp
bash tools/setup/fetch_ps2recomp.sh --reset; echo "exit=$?"   # two lines "applied: ..." and exit=0
```

**Limits.** Do not modify the files in `patches/`. Do not delete `third_party/PS2Recomp/out/`.

---

### T-003 · Build the PS2Recomp tools

**Model:** Haiku · **Depends on:** T-002 · **Time box:** 30 min

**Output:** `tools/pipeline/build_tools.sh`

**Steps.** Bash script that runs the commands below and fails if any of them fails:

```bash
cmake -S third_party/PS2Recomp -B third_party/PS2Recomp/out/build-tools -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build third_party/PS2Recomp/out/build-tools -j "$(nproc)"
```

At the end it prints the paths of the two binaries.

**Done when**

```bash
bash tools/pipeline/build_tools.sh; echo "exit=$?"       # exit=0
ls third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer \
   third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp
mkdir -p work/analyzer
third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer disc/SLPS_256.75 work/analyzer/config.toml \
  | grep -E 'Extracted [0-9]+ functions'
# Extracted 5817 functions
```

**Limits.** If the build fails, do not edit `third_party/`: return the full error.

---

### T-004 · Disc verification and extraction

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** [../knowledge/01-game-and-discs.md](../knowledge/01-game-and-discs.md)

**Output:** `config/discs.toml`, `tools/disc/verify_disc.py`, `tools/disc/extract.sh`, `tests/test_verify_disc.py`

**Steps**

1. Create `config/discs.toml` with exactly this content:

   ```toml
   [elf]
   name = "SLPS_256.75"
   sha256 = "994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1"

   [[variant]]
   id = "jp"
   description = "Japanese (original)"
   data_afs_sha1 = "1c2cdbe5500e5c2328a2ec9f030789156fecc0b5"

   [[variant]]
   id = "en-jpvoice"
   description = "English, Japanese voices (Lord Izen patch, 2023-09-07)"
   data_afs_sha1 = "12f174bccbeadc5c89eeb172317d163365904fd2"

   [[variant]]
   id = "en-envoice"
   description = "English, English voices (Lord Izen patch, 2023-09-07)"
   data_afs_sha1 = "ca29e2ee6d21b9137533d1ddd80547a10133ae0f"
   ```

2. `tools/disc/verify_disc.py <iso>`:
   - Reads `config/discs.toml` with `tomllib`.
   - Gets each file from the ISO with `7z x -so <iso> <name>`, reading standard output
     in blocks (do not load the whole file into memory), and computes the SHA-256 of `SLPS_256.75` and
     the SHA-1 of `DATA.AFS`.
   - If the ISO does not exist, `7z` fails or either of the two files is missing: message and exit 1.
   - If the SHA-256 of the ELF does not match: print both hashes and exit 2.
   - If the ELF matches but `DATA.AFS` is not from any variant: print `VARIANT: unknown` and the hash, exit 2.
   - If everything matches: print `VARIANT: <id>` and exit 0.
   - The decision logic goes in a pure function `classify(elf_sha256, data_sha1, config) -> (code, id)` so it can be tested without an ISO.
3. `tools/disc/extract.sh <iso> [destination=disc]`: calls `verify_disc.py`; if it exits with 0,
   runs `7z x -y -o<destination> <iso> SLPS_256.75 SYSTEM.CNF DIR.DIR MODULES`.
4. `tests/test_verify_disc.py`: tests `classify` with the three variants, with a wrong ELF
   and with an unknown `DATA.AFS`.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_verify_disc.py -q    # all green
uv run --no-project python -I tools/disc/verify_disc.py "$DON_ISO"
# VARIANT: jp
uv run --no-project python -I tools/disc/verify_disc.py en_patch/jpvoice/disc.iso     # VARIANT: en-jpvoice
uv run --no-project python -I tools/disc/verify_disc.py en_patch/envoice/disc.iso     # VARIANT: en-envoice
uv run --no-project python -I tools/disc/verify_disc.py /etc/hostname; echo "exit=$?"   # exit=1
```

**Limits.** Do not copy or move the ISOs. Do not add any game file to the repository.

---

### T-005 · Fetch Ghidra and the PS2 extension

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Output:** `tools/setup/fetch_ghidra.sh`, `tools/setup/ghidra.sha256`, `config/ghidra_language.txt`

**Steps.** Bash script:

1. Downloads into `third_party/ghidra/` if they are not already there:
   - `https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip`
   - `https://github.com/chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.38/ghidra_12.1.4_PUBLIC_20261004_ghidra-emotionengine-reloaded.zip`
2. Computes the SHA-256 of both. If `tools/setup/ghidra.sha256` does not exist, it creates it with these
   values (`sha256sum` format). If it exists and any value differs, error and exit 1.
3. Unzips Ghidra into `third_party/ghidra/` (this gives `third_party/ghidra/ghidra_12.1.4_PUBLIC/`).
4. Unzips the extension into `third_party/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Extensions/`.
5. Looks for the language identifier of the PS2 processor in the `*.ldefs` files of the
   extension (`grep -rho 'id="[^"]*"' .../Extensions/*/data/languages/*.ldefs`) and writes into
   `config/ghidra_language.txt` the one that contains `5900` (a single line, no quotes).
6. Idempotent: a second run does not download or unzip again.

**Done when**

```bash
bash tools/setup/fetch_ghidra.sh; echo "exit=$?"                     # exit=0
test -x third_party/ghidra/ghidra_12.1.4_PUBLIC/support/analyzeHeadless && echo ok
ls third_party/ghidra/ghidra_12.1.4_PUBLIC/Ghidra/Extensions/ | grep -i emotion
cat config/ghidra_language.txt                                         # one line that contains "5900"
```

**Limits.** Do not install outside `third_party/`. If `config/ghidra_language.txt` would end up
empty or with more than one line, stop and return the list of identifiers found.

---

### T-006 · Export the function map with Ghidra

**Model:** Sonnet · **Depends on:** T-005, T-002 · **Time box:** 3 h (the analysis can take 30 min)

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md);
`third_party/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java` (lines 1010–1138: it asks for
two files with `askFile`, first the TOML and then the CSV)

**Output:** `tools/funcmap/ghidra_export.sh`, `config/functions.ghidra.csv`; local: `work/ghidra/don.ghidra.toml`

**Steps**

1. A script that runs Ghidra headless. Starting point:

   ```bash
   G=third_party/ghidra/ghidra_12.1.4_PUBLIC
   mkdir -p work/ghidra
   "$G/support/analyzeHeadless" "$PWD/work/ghidra" don \
     -import disc/SLPS_256.75 -overwrite \
     -processor "$(cat config/ghidra_language.txt)" \
     -scriptPath third_party/PS2Recomp/ps2xRecomp/tools/ghidra \
     -postScript ExportPS2Functions.java \
         "$PWD/work/ghidra/don.ghidra.toml" "$PWD/config/functions.ghidra.csv" \
     2>&1 | tee work/ghidra/analyze.log
   ```

   In headless mode, `askFile` takes its values from the script arguments, in order.
   If that is not the case in this version, use a `.properties` file next to the script or the
   variant that the `analyzeHeadless` documentation indicates; note down what worked.
2. Check the result:
   - the CSV starts with `Name,Start,End,Size`;
   - count the rows and how many have `Start` ≥ `0x3902D4`;
   - read `[ghidra_export] function_count` and `code_label_count` from the TOML.
3. If there are functions with `Start` ≥ `0x3902D4`, list them in the report (they are candidates
   for data taken as code). Do not delete them here; T-008/T-009 decides that.

**Done when**

```bash
bash tools/funcmap/ghidra_export.sh; echo "exit=$?"                  # exit=0
head -1 config/functions.ghidra.csv                                     # Name,Start,End,Size
test "$(wc -l < config/functions.ghidra.csv)" -gt 5000 && echo "rows ok"
grep -A8 '^\[ghidra_export\]' work/ghidra/don.ghidra.toml
```

The report must include: number of functions, number of labels, functions beyond
`0x3902D4` and duration of the analysis.

**Limits.** Do not edit the CSV by hand. Do not modify the PS2Recomp export script; if
it has to be changed, propose it as a patch in the report.

---

### T-007 · Tests for the existing tools

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** `tools/afs_survey.py`, `tools/fpk_survey.py`, `tests/test_fpk.py` (already exists and serves as a model), [../knowledge/03-data-formats.md](../knowledge/03-data-formats.md)

**Output:** `tests/test_afs.py`, `tests/README.md`

**Steps**

1. `tests/test_afs.py`: build in the test, with `struct.pack`, a synthetic AFS with two
   entries (header `AFS\0`, entry table, data aligned to 0x800, name table with
   records of 0x30 bytes) written in `tmp_path`; check that `afs_survey.read_afs(path)`
   returns the two `(offset, size)` pairs and the two names. Add a test that a
   file that does not start with `AFS\0` makes `read_afs` end with `SystemExit`.
2. `tests/README.md`: three lines with the command to run the tests and the rule that no
   test in this folder may need game files.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests -q
# all green; at least 8 tests (6 already existing in test_fpk.py plus the new ones)
```

**Limits.** Do not use files from `disc/`, `en_patch/` or `gc_rvz/` in the tests.

---

### T-008 · Pin the code layout

**Model:** Sonnet · **Depends on:** T-006 · **Time box:** 3 h

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md) (section "Memory map"); `config/functions.ghidra.csv`; `docs/knowledge/data/vu_programs.tsv`; `tools/elf_survey.py`

**Output:** `tools/funcmap/layout.py`, `tests/test_layout.py`, `config/layout.toml`, and corrected sections "Memory map" and "Functions" in `docs/knowledge/02-ps2-executable.md`

**Context.** The first estimate of this document set (code from `0x100000` to about `0x381000`,
then data) was wrong. The Ghidra export of T-006 shows:

- functions continue without interruption up to `FUN_00390260`, which ends at `0x3902D4`; the
  first VU microprogram starts at `0x39030C`;
- there is more MIPS code **inside the data area**, in a gap between VU microprograms:
  nine functions between `0x3BC5D0` and `0x3BD8C8` (plus labels from `0x3BC428`). They load
  addresses of the form `0x8007xxxx` (kernel memory), so they are probably the SDK's kernel
  patch routines, stored as data and copied into kernel memory at boot (the SDK functions
  `InitAlarm`, `InitThread`, `InitTLB` and `kCopy` are in the executable). Unverified;
- a few `entry_*` rows sit even further (`0x3C....`, `0x4B....`, `0x4C....`), probably data
  that Ghidra disassembled by mistake.

So the layout is: one main code range, then data that contains a few islands of code.

**Steps**

1. `tools/funcmap/layout.py --csv config/functions.ghidra.csv --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml`:
   - main text end = the `End` of the last CSV row in the unbroken run of code that starts at
     `0x100000` (two consecutive rows belong to the same run when the gap between them is
     smaller than 0x10000 bytes; the first version of this card said 0x400, which is wrong:
     the main text has holes of valid code that Ghidra did not turn into functions, the
     largest of 0x15A0 bytes at `0x3362F0`);
   - every CSV row whose `Start` is at or beyond that end belongs to an island: merge rows that
     are less than 0x400 bytes apart into one island;
   - for each island compute: start, end, number of function rows and label rows, whether all
     its words decode as valid R5900 instructions (`rabbitizer`), and the set of high halves
     used by its `lui` instructions (to spot `0x8007`);
   - classify each island as `kernel_patch_blob` (valid code that uses `lui` with `0x8000` to
     `0x8007`), `code` (valid code, other), or `false_positive` (not all words are valid
     instructions, or it lies inside a VU microprogram listed in the `--vu` file).
2. Write `config/layout.toml`:

   ```toml
   [elf]
   sha256 = "994e840123eb5a4cd5d13170a52fdca275dabb0a9f4585665b5d0fc67f9180c1"
   load_start = 0x00100000
   file_end   = 0x004C9D80
   bss_end    = 0x005A6800
   entry      = 0x00100008

   [text]
   start = 0x00100000
   end   = 0x00000000   # value measured in this task

   [[island]]           # one table per island, in address order
   start = 0x00000000
   end   = 0x00000000
   kind  = "kernel_patch_blob"   # or "code" or "false_positive"
   functions = 0
   labels = 0
   note = "one line saying why it was classified this way"
   ```

   The VU microprogram ranges are not copied here; tools read them from
   `docs/knowledge/data/vu_programs.tsv`.
3. Inspect the bytes between the main text end and the first VU microprogram (`0x39030C`):
   they must be padding or data, not instructions. Report them.
4. For the islands classified `kernel_patch_blob`: find who references them. Search the main
   text for `lui`/`addiu` pairs that build an address inside the island (the algorithm is in
   `tools/re/strxref.py`, function `find_references`; here the target is an address range, not
   a string) and report the referencing code addresses and which CSV function contains each.
5. Tests of the island grouping and classification with synthetic rows and bytes.
6. Rewrite the sections "Memory map" and "Functions" of `docs/knowledge/02-ps2-executable.md`
   with the measured layout: keep the history honest by saying that the first estimate
   (`0x381000`) was wrong and why (a page-density heuristic). The statement that the heuristic
   analyzer "invents 15 functions inside the data area" must be corrected too: say which of
   those 15 start addresses are real code (in the main text tail or in an island) and which
   are not.

**Done when**

```bash
uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/test_layout.py -q
uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv \
    --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml
uv run --no-project python -I -c "
import tomllib; d = tomllib.load(open('config/layout.toml','rb'))
e = d['text']['end']; assert 0x390000 <= e <= 0x39030C and e % 4 == 0, hex(e)
print('text.end =', hex(e)); print([(hex(i['start']), hex(i['end']), i['kind']) for i in d['island']])"
```

**Limits.** If the main text end falls outside `0x390000`-`0x39030C`, stop and return the
measurements without writing the file. Do not edit the CSV. Deciding whether the islands are
recompiled is not part of this task (T-011 does it, with the orchestrator).

---

### T-009 · Function map validator

**Model:** Sonnet · **Depends on:** T-008 · **Time box:** 3 h

**Read first:** `config/layout.toml`; the first 20 lines of `config/functions.ghidra.csv`

**Output:** `tools/funcmap/validate.py`, `tests/test_funcmap_validate.py`; local: `work/reports/text_gaps.tsv`

**Context.** The CSV exported by PS2Recomp's Ghidra script holds two kinds of rows:

- **functions** (7,111 rows): names such as `FUN_00123456`, `entry` (the program entry) or a
  real name;
- **labels** (about 49,000 rows): names that start with `entry_`, `caseD_` or `default`. They
  are extra entry points, and almost all lie inside a function.

Ghidra reports a function with a non-contiguous body as one address span, so a function row
can contain other function rows (1,775 are nested inside another; one span of 91 KB starting
at `0x3163C8` contains most of them). That is expected, not an error.

**Steps.** `tools/funcmap/validate.py --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 [--extra config/extra_entry_points.txt]`:

1. Loads the CSV (`Name,Start,End,Size`; hexadecimal addresses) and separates functions from labels.
2. Checks, accumulating errors:
   - `E1` no two **functions** overlap partially (one starts inside another and ends after
     it). Nested functions are allowed and only counted;
   - `E2` in every row `Start` and `End` are multiples of 4, `End > Start` and `End - Start == Size`;
   - `E3` every row lies inside `[text.start, text.end)` or inside one island of the layout;
   - `E4` every `jal` instruction that lies inside the range of some CSV row in the main text
     (opcode 3; target `(word & 0x03FFFFFF) << 2`) has a target that is the `Start` of some row
     (function or label) or is listed in `--extra`;
   - `E5` the entry point `0x100008` is the `Start` of a row or lies inside a function.
3. Prints a summary: number of functions and labels, nested functions, labels outside every
   function, bytes of the main text covered by at least one row and the percentage, the
   uncovered gaps inside the main text, and the number of errors per type with the first 10
   of each. For the gaps: those made only of zero words are padding and are just counted;
   for the others print how many there are, their total size, how many decode completely as
   valid R5900 instructions, and the 20 largest (start, end, size, all words valid or not).
   First count by the orchestrator: 3,298 gaps, 322 of them with non-zero words (8,999
   words), the largest of 5,536 bytes at `0x3362F0`.
   With `--gaps-out <file>` the complete list of gaps with non-zero words is also written as
   a TSV (`start`, `end`, `bytes`, `nonzero_words`, `all_valid`).
4. Exits with 0 if there are no errors, with 1 if there are.
5. Tests with a synthetic CSV and ELF (a minimal ELF built with `struct.pack`: header, one
   program header and one segment with a few words) covering a good case and a bad case of
   each error type, and the nested-function case as allowed.

**Done when**

```bash
uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/test_funcmap_validate.py -q   # green
uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv \
    --layout config/layout.toml --elf disc/SLPS_256.75 --gaps-out work/reports/text_gaps.tsv; echo "exit=$?"
```

The report must copy the summary. `exit=0` is not required here: the errors that remain
(`E4` in particular: 13 `jal` targets were not the start of a row in a first count, 11 inside
a row and 2 in no row) are the work of T-010 and T-011. Each uncovered `E4` target must be listed with the address of one `jal` that
calls it.

**Limits.** The validator only reads; it does not fix the CSV.

---

### T-010 · Classify the code pointers

**Model:** Sonnet · **Depends on:** T-008 · **Time box:** 4 h

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md) (sections "Memory map" and "Functions"); `config/layout.toml`; `tools/funcmap/layout.py` and `tools/re/strxref.py` (function `find_references`: the `lui`/`addiu` scan); as a reference of the approach, `tools/scripts/regen.py` and `tools/scripts/find_code_pointers.py` of `drakolordx7/KillzoneRecomp` (clone into `/tmp/opencode/refs/` if it is not there; read only, do not copy code)

**Output:** `tools/funcmap/find_code_pointers.py`, `tests/test_find_code_pointers.py`, `config/extra_entry_points.txt`, `config/code_pointers_reject.txt`; local: `work/reports/code_pointers.json`

**Context.** The recompiled game can only jump through a register (`jr`, `jalr`) to an address
that is a row of the function map or an entry point of the recompiler configuration. Any other
target stops the game at run time with "function not found". So every address of the main text
that the game can hold as a pointer has to be found before the code is generated. A first
count by the orchestrator (2026-10-11, approximate):

| Where the address comes from | Distinct targets | Function start | Label start | Inside a row | In no row |
|---|---|---|---|---|---|
| `data`: words of the data area | 3,239 | 494 | 2,520 | 200 | 25 |
| `code_pair`: `lui` + `addiu`/`ori` in the code | 592 | 403 | 168 | 18 | 3 |
| `jal`: call targets that are not the start of a row | 13 | | | 11 | 2 |

Only 4 targets are found by both `data` and `code_pair`, so both sources are needed. Some
`inside_row` targets of `code_pair` are plain constants and not addresses (`0x00100010`,
`0x00101010`, `0x00180180`); an extra entry point there costs nothing, so this task does not
try to tell them apart.

**Steps**

1. Load the rows of `config/functions.ghidra.csv` whose `Start` is inside
   `[text.start, text.end)` of `config/layout.toml` (rows beyond it belong to the islands and
   are ignored here). A row is a label when its name starts with `entry_`, `caseD_` or
   `default`; otherwise it is a function.
2. Collect the candidates: values that are multiples of 4 inside `[text.start, text.end)`.
   For each one remember its sources and, per source, one address where it was found:
   - `data`: the 4-aligned words of the load segment from `text.end` to the end of the file
     data, skipping the VU microprograms (`docs/knowledge/data/vu_programs.tsv`: each row
     gives `elf_addr` and `bytes`) and the islands of kind `kernel_patch_blob` (their
     instructions would be read as addresses: `sll $v1, $s0, 2` is the word `0x00101880`);
   - `code_pair`: a `lui rt, hi` followed, within the next 4 instructions, by
     `addiu rt, rt, lo` (sign-extended) or `ori rt, rt, lo`, both inside rows of the main text;
   - `jal`: the targets of the `jal` instructions that lie inside rows of the main text.
3. Classify each distinct candidate:
   - `row_start`: equal to the `Start` of a row (function or label);
   - `inside_row`: inside the range of a row but not a `Start`;
   - `new_code`: in no row and, decoding from there with `rabbitizer` (category R5900), every
     instruction is valid until a `jr $ra` (`0x03E00008`) within 4,096 instructions;
   - `rejected`: the rest.
4. Write `config/extra_entry_points.txt` with the `inside_row` and `new_code` candidates (one
   `0x........` address per line, sorted, with a trailing comment
   `# inside_row <row name> <sources>` or `# new_code <sources>`), and
   `config/code_pointers_reject.txt` with the `rejected` ones. Addresses already present in
   `code_pointers_reject.txt` are respected: they are never written to
   `extra_entry_points.txt` (this allows manual corrections).
5. `work/reports/code_pointers.json`: `counts` (class → number of distinct candidates),
   `counts_by_source` (source → class → number), and the lists `inside_row`, `new_code` and
   `rejected`, each entry with `address`, `sources`, `found_at` (one address per source) and,
   for `inside_row`, `row` (the innermost function that contains it).
6. Tests with a synthetic CSV, layout and ELF (built with `struct.pack`) covering the three
   sources and the four classes, and the rule about `code_pointers_reject.txt`.

**Done when**

```bash
uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/test_find_code_pointers.py -q
uv run --no-project --with rabbitizer python -I tools/funcmap/find_code_pointers.py \
    --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 \
    --vu docs/knowledge/data/vu_programs.tsv
uv run --no-project python -I -c "
import json; d = json.load(open('work/reports/code_pointers.json'))
print(d['counts']); print(d['counts_by_source']); assert sum(d['counts'].values()) > 3000"
```

The report must include the counts, and every `new_code` and `rejected` candidate with its
source and where it was found (they are few), plus 10 examples of `inside_row` per source.

**Limits.** Do not modify the Ghidra CSV. If `new_code` exceeds 500, stop and warn: it would
mean that the Ghidra analysis was incomplete and T-006 should be repeated with other options.
Turning the `new_code` addresses into rows of the function map (with their real extent) is
not part of this task.

---

### T-011 · Assemble the recompiler configuration

**Model:** Sonnet · **Depends on:** T-025, T-020, T-021 (its output is generated again after T-026) · **Time box:** 3 h

**Read first:** [../knowledge/08-external-references.md](../knowledge/08-external-references.md) (PS2Recomp section); `config/layout.toml`; `tools/funcmap/validate.py`; `work/analyzer/config.toml` (example of a TOML that the recompiler understands); `config/stub_policy.toml` (from T-020)

**Output:** `tools/pipeline/make_config.py`, `tests/test_make_config.py`; local: `work/don.toml`

**Steps.** `tools/pipeline/make_config.py` generates `work/don.toml` by combining:

1. A filtered function map, `work/functions.filtered.csv`: the rows of
   `config/functions.ghidra.csv` that lie inside `[text.start, text.end)` of
   `config/layout.toml`, plus the rows inside islands whose `kind` is `code`. Rows in islands
   of kind `kernel_patch_blob` or `false_positive` are dropped (the kernel patch blobs are code
   meant to be copied into kernel memory, which the runtime replaces; they are never executed
   at their address in the executable). Print how many rows were dropped per island.
2. Absolute paths: `input` = `disc/SLPS_256.75`, `output` = `generated/`, `ghidra_output` =
   `work/functions.filtered.csv`.
3. `single_file_output = false`, `patch_syscalls = false`, `patch_cop0 = true`, `patch_cache = true`.
4. `stubs`: the `name@0xADDR` entries of `work/analyzer/config.toml` (produced by the
   analyzer with its SDK signature database), filtered by `config/stub_policy.toml`: only
   those that the policy marks as `stub` pass.
5. `entry_points`: those of `work/analyzer/config.toml` plus the addresses of
   `config/extra_entry_points.txt` (with name `entry_<addr>@0x<addr>`), without duplicates.
6. `[patches] instructions`: only those that T-021 decided to keep (file
   `config/instruction_patches.toml`; if it does not exist, an empty list).
7. `[jump_tables]`: the tables of `work/analyzer/config.toml` whose targets all lie in
   `[text.start, text.end)`. The others are dropped and printed (of the 7 tables the analyzer
   finds, the one at `0x3c0000` has its targets in data).
8. `[mmio]`: the entries of `work/analyzer/config.toml` (instruction address → hardware
   register address; the recompiler marks those instructions as hardware accesses) whose
   instruction address lies in `[text.start, text.end)`. There are 788, 4 of them outside.
9. No stub or entry point whose address is outside `[text.start, text.end)` is copied
   (print the ones dropped).
10. The table `[performance]` of the analyzer file is not copied: the recompiler does not read it.

`work/analyzer/config.toml` is the local output of the analyzer
(`third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer disc/SLPS_256.75 work/analyzer/config.toml`).
If it does not exist, `make_config.py` stops with a message that gives that command.

A stub entry `name@0xADDR` makes the recompiler replace the function at that address **and
any function with that name**. The Ghidra map has a few named rows (`AddIntcHandler`,
`AddDmacHandler`, `RFU091`, `RFU116`, `thunk_...`): list in the report the stubs whose name
matches a row at another address.

Tests: with synthetic input files, check the island filtering of the function map, the
filtering by policy, the filtering of jump tables and hardware accesses, the removal of
duplicates and that the resulting TOML can be read with `tomllib`.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_make_config.py -q
uv run --no-project python -I tools/pipeline/make_config.py
uv run --no-project python -I -c "
import tomllib; d = tomllib.load(open('work/don.toml','rb'))['general']
print(len(d['stubs']), 'stubs;', len(d['entry_points']), 'entry_points'); assert d['ghidra_output'].endswith('functions.filtered.csv')"
```

The filtered function map is what the recompiler will use, so it must pass the validator:

```bash
uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv work/functions.filtered.csv \
    --layout config/layout.toml --elf disc/SLPS_256.75 --extra config/extra_entry_points.txt; echo "exit=$?"   # exit=0
```

How the recompiler uses these inputs (read in `ps2xRecomp/src/lib/ps2_recompiler.cpp`,
`discoverAdditionalEntryPoints`): a row named `entry_...` is a slice of the function that
contains it; an address of `entry_points` that lies inside a function becomes such a slice,
and one that lies in no row becomes a function of its own that ends at the next known start.

**Limits.** `work/don.toml` and `work/functions.filtered.csv` are not versioned (local paths, and
the second is derived from versioned files).

---

### T-012 · Generate the code and audit the translation

**Model:** Haiku · **Depends on:** T-011, T-026, T-003 · **Time box:** 1 h

**Output:** `tools/pipeline/generate.sh`, `tools/pipeline/audit.py`, `tests/test_audit.py`; local: `generated/`, `work/reports/translation_audit.json`

**Steps**

1. `tools/pipeline/generate.sh` runs the whole chain, stopping at the first step that fails:
   - if `work/analyzer/config.toml` does not exist, the analyzer:
     `third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer disc/SLPS_256.75 work/analyzer/config.toml > work/analyzer/analyzer.log 2>&1`;
   - `tools/pipeline/make_config.py` (T-011), which writes `work/don.toml`;
   - deletes `generated/` and runs
     `third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp work/don.toml > work/logs/recomp.log 2>&1`;
   - `audit.py`.
2. `tools/pipeline/audit.py --log work/logs/recomp.log --layout config/layout.toml --out generated`
   writes `work/reports/translation_audit.json` with:
   - `functions`: the number from the line `recompiling N functions`;
   - `generated_files`: number of `.cpp` files in `generated/`;
   - `unhandled_total`: lines that contain `unhandled-instruction`;
   - `unhandled_in_text`: of those, the ones with `addr=0x...` inside `[text.start, text.end)`;
   - `functions_in_data`: distinct `function=` names whose error lines have an
     address ≥ `text.end`;
   - `control_flow_warnings`: lines with `[warning] control-flow`.
   - `log_counts`: for every line that starts (after spaces) with `[level] category`, the
     number of lines per `level category` pair (for example `"error unhandled-instruction": 0`,
     `"warning control-flow": 1604`), so that any new kind of message is seen;
   - `recompiler_exit`: the exit code of the recompiler, passed by `generate.sh` with `--exit-code`.

   Format of the log lines:
   `  [error] unhandled-instruction function=sub_00400004 addr=0x4b32e4 - Unhandled SPECIAL instruction: 0x1 raw=0x1`
3. `audit.py` exits with 0 if `unhandled_in_text == 0` and `functions_in_data` is empty; with 1 otherwise.
4. `tests/test_audit.py`: with a synthetic log of 6 lines.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_audit.py -q
bash tools/pipeline/generate.sh; echo "exit=$?"                       # exit=0
cat work/reports/translation_audit.json
# "unhandled_in_text": 0  and  "functions_in_data": []
```

As a reference, the heuristic baseline (without Ghidra) gives `functions = 5817`,
`unhandled_total = 35236`, `unhandled_in_text = 0` and 15 functions in data. Its log is
`work/analyzer/recomp.log` (local), useful to see the real format of the lines.

**Limits.** If `audit.py` exits with 1, do not touch the configuration: return the JSON and the
first 20 error lines.

---

### T-013 · Check the `sqrt.s` fix in the generated code

**Model:** Haiku · **Depends on:** T-012 · **Time box:** 1 h

**Output:** `tools/verify/check_sqrt.py`, `tests/test_check_sqrt.py`

**Context.** `sqrt.s fd, ft` computes the square root of register `ft`. PS2Recomp without patch
`0002` generated the root of `fs` (which in this instruction is always 0). The ELF has 96
`sqrt.s` instructions, 47 of them with `ft` other than 0.

**Steps.** `tools/verify/check_sqrt.py --elf disc/SLPS_256.75 --layout config/layout.toml --generated generated`:

1. Finds in the ELF code the words with `(w >> 26) == 0x11`, `((w >> 21) & 31) == 0x10`
   and `(w & 0x3F) == 4`. For each one: `ft = (w >> 16) & 31`, `fd = (w >> 6) & 31`.
2. In the `.cpp` files of `generated/`, each instruction appears like this:

   ```
       // 0x137794: 0x46000344  ...
       ctx->pc = 0x137794u;
       ctx->f[13] = FPU_SQRT_S(...ctx->f[N]...);
   ```

   For each address from step 1, locate the line `ctx->pc = 0x<addr>u;` and the next
   statement; extract the destination register and the source register `N` (the first
   `ctx->f[N]` inside the parentheses; the argument can be wrapped, as in
   `FPU_SQRT_S(fabsf(ctx->f[0]))`). The same address appears in several files (the function
   and the `entry_...` slices that contain it): check every occurrence.
3. Error if the destination is not `fd` or the source is not `ft` in any occurrence, or if an
   address appears in no file. One exception for a missing address: it lies inside a function
   that is replaced by a runtime handler (the functions declared in
   `generated/ps2_recompiled_stubs.h`, whose names end in `_0x<start address>`; their ranges
   are the rows of `work/functions.filtered.csv`). Those are counted apart as `skipped`.
4. Prints `sqrt.s: <total> checked, <errors> errors` (and `, <n> skipped` when there are any)
   and exits with 0 or 1.
5. Test with a synthetic ELF of two instructions and one correct and one incorrect synthetic `.cpp`.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_check_sqrt.py -q
uv run --no-project python -I tools/verify/check_sqrt.py --elf disc/SLPS_256.75 \
    --layout config/layout.toml --generated generated; echo "exit=$?"
# sqrt.s: 96 checked, 0 errors   (checked + skipped = 96)
# exit=0
```

**Limits.** If there are errors, do not touch anything: the report must say how many and give 5 examples
(it would indicate that patch `0002` is not applied or that the tools were not rebuilt).

---

### T-014 · Build the `don` executable

**Model:** Sonnet + review · **Depends on:** T-012 · **Time box:** 5 h

**Read first:** [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md);
`third_party/PS2Recomp/ps2xRuntime/CMakeLists.txt` lines 425–525 (how `ps2EntryRunner` is assembled);
`third_party/PS2Recomp/ps2xRuntime/include/game_overrides.h`; [../DECISIONS.md](../DECISIONS.md) D7

**Output:** `CMakeLists.txt`, `CMakePresets.json`, `src/game/don_overrides.cpp`, `tools/pipeline/build.sh`

**Steps**

1. `CMakeLists.txt` in the root:
   - adds `third_party/PS2Recomp` as a subdirectory with `PS2X_BUILD_RECOMP`,
     `PS2X_BUILD_ANALYZER`, `PS2X_BUILD_TEST`, `PS2X_BUILD_STUDIO` and `PS2X_ENABLE_FFMPEG` set to `OFF`;
   - defines the executable `don` with: all the `.cpp` files of `generated/`
     (`file(GLOB ... CONFIGURE_DEPENDS)`), `src/game/don_overrides.cpp` and the runtime
     `main.cpp` (`third_party/PS2Recomp/ps2xRuntime/src/main.cpp`), linked with `ps2_runtime`
     the same way `ps2EntryRunner` does it (same debug interface if it is enabled);
   - `generated/` in the include paths of `don`;
   - unity build (`UNITY_BUILD ON`, batches of 32);
   - options for the whole tree: `-march=x86-64-v3 -ffp-contract=off` on GCC/Clang;
   - if `generated/` does not exist or is empty: a clear error at configure time.
2. `CMakePresets.json` with the preset `linux-release` (Ninja generator, `Release`, folder
   `build/linux-release`).
3. `src/game/don_overrides.cpp`:
   - registration: `PS2_REGISTER_GAME_OVERRIDE("Battle Stadium D.O.N", "SLPS_256.75", 0x00100008u, 0x7E09D26Du, applyDon);`
   - in `applyDon`: if the environment variable `DON_ISO` exists, put that path in
     `cdImage` with `PS2Runtime::getIoPaths()` / `PS2Runtime::setIoPaths(...)`;
   - print a line `[don] override applied` to `stderr`;
   - the game's own text output: the function at `0x10C868` is the C library `write` (file
     descriptor in `$a0`, buffer in `$a1`, length in `$a2`), which for descriptors 1 and 2
     sends the text to the debug console of the development kit. Replace it with a host
     function that, when the environment variable `DON_TTY=1` is set, writes that text to
     `stderr` with every line prefixed `[don-tty] `, and in every case returns the length
     (for other descriptors, and when the variable is not set and the descriptor is not 1 or
     2, it must behave as before: find in `game_overrides.h` how an override calls the
     original recompiled function, and if that is not possible say so and leave this step
     out). The decision is recorded as Q-TTYIO in
     [../knowledge/11-stub-policy.md](../knowledge/11-stub-policy.md).
4. `tools/pipeline/build.sh`: `cmake --preset linux-release && cmake --build --preset linux-release`
   and at the end it prints the duration.

**Done when**

```bash
bash tools/pipeline/build.sh; echo "exit=$?"            # exit=0
ls -la build/linux-release/don
build/linux-release/don; echo "exit=$?"                 # message that the ELF path is missing, exit other than 0
```

The report must include the build time, the size of the binary and the compiler warnings
that repeat more than 100 times.

**Limits.** Do not copy generated code into `third_party/`. Do not edit `generated/`. If
something in the runtime has to change, do it as a new patch in `patches/ps2recomp/` (see
"How to create a patch" in `AGENTS.md`).

---

### T-015 · Smoke run and log classification

**Model:** Sonnet · **Depends on:** T-014 · **Time box:** 4 h

**Read first:** [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md) (table of runtime messages)

**Output:** `tools/run/smoke.sh`, `tools/run/classify_log.py`, `tests/test_classify_log.py`, `docs/journal/README.md` updated with the chosen signal

**Steps**

1. Decide **the signal for "the game is in its frame loop"**. Look in the runtime for
   a counter of presented frames or of vsync interrupts that can be reached
   (`ps2_runtime.h`, `EeScheduler.cpp`). If there is one, make `don_overrides.cpp` print it
   every 60 units as `[don] vsync <n>`. If there is not, wrap `sceGsSyncV` (`0x001011D8`)
   from `don_overrides.cpp`: save the current handler with `lookupFunction`, replace it with
   `replaceFunction` by one that counts, prints every 60 and calls the original.
2. `tools/run/smoke.sh [seconds=60]`: runs
   `DON_ISO="<Japanese ISO>" DON_TTY=1 xvfb-run -a timeout <seconds> build/linux-release/don disc/SLPS_256.75`
   with the output in `work/logs/smoke-<date>.log`, and then `classify_log.py` on that log.
   `DON_TTY=1` puts the game's own text output in the log (lines `[don-tty] ...`, see T-014).
   The path of the ISO comes from the environment variable `DON_ISO` if it is already set,
   otherwise from the constants table of this document.
3. `tools/run/classify_log.py <log>` prints a line `RESULT: <category> <detail>`
   and exits with 0 only for `OK_VSYNC_LOOP`. Categories, in order of priority:

   | Category | Condition |
   |---|---|
   | `MISSING_FUNCTION <addr>` | `No exact recompiled function for guest PC 0x<addr>` appears |
   | `UNIMPLEMENTED_SYSCALL <detail>` | `Unimplemented PS2 syscall` or `[Syscall TODO]` appears |
   | `IOP_UNHANDLED <detail>` | `[IOP/RPC trace:unhandled]` appears |
   | `CRASH <signal>` | the log ends with a dump or a signal (`Segmentation fault`, `Aborted`, `terminate called`) |
   | `OK_VSYNC_LOOP <frames> <vsyncs>` | the largest `[don] frame <n>` and the largest `[don] vsync <n>` are both ≥ 300 |
   | `FRAME_LOOP_UNPACED <frames>` | frames reach 300 but vsyncs do not: the loop runs, but no vertical blank reaches the game |
   | `NO_PROGRESS <detail>` | none of the above |

   Changed on 2026-10-11 after T-028: the game's frame loop (`0x131CA0`) does not wait for the
   vertical blank, so one counter is not enough. `[don] frame <n>` counts iterations of the
   loop (calls of `0x138B00`) and `[don] vsync <n>` counts calls of the game's vertical-blank
   handler (`0x138EE0`); both are printed only with `DON_TRACE_FRAMES=1`.

   It also writes next to the log a `.json` with the category and the last 30 lines.
4. Tests of `classify_log.py` with one synthetic log per category.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_classify_log.py -q
bash tools/run/smoke.sh 60; echo "exit=$?"
# prints RESULT: <category> ...   (any category is valid to consider this task done)
```

**Limits.** This task does not fix the boot: it only leaves the tool and the first
classification. The report must include the category obtained and the last 30 lines.

---

### T-016 · Boot loop up to the first frame loop

**Model:** Orchestrator (with delegated iterations) · **Depends on:** T-015, T-013 · **Time box:** that of the phase (2 weeks)

**Read first:** [../TASKS.md](../TASKS.md) (section "Unblocking loops"); catalog of fixes in [../knowledge/08-external-references.md](../knowledge/08-external-references.md); [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md)

**Output:** patches in `patches/ps2recomp/`, adjustments in `src/game/don_overrides.cpp` and `config/`, one entry in `docs/journal/` per blocker

**What to expect, in likely order:** functions missing from the map (add the address to
`config/manual_entry_points.txt`, which `tools/pipeline/make_config.py` merges with the
generated `config/extra_entry_points.txt`, and regenerate; if the address is in no row of the
map, add it to `config/ghidra_seeds.txt` instead and run `tools/funcmap/ghidra_export.sh --force`); unimplemented system calls; IOP reset
with `IOPRP300.IMG`; loading modules from a buffer; opening `data.afs` through the CRI
reader; memory card check; opening the startup video (skip it, see T-113).

**Done when**

```bash
bash tools/run/smoke.sh 60; echo "exit=$?"
# RESULT: OK_VSYNC_LOOP <n>   with n ≥ 300
# exit=0
```

in three consecutive runs (the boot may not be deterministic).

**Limits.** No fix may consist of making a function "return success" without
understanding what it does, except as a temporary diagnostic noted in the journal and removed afterwards.

---

### T-017 · Reference captures with PCSX2, without a person

**Model:** Sonnet · **Depends on:** T-024 · **Time box:** 5 h

**Read first:** `docs/REFERENCE_CAPTURE.md` (the manual procedure this task automates: the two keys, the six moments, the keyboard mapping); `tools/ref/run_pcsx2.sh` (how the reference PCSX2 is configured and started); `tools/ref/name_capture.sh`, `tools/ref/check_captures.sh`

**Output:** `tools/ref/pcsx2_auto.sh` (starts the reference PCSX2 on a virtual display), `tools/ref/drive.py` (sends buttons, waits, grabs the screen, takes a capture), `tools/ref/route.txt` (the route to the six moments), `tests/test_ref_route.py`; local, not versioned: `work/ref/pcsx2/` (the 18 files, `NOTES.md` and the screen grabs used on the way)

**Why.** PCSX2 running the same disc is the reference the port is compared against. Three
files are saved at six moments of the game: a single-frame GS dump (the exact graphics
commands of that frame), the screenshot PCSX2 saves with it, and a savestate (the console
memory at about that moment). This was first planned as fifteen minutes of play by the owner.
The owner asked, rightly, why a person should do it: an agent can drive the emulator, look at
the screen and press the keys. Doing it that way also leaves a route that can be replayed,
which the port needs later for its own scripted input (T-112).

No exact instant has to be hit. The captures are used like this: the dump is replayed through
the port's graphics bridge and through PCSX2's own dump runner, and the two images must be
equal (T-107); the screenshots of the screens that do not move (title, menu, character
select) are compared with the port directly; the savestates are read for data (T-018, T-201).

**Steps**

1. `tools/ref/pcsx2_auto.sh`: starts the reference PCSX2 with the game on a virtual display
   (`Xvfb`), never on the user's display, with its own copy of the PCSX2 data folder so that
   the manual setup of `tools/ref/run_pcsx2.sh` is left as it is. Same settings as the manual
   one (native resolution, no enhancements, no patches, no cheats, captures under
   `work/ref/pcsx2/raw/`), no sound device. A real GPU cannot present to a virtual display, so
   find which renderer works there (the hardware renderer on Mesa's software Vulkan driver,
   the OpenGL renderer on Mesa's software GL, or PCSX2's software renderer) and prefer, in
   that order, the one that runs at a usable speed. Record which one was used and the speed.
2. `tools/ref/drive.py`: commands to press and release a PS2 button for a given time (through
   the keyboard mapping of the reference PCSX2, with X test events), to wait, to grab the
   virtual screen into a PNG under `work/ref/pcsx2/auto/`, to press the capture keys
   (Shift+F8, then F1) and name the result with `tools/ref/name_capture.sh`, and to play a
   route file.
3. Find the route by looking: grab the screen, read the image, decide the next button. The
   six moments:

   | Name | Moment |
   |---|---|
   | `01-title` | Title screen |
   | `02-menu` | Main menu |
   | `03-charselect` | Character select |
   | `04-match-1v1` | A few seconds into a match between two fighters |
   | `05-match-4p` | A few seconds into a match between four fighters (the other three can be played by the console) |
   | `06-results` | Results screen |

4. `tools/ref/route.txt`: the route as text, one step per line, in terms of PS2 buttons and
   not of keys (`press cross 100`, `wait 2.5`, `wait_screen <name> <seconds>`,
   `capture 01-title`, comments with `#`), so that the same file can later drive the port.
   Where a step must wait for the game (loads, videos), wait for what is on the screen, not
   for a fixed time: compare the grab with a stored grab of the expected screen (stored under
   `work/ref/pcsx2/auto/`, since it is an image of the game) and go on when they match.
5. Play the whole route from power-on, unattended, and check the result with
   `tools/ref/check_captures.sh`. Then play it a second time into a different folder to show
   that it can be repeated.
6. `work/ref/pcsx2/NOTES.md`: the renderer used and the speed, how long each load takes, the
   buttons from the title to each moment in words, and anything that looks wrong in the
   emulator.
7. `tests/test_ref_route.py`: the parser of the route file and the mapping from buttons to
   keys, with a synthetic route. No test starts the emulator.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_ref_route.py -q
bash tools/ref/check_captures.sh; echo "exit=$?"
# lists the 18 files (6 x .png, .p2s, .gs.zst) as present, and exit=0
```

and the report shows, for each of the six captures: the size of the three files, that the
screenshot is not blank (number of distinct colours), and a one-line description of what the
agent sees in it; plus the result of the second unattended run.

**Limits.** Nothing may appear on the user's display: virtual display only. Nothing is
installed on the system (`xdotool`, `Xvfb`, `xwd` and ImageMagick's `import` are already
there; Python packages come through `uv run --with`). Images of the game, savestates and
dumps stay under `work/` and `third_party/`, which git ignores; `route.txt` holds button
names and waits only. If no renderer works on a virtual display, or the route cannot be made
repeatable within the time box, stop and report what works and what does not: the manual
procedure of `docs/REFERENCE_CAPTURE.md` remains as the fallback, and whatever part of the
route was found is still delivered.

---

### T-018 · PCSX2 savestate reader

**Model:** Sonnet · **Depends on:** T-017 for the check on real savestates (the tool and its tests do not need them) · **Time box:** 3 h

**Output:** `tools/ref/p2s_extract.py`, `tests/test_p2s_extract.py`, `docs/knowledge/10-pcsx2-savestates.md`

**Steps**

1. A `.p2s` is a ZIP. List its contents and document in `10-pcsx2-savestates.md` which
   files it contains (main CPU memory, memory and microcode of the vector
   units, GS state, IOP memory...) with their sizes.
2. `tools/ref/p2s_extract.py <savestate.p2s> <folder>` extracts those components with fixed names
   (`ee_ram.bin`, `vu1_micro.bin`, `vu1_mem.bin`, `iop_ram.bin`, `gs.bin`).
3. Subcommand `code-pointers --layout config/layout.toml --csv config/functions.ghidra.csv`:
   walks `ee_ram.bin` outside the range of the ELF (heap and stack) and lists the values that
   point into the code and are **not** a function start in the CSV nor appear in
   `config/extra_entry_points.txt`.
4. Subcommand `vu1-program --programs docs/knowledge/data/vu_programs.tsv --elf disc/SLPS_256.75`:
   compares `vu1_micro.bin` with each microprogram in the ELF and says which ones are loaded.
5. Test with a synthetic ZIP.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_p2s_extract.py -q
uv run --no-project python -I tools/ref/p2s_extract.py work/ref/pcsx2/04-match-1v1.p2s work/ref/x/04
ls -la work/ref/x/04/ee_ram.bin        # 33554432 bytes
```

The report must include, for each of the 6 savestates, how many uncovered code pointers
appear and which microprograms are loaded.

**Limits.** The savestates contain game memory: they are not versioned and are not copied outside `work/`.

---

### T-019 · String reference finder

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h 30 min

**Read first:** `tools/layout_survey.py` (it already contains the algorithm; this task turns it into a general tool)

**Output:** `tools/re/strxref.py`, `tests/test_strxref.py`

**Steps.** `tools/re/strxref.py --elf <elf> --text-end 0x3902D4 <regular expression> [--csv config/functions.ghidra.csv]`:

1. Walks the code looking for `lui rt, hi` pairs followed, within the next 4 instructions,
   by `addiu rt, rt, lo` (opcode `0x09`, signed `lo`) or `ori rt, rt, lo`
   (`0x0D`) on the same register. Resulting address: `(hi << 16) + lo`.
2. If the address falls in the data and there is a printable ASCII string of 5 to 200
   characters, zero-terminated, that matches the regular expression, it prints
   `<lui address>  <string address>  <function>  "<string>"`. `<function>` is the
   name from the CSV if `--csv` was passed and the address falls in a function; otherwise `-`.
3. Test with a synthetic ELF that contains a string and a `lui`/`addiu` pair that references it.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_strxref.py -q
uv run --no-project python -I tools/re/strxref.py --elf disc/SLPS_256.75 --text-end 0x3902D4 'modules/.*\.fpk'
# at least two lines: one with "modules/0000.fpk" and another with "modules/1000.fpk"
```

**Limits.** None in particular.

---

### T-020 · SDK function policy: recompile or replace

**Model:** Sonnet · **Depends on:** T-003 · **Time box:** 3 h

**Read first:** [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md); [../DECISIONS.md](../DECISIONS.md) D4, D6 and D9; `third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/` (what each handler does; search for `TODO_NAMED`)

**Output:** `config/stub_policy.toml`, `docs/knowledge/11-stub-policy.md`

**Context.** The analyzer proposes replacing 197 SDK functions with runtime handlers
(the `stubs` list of `work/analyzer/config.toml`). Replacing is correct for anything that
talks to hardware that the runtime models in a different way, and **harmful** for anything that must reach
a real IOP module: if `sceSdRemote` is replaced with an empty handler, sound will never
reach `sdrdrv`.

**Steps**

1. Group the 197 by library (prefix: `sceGs`, `sceDma`, `sceSif`, `scePad`, `sceMc`,
   `sceCd`, `sceSd`, `sceMpeg`, `sceIpu`, `sceDeci2`, `sceTty`, kernel, libc...).
2. For each group, read the runtime handler and decide:
   - `stub`: it is replaced (the handler is implemented and is consistent with decisions D4, D6, D9);
   - `recompile`: the original recompiled code is kept;
   - `review`: it is not clear; explain why.
3. Starting rule, to be confirmed by reading the code: `sceSd*` → `recompile` (D6); `sceSif*` →
   whatever is needed so that remote calls reach the IOP emulator; `sceDeci2*` and
   `sceTty*` → `stub`; any handler that is only `TODO_NAMED` → never `stub` without
   justifying it.
   Also to confirm: the SDK routines that install kernel patches (`InitAlarm`, `InitThread`,
   `InitTLB` and whatever copies the code islands that `config/layout.toml` classifies as
   `kernel_patch_blob`) must be `stub`. They copy code into kernel memory and hook it into the
   console's kernel; the runtime replaces the kernel, so running them recompiled would write
   into memory the runtime does not model.
4. `config/stub_policy.toml`: a `[policy]` table with `name = "stub" | "recompile" | "review"`
   for the 197, and `default = "recompile"`.
5. `11-stub-policy.md`: a table per group with the decision and the reason, citing the file and
   line of the handler.

**Done when**

```bash
uv run --no-project python -I -c "
import tomllib, collections
p = tomllib.load(open('config/stub_policy.toml','rb'))['policy']
print(collections.Counter(p.values())); assert len(p) >= 197"
```

**Limits.** Do not recompile or run the game in this task: it is reading and reasoned
decision. Those marked `review` are resolved by the orchestrator.

---

### T-021 · Review the 7 instructions the analyzer wants to nop

**Model:** Sonnet · **Depends on:** T-003 · **Time box:** 2 h

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md) (end of the section "Functions")

**Output:** `config/instruction_patches.toml`, a new section in `docs/knowledge/02-ps2-executable.md`

**Steps**

1. For each address (`0x11a31c`, `0x11a808`, `0x11b264`, `0x11b310`, `0x11b608`,
   `0x11bde0`, `0x11dc34`): disassemble 16 instructions around it (with `rabbitizer`,
   R5900 category), identify which SDK function it falls in (names in
   `work/analyzer/config.toml`) and explain what the instruction does and why the analyzer
   marks it as "possible self-modifying code".
2. Decide for each one: keep the nop or not.
3. Write `config/instruction_patches.toml` with an `instructions` list in the format of the
   recompiler (`{ address = "0x...", value = "0x0" }`) with only the ones that are kept, and a
   comment per entry.

**Done when** the document has a table with the 7 addresses, function, instruction,
decision and reason, and:

```bash
uv run --no-project python -I -c "
import tomllib; print(len(tomllib.load(open('config/instruction_patches.toml','rb'))['instructions']), 'patches kept')"
```

**Limits.** If a decision cannot be justified, mark it as "doubt" in the table and do **not**
include it in the file.

---

### T-022 · Map pack names to `DATA.AFS` indices

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h 30 min

**Read first:** [../knowledge/03-data-formats.md](../knowledge/03-data-formats.md); `docs/knowledge/data/data_afs_ps2.tsv`

**Output:** `tools/re/pack_names.py`, `docs/knowledge/data/data_afs_names.tsv`

**Steps**

1. Extract from the ELF, in the order in which they appear, all the strings that match
   `^fpack/[a-z0-9_/%]+\.fpk$` (there are 150, three of them templates with `%d`).
2. For each `DATA.AFS` index (0 to 134), get the folders of its inner files
   (they are already in `data_afs_ps2.tsv`).
3. Propose the mapping: test the hypothesis "index N corresponds to the N-th
   path in alphabetical order" and the hypothesis "N-th in order of appearance in the ELF".
   An assignment is consistent if the folder of the path (`fpack/chr/gok/1000.fpk` → `chr/gok`)
   appears among the inner folders of the pack.
4. Write `data_afs_names.tsv` with columns `afs_index`, `proposed_name`, `consistent`
   (yes/no) for the hypothesis with the most consistent entries, and state in the first comment line
   which one it is and its consistency percentage.

**Done when**

```bash
uv run --no-project python -I tools/re/pack_names.py
head -3 docs/knowledge/data/data_afs_names.tsv
wc -l docs/knowledge/data/data_afs_names.tsv        # 137 lines (comment + header + 135)
```

**Limits.** If neither hypothesis exceeds 80 % consistency, leave the file with the best one
and say so in the report; do not invent a third one without data. The real mapping will be
confirmed at run time (which index the game opens for each name).

---

### T-023 · Decompile a function by address

**Model:** Sonnet · **Depends on:** T-006 · **Time box:** 2 h

**Output:** `tools/re/ghidra/DecompileAt.java`, `tools/re/decompile.sh`, `tools/re/callers.py`, `tests/test_callers.py`

**Context.** Many later tasks need to read what a specific function does. This
task leaves two tools so that any agent can do it without opening Ghidra by hand.

**Steps**

1. `tools/re/ghidra/DecompileAt.java`: a Ghidra script that receives addresses as arguments
   and, for each one, prints between the markers `=== BEGIN 0x<addr> <name> ===` and
   `=== END 0x<addr> ===` the decompiled C of the function that contains that address (interface
   `DecompInterface`, limit of 60 s per function) and, after that, its disassembly.
2. `tools/re/decompile.sh 0xADDR [0xADDR...]`: runs `analyzeHeadless` on the project that is already
   analyzed (`work/ghidra`, project `don`, with `-process SLPS_256.75 -noanalysis -readOnly`)
   with that script, and shows only the text between the markers.
3. `tools/re/callers.py --elf disc/SLPS_256.75 --layout config/layout.toml --target 0xADDR [--csv config/functions.ghidra.csv]`:
   lists the addresses of all the `jal` instructions whose target is `--target` and, if the
   CSV is given, the function that contains each one. Test with a synthetic ELF.

**Done when**

```bash
uv run --no-project --with pytest python -I -m pytest tests/test_callers.py -q
bash tools/re/decompile.sh 0x001011D8 | head -40
# starts with "=== BEGIN 0x001011d8" (or equivalent) and contains C code
uv run --no-project python -I tools/re/callers.py --elf disc/SLPS_256.75 --layout config/layout.toml --target 0x001011D8
# prints at least one line, or "0 calls" if nobody calls that function directly
```

**Limits.** The decompiled C is code derived from the game: it can be quoted in the journal
in short fragments to explain a diagnosis, but whole functions are not dumped into the
repository.

---

### T-024 · Portable PCSX2 for reference captures

**Model:** Sonnet · **Depends on:** nothing · **Time box:** 3 h

**Read first:** [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md); PCSX2 sources at tag `v2.8.2` for the default hotkeys and the portable mode (`EmuFolders::ShouldUsePortableMode` in `pcsx2/Pcsx2Config.cpp`: a `portable.ini` file next to the program, or the `-portable` argument)

**Output:** `tools/setup/fetch_pcsx2_ref.sh`, `tools/ref/run_pcsx2.sh`, `tools/ref/name_capture.sh`, `tools/ref/check_captures.sh`, `docs/REFERENCE_CAPTURE.md`

**Steps**

1. `tools/setup/fetch_pcsx2_ref.sh`: downloads
   `https://github.com/PCSX2/pcsx2/releases/download/v2.8.2/pcsx2-v2.8.2-linux-appimage-x64-Qt.AppImage`
   into `third_party/pcsx2-ref/`, records its SHA-256 in `tools/setup/pcsx2_ref.sha256` on the
   first download and checks it afterwards, and makes it executable.
2. Portable configuration inside `third_party/pcsx2-ref/` (nothing is written to the user's
   home): hardware renderer (Vulkan), internal resolution 1x, no widescreen or other patches,
   no cheats; screenshots, savestates and GS dumps all saved under `work/ref/pcsx2/raw/`.
3. BIOS: the script takes the folder that contains the user's PS2 BIOS from the environment
   variable `DON_PS2_BIOS_DIR` and points PCSX2 at it. It never copies the BIOS into the
   repository tree. If the variable is not set, it prints what is needed and exits with 1.
   On this machine the folder is the one in `DON_PS2_BIOS_DIR` of `local.env` (two Japanese BIOS files,
   `ps2-0210j-20040917.bin` and `ps2-0200j-20040614.bin`); select the first one in the
   PCSX2 configuration.
4. `tools/ref/run_pcsx2.sh [iso]`: starts that PCSX2 with the ISO (default: the Japanese ISO).
5. `docs/REFERENCE_CAPTURE.md`: the exact keys for screenshot, savestate and single-frame GS
   dump, read from the PCSX2 sources at this version (not from memory), and the six moments.
6. `tools/ref/name_capture.sh <name>`: moves the newest `.png`, `.p2s` and `.gs.zst` from
   `work/ref/pcsx2/raw/` to `work/ref/pcsx2/<name>.<ext>`; fails if any of the three is missing.
7. `tools/ref/check_captures.sh`: lists which of the 18 expected files exist; exit 0 only if
   all are present.

**Done when**

```bash
bash tools/setup/fetch_pcsx2_ref.sh; echo "exit=$?"                 # exit=0
DON_PS2_BIOS_DIR= bash tools/ref/run_pcsx2.sh; echo "exit=$?"       # explains the BIOS variable, exit=1
bash tools/ref/check_captures.sh; echo "exit=$?"                    # lists 18 missing files, exit=1
```

and, with `DON_PS2_BIOS_DIR` set by the user, PCSX2 opens and boots the game (the user confirms).

**Limits.** Do not download or link a BIOS. Do not write outside `third_party/pcsx2-ref/` and
`work/ref/pcsx2/`. The savestates and GS dumps contain game memory: they stay in `work/`.

---

### T-025 · Export contiguous function ranges from Ghidra

**Model:** Sonnet · **Depends on:** T-006, T-009, T-010 · **Time box:** 3 h

**Read first:** `AGENTS.md` ("How to create a patch"); `third_party/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java` lines 880–1090 (how label rows and function rows are built); `tools/funcmap/ghidra_export.sh`; [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md) (section "Functions")

**Output:** `patches/ps2recomp/0003-ghidra-export-contiguous-function-ranges.patch` and its line in `patches/ps2recomp/series`; regenerated `config/functions.ghidra.csv`, `config/layout.toml`, `config/extra_entry_points.txt` and `config/code_pointers_reject.txt`; updated counts in the section "Functions" of `docs/knowledge/02-ps2-executable.md`

**Context.** In Ghidra the body of a function can be made of several separate address ranges
(for example a small routine that jumps to a shared return placed far away). PS2Recomp's export
script writes one row per function with `End` = the highest address of the body and `Size` =
the number of bytes of the body (`ExportPS2Functions.java`, near line 1049). For a function in
several pieces that span is not the function. Measured on 2026-10-11 by T-009 and the
orchestrator:

- 12 functions whose body is 32 to 164 bytes have spans of 19 to 118 KB:
  `FUN_003163c8` (91 KB), `FUN_00349b38` (19 KB), `FUN_00350370` (82 KB) and nine between
  `FUN_0034b400` and `FUN_0034c3f0` (111 to 118 KB each);
- the spans of the 7,102 functions of the main text add up to 3.71 MB, for a text of 2.69 MB;
- 1,775 functions lie inside the span of another, 32 pairs overlap partially, and in 1,114
  rows `Size` is not `End - Start`.

The recompiler takes `[Start, End)` as the function. With these rows it would translate about
1.2 MB of code twice and produce a dozen functions of some 28,000 instructions each.

**Steps**

1. In `ExportPS2Functions.java`, for each function (let `N` be the lowest entry point of
   another function that is higher than this function's entry point):
   - the function row goes from the entry point to the end of the last range of the body
     that starts before `N` (`End` exclusive, `Size` = `End - Start`). So the small holes
     inside a function stay inside its row, and the row stops before the next function;
   - every range of the body that starts at or after `N`, and every range that lies
     completely before the entry point, becomes a label row named `entry_<8 hex digits>` (the
     format of `makeAnonymousEntryName`), unless a row already starts at that address. Add
     them through the same path as the other label rows (`collectExecutableLabelRecords`), so
     that no two rows share a `Start`;
   - the end of a label row is computed by the existing code (the row of the function whose
     span contains it, then the next row start). One refinement is allowed: a label that is
     in no function row but lies in the body of a function ends where that range of the body
     ends, if that is before the next row start.

   **Correction (2026-10-11).** The first version of this card said "the function row covers
   the range of the body that contains the entry point". That was wrong: 1,067 functions
   have small holes in their body (a few words that Ghidra's flow does not reach), and the
   rule cut each of them at its first hole (`FUN_00190580` went from 22 KB to 556 bytes), so
   the rest of the function was left to label rows that no function row contained (labels
   outside every function went from 2,826 to 15,478).
   - add two counters to the `[ghidra_export]` table of the TOML the script writes:
     `split_function_count` (functions with more than one range) and `body_range_label_count`
     (label rows added by this change).
   Change nothing else in the script.
2. Create the patch as `AGENTS.md` says (comment header: what it fixes and how it was
   verified; it is original work for this project, written against PS2Recomp commit
   `2c5fbb9389e11dd95693385969490c9e8e6f57b4`), add it to `series`, and check that the three
   patches apply from clean with `bash tools/setup/fetch_ps2recomp.sh --reset`.
3. Regenerate, in this order: `bash tools/funcmap/ghidra_export.sh` (it reuses the analyzed
   project, a few seconds); `tools/funcmap/layout.py`; `tools/funcmap/find_code_pointers.py`;
   `tools/funcmap/validate.py` with `--extra config/extra_entry_points.txt`. The exact
   commands are in "Done when".
4. Update the "Functions" section of `docs/knowledge/02-ps2-executable.md`: the row counts of
   the Ghidra export, and a short paragraph that explains the problem and the patch.

**Done when**

```bash
bash tools/setup/fetch_ps2recomp.sh --reset; echo "exit=$?"     # three "applied:" lines, exit=0
bash tools/funcmap/ghidra_export.sh; echo "exit=$?"             # exit=0
uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv \
    --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml
uv run --no-project --with rabbitizer python -I tools/funcmap/find_code_pointers.py \
    --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 \
    --vu docs/knowledge/data/vu_programs.tsv
uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv \
    --layout config/layout.toml --elf disc/SLPS_256.75 --extra config/extra_entry_points.txt; echo "exit=$?"
uv run --no-project python -I -c "
import csv
lab = lambda n: n.startswith(('entry_', 'caseD_', 'default'))
f = [(int(r['Start'], 16), int(r['End'], 16)) for r in csv.DictReader(open('config/functions.ghidra.csv')) if not lab(r['Name']) and int(r['Start'], 16) < 0x3902D4]
s = sum(e - a for a, e in f); print(len(f), 'functions, spans add up to', s, ', largest', max(e - a for a, e in f)); assert s < 2_700_000"
uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/test_layout.py tests/test_funcmap_validate.py tests/test_find_code_pointers.py -q
```

Expected: `text.end = 0x003902D4` and the same three islands as before; in the validator
`E1=0`, `E5=0`, `E2=0` or a handful that the report explains one by one, and `E3` only for the
label `entry_003c12a0`; no nested functions; labels outside every function close to the
2,826 of before the patch; the sum of the function spans close to the sum of the body sizes
(2.46 MB). The main text coverage can be a little lower than the 97.89 % of before, because
the bytes that only the giant spans covered are no longer claimed by any row: report the
figure and what the newly uncovered code is (T-026 deals with it). `E4` can be above 0 for
the same reason. The validator may exit with 1 because of the `E3` row and of `E4`.

**Limits.** Do not edit the CSV by hand and do not change the three tools: if one of them
needs a change, say which and why. If the main text end moves or an island changes, stop and
report the measurements. Another agent may be using the Ghidra
project through `tools/re/decompile.sh`: if `analyzeHeadless` reports that the project is
locked, wait 15 seconds and try again (up to 10 times).

---

### T-026 · Make Ghidra claim the code that no function owns

**Model:** Sonnet · **Depends on:** T-025 · **Time box:** 4 h

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md) (section "Functions"); `tools/funcmap/ghidra_export.sh`; `tools/re/ghidra/DecompileAt.java` (a script of this project that takes arguments in headless mode); `config/extra_entry_points.txt`

**Output:** `tools/funcmap/ghidra/ClaimOrphanCode.java`; changes in `tools/funcmap/ghidra_export.sh`; regenerated `config/functions.ghidra.csv`, `config/layout.toml`, `config/extra_entry_points.txt` and `config/code_pointers_reject.txt`; updated section "Functions" of `docs/knowledge/02-ps2-executable.md`

**Context.** Measured after T-025:

- 463 stretches of the main text hold code that is in no row of the function map: 50,136
  bytes, and every word decodes as a valid R5900 instruction. In the Ghidra project 461 of
  them are undefined bytes (Ghidra never disassembled them, because nothing it followed
  leads there) and 2 are partly disassembled (`0x1086D0`, `0x355744`).
- It is not all dead code. 39 addresses inside those stretches are referenced by pointers
  (the `new_code` lines of `config/extra_entry_points.txt`). And that code calls more code
  in other stretches: 13 call targets were found only by accident (`0x31BF20`, `0x31E210`,
  `0x31EA48`, `0x31FA90`, `0x31FF88`, `0x326548`, `0x326C48`, `0x326C58`, `0x326C78`,
  `0x328120`, `0x32DA38`, `0x32DA78`, `0x34E840`), and no scan of the rows of the map can see
  them now, because their callers are not in any row.
- The recompiled game can only jump to addresses the function map knows. So the map has to
  cover all the code, and Ghidra has to be the one that analyses the missing part, so that
  the calls, jump tables and pointers of that code are followed too.

**Steps**

1. `tools/funcmap/ghidra/ClaimOrphanCode.java <text start> <text end> [seeds file]`
   (a Ghidra script; it changes the program and must be saved):
   - seeds first: for each address of the seeds file (lines `0x........`, the rest of the
     line is a comment; only the lines whose comment starts with `new_code` are used): if
     there is no instruction there, disassemble from it; if no function contains it, create
     a function at it;
   - sweep: walk `[text start, text end)` in address order. At each address that is a
     multiple of 4, has no instruction and no defined data, and whose word is not zero:
     disassemble from it, and if no function contains it afterwards, create a function at
     it. Zero words are padding (or `nop`) and never start a function;
   - after the sweep call `analyzeChanges(currentProgram)` so that Ghidra analyses the new
     code, and repeat the sweep until a round creates nothing (at most 5 rounds);
   - print the counters: seeds used, instructions disassembled, functions created, rounds,
     and the number of non-zero words of the range that still have no instruction;
   - touch nothing outside `[text start, text end)`. A second run on the same project must
     create nothing.
2. `tools/funcmap/ghidra_export.sh`: run that script after the analysis and before the
   export, with the text range taken from `[text]` of `config/layout.toml` (defaults
   `0x00100000` and `0x003902D4` if the file does not exist) and
   `config/extra_entry_points.txt` as seeds file if it exists. The project is saved (this
   step is not read-only). A marker file `work/ghidra/.orphans-claimed` makes it run only
   once on an analyzed project; `--force` redoes everything from the import.
3. Before the first run on the existing project, keep a copy of it
   (`cp -a work/ghidra work/ghidra.before-t026`), so that the previous state can be restored.
4. Regenerate, in this order: `bash tools/funcmap/ghidra_export.sh`; `layout.py`;
   `find_code_pointers.py`; `validate.py` with `--extra`. If `find_code_pointers.py` still
   reports `new_code` addresses, run `ghidra_export.sh` once more with the new seeds only if
   that needs no change in its logic; otherwise report them.
5. Update the "Functions" section of `docs/knowledge/02-ps2-executable.md` with the new
   counts and a paragraph on what this step does and why.

**Done when**

```bash
bash tools/funcmap/ghidra_export.sh; echo "exit=$?"             # exit=0
uv run --no-project --with rabbitizer python -I tools/funcmap/layout.py --csv config/functions.ghidra.csv \
    --elf disc/SLPS_256.75 --vu docs/knowledge/data/vu_programs.tsv --out config/layout.toml
uv run --no-project --with rabbitizer python -I tools/funcmap/find_code_pointers.py \
    --csv config/functions.ghidra.csv --layout config/layout.toml --elf disc/SLPS_256.75 \
    --vu docs/knowledge/data/vu_programs.tsv
uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv \
    --layout config/layout.toml --elf disc/SLPS_256.75 --extra config/extra_entry_points.txt; echo "exit=$?"
uv run --no-project python -I -c "
import csv
starts = {int(r['Start'], 16) for r in csv.DictReader(open('config/functions.ghidra.csv'))}
t = [0x31BF20, 0x31E210, 0x31EA48, 0x31FA90, 0x31FF88, 0x326548, 0x326C48, 0x326C58, 0x326C78, 0x328120, 0x32DA38, 0x32DA78, 0x34E840]
missing = [hex(a) for a in t if a not in starts]; print('call targets that are not rows:', missing); assert not missing"
bash tools/funcmap/ghidra_export.sh; echo "exit=$?"             # second run: creates nothing, same CSV
```

Expected: `text.end = 0x003902D4` and the same three islands; `new_code` 0; in the validator
no gaps with non-zero words (or a handful, each explained in the report), `E1=0`, `E2=0`,
`E4=0`, `E5=0` and `E3` only for `entry_003c12a0`; the CSV has more rows than before (56,246)
and none of the old function rows has disappeared.

**Limits.** Do not change the three Python tools or the export script of PS2Recomp. If
after 5 rounds more than 1,024 bytes of non-zero words remain without instructions, or the
main text end or an island changes, or old function rows disappear, stop, restore the copy
of the project and report the measurements. Other agents may be using the Ghidra project
through `tools/re/decompile.sh`: if `analyzeHeadless` reports that the project is locked,
wait 15 seconds and try again (up to 10 times).

---

### T-027 · Refresh the environment and tools document

**Model:** Haiku · **Depends on:** nothing · **Time box:** 1 h

**Read first:** [../knowledge/09-environment-and-tools.md](../knowledge/09-environment-and-tools.md); [../STATUS.md](../STATUS.md); the header comment of every script under `tools/`

**Output:** `docs/knowledge/09-environment-and-tools.md` (updated)

**Context.** That document was written before any task ran. Since then Ghidra, the PCSX2
sources, the reference PCSX2 and the build container were installed under `third_party/`,
and about thirty scripts were added to `tools/`. Parts of the document now say the opposite
of what is true ("Ghidra: not installed", a list of versions "still to be installed").

**Steps**

1. Section "This machine": correct the rows that changed. Each new fact is checked with a
   command (for example `ls third_party`, `docker images don-build`,
   `cat config/ghidra_language.txt`) and marked **Measured**.
2. Section "Repo scripts (`tools/`)": one row per script that exists now
   (`find tools -type f \( -name '*.py' -o -name '*.sh' -o -name '*.java' \) | sort`), grouped
   by folder, with what it does in one line (taken from its header comment, not invented),
   the command to run it and the task that created it (from `docs/STATUS.md` and the task
   cards). Scripts under `tools/container/` that only run inside the image are marked so.
3. A new section "The pipeline, in order": the commands that take a clean checkout to a
   built `don` executable and a built `libdon_gs.so`, in order, one line each, with the
   outputs each one leaves (take the order from the task cards T-001 to T-014, T-025, T-026
   and T-103 to T-105; do not run them).
4. Section "Pinned versions for what is still to be installed": rename it to "Pinned
   versions" and say, for each item, which script fetches it and where it ends up.
5. Do not remove findings: a statement that is no longer true is corrected in place with a
   short note of what changed and when (2026-10-11).

**Done when**

```bash
grep -c 'Not\*\* installed' docs/knowledge/09-environment-and-tools.md      # 0
for f in $(find tools -type f \( -name '*.py' -o -name '*.sh' -o -name '*.java' \) | sort); do
  grep -q "$(basename "$f")" docs/knowledge/09-environment-and-tools.md || echo "not listed: $f"; done   # prints nothing
```

**Limits.** Documentation only: no script is changed or run, except read-only commands that
check a fact (`ls`, `cat`, `docker images`, `--help`). Nothing is installed.

---

### T-028 · Static map of the boot sequence

**Model:** Sonnet · **Depends on:** T-023, T-020, T-101 · **Time box:** 4 h

**Read first:** [../knowledge/02-ps2-executable.md](../knowledge/02-ps2-executable.md); [../knowledge/05-audio-video-input.md](../knowledge/05-audio-video-input.md); [../knowledge/11-stub-policy.md](../knowledge/11-stub-policy.md) (sections "Kernel patches" and "Review"); [../knowledge/14-iop-modules.md](../knowledge/14-iop-modules.md); `config/stub_policy.toml`

**Output:** `docs/knowledge/16-boot-sequence.md`

**Why.** The next step of the plan is to run the recompiled game and fix what stops it, one
blocker at a time (T-016). Each of those iterations is slow. Knowing beforehand what the game
does between its entry point and its first frame loop tells us where it has stopped and what
it was about to do, and lets several fixes be prepared before the first run.

**Steps.** With `tools/re/decompile.sh`, `tools/re/callers.py` and `tools/re/strxref.py`,
starting at the entry point (`0x100008`) and the game's `main` (`0x132280`), write
`docs/knowledge/16-boot-sequence.md` with these sections:

1. **Call sequence.** From the entry point down three or four levels, in execution order:
   address, name in the function map, SDK name when `work/analyzer/config.toml` gives one,
   what the step does in one line, and whether the function is replaced by a runtime handler
   or recompiled (`config/stub_policy.toml`).
2. **I/O processor.** The reset (which image string it passes), the modules it loads, in
   order, from which package and through which functions. Close question Q-LOAD of
   `11-stub-policy.md` as far as static reading allows: the arguments the callers of
   `0x114C68` and `0x115278` pass, and what the game does with the result.
3. **Files.** Every file name the boot path opens, in order, and through which interface
   (`sceCd*`, the CRI file layer, `sceOpen`...).
4. **Threads, interrupts and callbacks** created or registered during boot: entry function,
   priority, stack size, and what each is for.
5. **System calls.** The system call numbers used by the functions of sections 1 to 4 (the
   instruction `syscall` preceded by `li $v1, N`), each with the name the runtime gives it and
   whether the runtime implements it (file and line in
   `third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/`), marking the ones that are
   missing or are only a TODO.
6. **Remote services.** The SIF RPC services the boot binds or calls (service identifiers),
   which I/O processor module serves each, and whether the runtime or its IOP emulator has it.
7. **Opening video.** Where the first video is started and what the game waits for before it
   goes on (input for T-113).
8. **Frame loop.** The function that runs once per frame, where it waits for the vertical
   blank, and what would be a reliable sign in a log that the game has reached it (input for
   T-015).
9. **What will probably fail first**, in order, with the reason and a proposed fix for each
   (a handler to bind, a configuration change, a runtime patch), without implementing any.

**Done when** the document has the nine sections, every statement about the game carries an
address, every statement about the runtime carries a file and line, and section 5 is a table
with one row per system call number.

**Limits.** Read only: nothing is built, run or changed. No whole decompiled function is
pasted (rule 1 of `AGENTS.md`); a few lines are fine when they justify a statement. What
static reading cannot settle is marked **Unverified**, with what would settle it at run time.
