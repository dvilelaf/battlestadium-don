# 001 · The function map: three wrong assumptions and their fixes

- **Date:** 2026-10-11
- **Task:** T-006, T-008, T-009, T-010, T-025, T-026
- **Status:** resolved

## Symptom

Nothing failed at run time: this entry records what the first Ghidra export contradicted in
the plan, before any code was generated. Three assumptions of the task cards turned out to be
false when the export of T-006 was measured:

1. "The code ends at about `0x381000`." Function rows went on without interruption up to
   `0x3902D4`, and nine more functions appeared at `0x3BC5D0`–`0x3BD8C8`, inside the data.
2. "A row of the export is a function." 1,775 function rows lay inside another one, 32 pairs
   overlapped partially, and 12 functions of 24 to 164 bytes had spans of 19 to 118 KB.
3. "What is in no row is not code." 50,136 bytes of the main text were in no row, and every
   word of them decoded as a valid instruction.

Commands that show the final state (they are the "Done when" of T-026):

```bash
uv run --no-project --with rabbitizer python -I tools/funcmap/validate.py --csv config/functions.ghidra.csv \
    --layout config/layout.toml --elf disc/SLPS_256.75 --extra config/extra_entry_points.txt
```

## Classification

Not a run-time category. Precedent: the Killzone project (`drakolordx7/KillzoneRecomp`) keeps
its own scripts to complete the Ghidra export with code pointers found in data
(`docs/knowledge/08-external-references.md`).

## Investigation

1. **Code end.** The first estimate came from a page heuristic (pages with more than 97 %
   valid instructions and at least one `jr $ra`). The export showed functions beyond it, so
   T-008 measured the layout from the rows: main text `0x100000`–`0x3902D4`, 56 bytes of
   padding, then the VU microprograms. Rows beyond the text were grouped into islands: one is
   real code that addresses kernel memory (`lui 0x8007`), copied by the SDK to
   `0x80074000`–`0x80076000` through system call `0x5A`; the other two are stray labels over
   data. The sizes passed to the copy calls end exactly where the next blob starts.
2. **Spans.** `ExportPS2Functions.java` writes `End` = highest address of the Ghidra body.
   A Ghidra body can be several separate ranges (a small routine that jumps to a shared
   return far away), so the span is not the function. The first fix tried, "take only the
   range that contains the entry point", was wrong too: 1,067 functions have holes of a few
   words that Ghidra's flow does not reach, and each was cut at its first hole
   (`FUN_00190580`: 22 KB to 556 bytes; labels outside every function: 2,826 to 15,478). The
   agent stopped and reported instead of accepting the result, which is what exposed it.
3. **Code in no row.** T-009 found 322 stretches of valid code in no row; after the spans were
   fixed there were 463 (the giant spans had been covering some by accident). In Ghidra 461
   were undefined bytes: nothing it followed leads there. They are not all dead: 39 addresses
   in them are referenced by pointers, and that code calls 13 more addresses that no scan of
   the rows can see, because their callers are in no row either.
4. Ruled out along the way: the 42 instructions the analyzer wanted to nop as "self-modifying
   code" are ordinary stores to globals (T-021); the 12 `lui 0x1100` are constants, not
   accesses to vector unit memory (T-121).

## Cause

- The page heuristic missed the last 62 KB because large functions do not have a `jr $ra`
  on every page.
- The export script of PS2Recomp assumes that a Ghidra body is one contiguous range.
- Ghidra only disassembles what its analysis reaches. Code reached only through pointers it
  did not recognise, and whatever that code calls, stays undefined.

## Fix

- `tools/funcmap/layout.py` writes `config/layout.toml` (main text and islands); every tool
  reads the text range from it instead of a constant.
- `patches/ps2recomp/0003-ghidra-export-contiguous-function-ranges.patch`: a function row
  goes from the entry point to the end of the last range of the body that starts before the
  next function; far ranges become label rows.
- `tools/funcmap/ghidra/ClaimOrphanCode.java`, run by `tools/funcmap/ghidra_export.sh` after
  the analysis: every non-zero word of the main text without an instruction is disassembled
  and, if no function contains it, becomes a function; Ghidra then analyses the new code.
- `tools/funcmap/find_code_pointers.py` writes `config/extra_entry_points.txt`: addresses
  held as pointers (data words, `lui`+`addiu` pairs, call targets) that are not the start of
  a row. The recompiler accepts them as entry points.
- `tools/pipeline/make_config.py` drops the rows outside the main text (the kernel patch blob
  is never executed at its address in the executable: the runtime replaces the kernel).

These fix the causes: the map is now derived from measurements, and nothing is hidden (no
row was deleted by hand, no check was relaxed).

## Verification

Measured on 2026-10-11 after T-026:

| | First export | Final |
|---|---|---|
| Rows (functions / labels) | 56,246 (7,111 / 49,135) | 57,789 (8,050 / 49,739) |
| Nested functions / partial overlaps | 1,775 / 32 | 0 / 0 |
| Sum of function spans in the main text | 3.71 MB | 2.48 MB before the claim step |
| Non-zero words of the text in no row | 8,999 (first count) | 0 |
| Main text covered by rows | 97.89 % (partly by accident) | 99.13 % (the rest is padding) |
| Pointers to code in no row (`new_code`) | 30 | 0 |
| Extra entry points | 255 | 194 |
| Validator on the filtered map | not run | `E1=0, E2=0, E3=0, E4=0, E5=0` |

The whole test suite passes (96 tests).

## Open points

- A pointer built in a way none of the scans sees (computed, or a `lui`/`addiu` pair that
  uses two registers) would still give "function not found" at run time. The target is now
  always inside a row, so the fix will be one line in `config/extra_entry_points.txt`.
- The sweep may have created functions at unreachable tails of earlier functions. It only
  costs rows.
- 41 rows named `caseD_*` are functions for Ghidra and labels for our tools (which classify
  by name). No effect seen; noted in case a count does not add up.
- Reproducibility (checked after this entry was first written): two runs of
  `ghidra_export.sh --force` (import, analysis, claim, export; about one minute each) gave a
  CSV byte-identical to the versioned one. The 39 seed addresses are kept in
  `config/ghidra_seeds.txt`.

## Temporary diagnostics

None added.
