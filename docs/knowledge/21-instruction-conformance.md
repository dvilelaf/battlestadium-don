# 21 · Instruction conformance: what the recompiler translates wrongly, and what this game can see of it

Task T-210 (six pieces, 2026-10-13). History and every measurement:
[docs/journal/025](../journal/025-instruction-conformance.md); its last section is the **regeneration checklist** (order of
patches 0043-0053, what each needs rebuilt, the two commands of the check and the numbers they print). Data:
[data/instruction_conformance.tsv](data/instruction_conformance.tsv).

Markers: **Measured** (a command was run and its output seen), **Reasoned** (read from the code or the table, not run at run
time), **Unverified** (the entry says what would settle it).

## 1. What the suite is and what it proves

Each instruction is given to the **real recompiler** (`ps2_recomp`), the generated C++ is compiled and run, and the resulting
registers and memory are compared with a model written from the reference emulator's interpreter (`third_party/pcsx2/pcsx2/`:
`R5900OpcodeImpl.cpp`, `MMI.cpp`, `FPU.cpp`, `VUops.cpp`, ...; every model function cites its source). For floating point and the
vector unit the driver sets the host control register to `0xFFC0` (round toward zero, flush-to-zero, denormals-are-zero) before
each case, as the game thread does (`src/host/fp_env.cpp`). Measured.

It proves: for the operand classes it runs (zeros, signs, denormal and maximal patterns, exponent all ones, sign-extension
edges, every destination mask the game uses), one instruction at a time, the generated code gives the reference's result. It
does not prove that a sequence of instructions works, or that the game never meets a case that was not run. No game file is
needed (the program is a few words of synthetic code per case). Measured.

Sensitivity: each piece was checked by changing the model on purpose (a wrong mask order, a wrong broadcast lane, a wrong shift
unit...) and seeing the instructions fail; the model was restored byte-identical. Measured (journal 025, "Sensitivity check").

## 2. How to run it and read the table

```bash
uv run --no-project --with rabbitizer --with pytest python -I -m pytest tests/conformance -q
```

- Shipping recompiler, committed series (patches 0043-0053 in it since 2026-10-13): `215 passed, 133 xfailed` (about 5 min). Measured.
- History: before the regeneration of 2026-10-13 the same command printed `185 passed, 163 xfailed` (168 instructions equal); with the
  eleven patches stacked in a scratch tree, `214 passed, 1 skipped, 133 xfailed` (the skip was the table-freshness test of that tree).
- Another recompiler and another list of patches: `DON_RECOMP=<ps2_recomp>`, `DON_RT_DIR=<tree with its runtime headers>`,
  `DON_CONF_SERIES=<file with patch names, one per line>`.
- `tests/conformance/test_conformance.py`, `KNOWN_DEFECTS`: each expected failure names the patch files that fix it and what is left
  after them. A failure is expected when something is left or a patch is not in the list. An entry whose patches are all in the list
  and which still fails fails with "the recompiler was not rebuilt".
- The table is regenerated with `uv run --no-project --with rabbitizer --with pytest python -I tests/conformance/make_table.py`
  (about 5 min; the sites are counted in the executable, which needs the disc).

Columns of `instruction_conformance.tsv`:

| Column | Meaning |
|---|---|
| `instruction` | The name as rabbitizer prints it (`qmtc2.ni` and `cfc2.ni` are the non-interlocked forms). |
| `family` | `arith`, `muldiv`, `shifts`, `compare`, `branches`, `memory`, `mmi`, `fpu`, `vu0`. |
| `sites_in_game` | Occurrences of the instruction in `SLPS_256.75` (0 = the game never executes it as code that was found). |
| `cases_run` | Cases that were run (forms x operand sets). |
| `equal_to_reference` | `yes`: every case equal. `no`: at least one case differs, with the failing classes in `note`. |
| `smallest_failing_case` | The simplest failing case, to reproduce it. |
| `overflow_cases_not_run` | Cases where the instruction raises an integer-overflow exception (add, addi, dadd...): counted, not compared. |
| `note` | The patch that fixes it, or the reason it is left, and the classes that fail. |

Three kinds of difference are **counted apart and never make a case fail**: the sticky exception flags of FCR31 and the MAC / status / clip
flags of the vector unit (no site in the game reads them for a decision), and cases where the reference interpreter itself is ambiguous
(a value wider than the console's register, an unaligned `lqc2`, TPC and R as the port stores them).

## 3. Result by family (the table regenerated 2026-10-13 after the regeneration, shipping recompiler)

| Family | Instructions | Equal | Differ, with sites in the game | Differ, without sites | Cases |
|---|---:|---:|---:|---:|---:|
| arith | 20 | 20 | 0 | 0 | 8,951 |
| muldiv | 20 | 20 | 0 | 0 | 15,384 |
| shifts | 15 | 15 | 0 | 0 | 1,209 |
| compare (slt..., movz, movn) | 6 | 6 | 0 | 0 | 846 |
| branches | 16 | 16 | 0 | 0 | 2,340 |
| memory | 21 | 21 | 0 | 0 | 39,144 |
| mmi | 94 | 56 | 0 | 38 | 77,542 |
| fpu (COP1) | 34 | 18 | 14 | 2 | 53,547 |
| vu0 (COP2 inline) | 105 | 26 | 34 | 45 | 129,302 |
| **Total** | **331** | **198** | **48** | **85** | **328,265** |

Measured (the counts come from the tsv: `awk` over columns 2-5). "With sites" means `sites_in_game > 0`; the 48 that
still differ and have sites are all the exponent-all-ones case (section 5): 14 fpu, 34 vu0.

History: the table before the regeneration had 168 equal, 68 differ with sites, 95 differ without. The regeneration moved 30 instructions
to equal and 20 with sites; 10 without sites also became equal. Measured.

## 4. Differences that have sites in this game

| Difference | Sites | Status | Can play reach it? |
|---|---|---|---|
| bltzall / bgezall link even when the branch is not taken | 0 | fixed by 0043, in the series since 2026-10-13 | No site. |
| movz / movn copy all 128 bits, the reference moves the low 64 | 527 / 778 | fixed by 0044, in the series since 2026-10-13 | Only where the upper half of the destination is read afterwards. Unverified: a run with 0044 and a comparison. |
| lq / sq with an unaligned address do not round down | 11,841 / 12,399 | fixed by 0045, in the series since 2026-10-13 | Only if an address is unaligned at run time (a game bug); expected no. Unverified. |
| SA kept in bits instead of bytes: mtsa, mtsab, mtsah, qfsrv | 1, 32, 1, 42 | fixed by 0046, in the series since 2026-10-13 | No: the port is consistent with itself, and the direct reads of SA are the interrupt context save, which hands the value back unchanged. Reasoned. |
| div.s by zero gives an infinity or NaN, the console the largest finite number | 1,981 | fixed by 0047, in the series since 2026-10-13 | Yes by construction (a zero length, a normalised null vector); the frequency in a fight is not measured. |
| cvt.w.s of 2^31 and beyond gives 0x80000000 for a positive value | 911 | fixed by 0048 (header only), in the series since 2026-10-13 | Only if a value passes 2^31 before the conversion. Unverified. |
| min.s / max.s with a signed zero or a denormal pattern | 6 / 23 | fixed by 0049, in the series since 2026-10-13 | Needs such an operand; unlikely. |
| sqrt.s of -0 or a denormal gives +0 | 96 | fixed by 0050, in the series since 2026-10-13 | Needs such an operand. |
| VF0 is writable (qmtc2, lqc2 and any operation with destination 0) | 1 (an `lqc2` at 0x102cbc) | fixed by 0051, in the series since 2026-10-13 | The effect is nil unless a saved VF0 holds something other than (0,0,0,1). Reasoned. |
| vmax / vmini on float compare instead of bit patterns | 87 each (+1 vmaxx, +1 vminix) | fixed by 0052, in the series since 2026-10-13 | Needs a signed zero or a denormal operand. |
| vsqrt of a negative number gives 0 | 1 (0x1b805c) | fixed by 0053, in the series since 2026-10-13 | Unverified: whether a fight's 3D path takes a negative operand there. |
| Every arithmetic form with an exponent-all-ones operand | all of them | not fixed (decision, section 5) | Unverified: it is what a division by zero or an overflow leaves; 0047 removes the division source. |

Rows 0043-0053 are measured as fixed by the conformance suite (215 passed). Whether play reaches them is as marked; the pictures of two routes
were byte-identical before and after (journal 025, follow-up). Sources: journal 025, "Result", "Differences" of pieces 5 and 6, and the tsv. Measured (sites) / Reasoned (reachability), as marked.

## 5. The two standing decisions

1. **Operands whose exponent field is all ones are not clamped** (fpu and vu0 arithmetic, compares, `c.*.s`). The console reads
   such a pattern as a very large finite number; the host reads an infinity or a NaN. Fixing it would add work to each of the roughly
   30,000 arithmetic sites for a pattern a fight hardly meets: patch 0047 removes the infinity that a division by zero produced, the
   main source. Decision of the owner (journal 025, piece 5). These instructions stay `no` in the tsv with the reason in `note`.
2. **Instructions with no site in this game are not patched.** 95 instructions differ and the game never executes them; a patch
   there costs a regeneration and a regression run and changes nothing observable. They stay in the table as a record.

## 6. What the suite does not cover

- **Microprograms**: `vcallms`, `vcallmsr` (1 site, 0x102b14) and `ctc2` to CMSAR1 (1 site). Running them would need a micro-memory image,
  an interpreter of the micro instruction set and the unit's data memory; none of that is in the harness state.
- **VU0 operations not run**, with their sites in the executable (census 2026-10-13): vclip 0; integer operations (viadd, viaddi, viand,
  vior, visub, vilwr, viswr) 0; vlqi 1; vsqi 1; vlqd 0; vsqd 0; vmfir 0; vmtir 0; vrget, vrnext, vrinit, vrxor 0; vwaitq 1. Measured.
  vlqi/vsqi/vlqd/vsqd address the unit's data memory, which is not part of the state.
- **Exceptions and traps**: only the integer overflows the generated code raises (add 12 sites, addi 29; dadd, dsub, daddi 0), counted
  apart in `overflow_cases_not_run`. Interrupts, the TLB, the cache instructions, COP0 registers, the GS / VIF / DMA registers and
  instruction timing are not run.
- **Sticky flags**: FCR31's exception bits (I D O U SI SD SO SU) and the vector unit's MAC, status and clip flags. The port computes
  none of the vector ones; the game reads them at one site each (a context save) with no consumer.
- **Sequences**: each case is one instruction; interactions through the accumulator, the flags or delay slots are not run.
- The exponent-all-ones operands once the arithmetic is patched (section 5).

## 7. Warning for any other game built on this runtime

The suite measures this game's instructions only where it counted sites. Instructions that the recompiler translates wrongly and
that **this game happens not to use**, so nothing here patches them, and another game may:

- branches: `bltzall`, `bgezall` (0043 fixes them);
- floating point: `rsqrt.s`, `suba.s` (also with exponent-all-ones operands differing);
- multimedia (38): padduh psubuh psllvw psrlvw psravw pinth pinteh pabsw pabsh pexeh prevh prot3w pexch pexcw plzcw pmaddw pmsubw
  pmultw pmultuw pmadduw pmaddh pmsubh pmulth phmadh phmsbh pdivw pdivuw pdivbw pmfhi pmflo pmfhl.lw pmfhl.uw pmfhl.slw pmfhl.lh
  pmfhl.sh pmthi pmtlo pmthl.lw (the tsv has their smallest failing cases);
- vector unit: vftoi0/4/12/15, vdiv, vrsqrt, and the accumulate and other forms of the arithmetic with no site (listed in the tsv with
  `sites_in_game` 0 and `equal_to_reference` no).

Before reusing the runtime for another title, run the suite against that title's executable (the table counts sites from the disc named
by `DON_ISO`/`disc/`) and read every row with `no` and sites above 0; and note that the eleven patches of section 4 are already in
`series`; for a new recompiler build, follow the regeneration checklist (journal 025).
