# 025 · Instruction conformance tests, part 1: branches, comparisons, shifts

- **Date:** 2026-10-13
- **Task:** T-210 (first piece)
- **Status:** harness resolved; two differences found, patch files 0043 and 0044 (not in `series`)

## Symptom
The sign-branch defect (journal 019) was found from a rolled picture. The aim is to find that kind of defect
without a symptom: run each instruction through the port's own recompiler and compare with the reference.

## How the harness works (`tests/conformance/`)
- `asm.py`: a tiny assembler for the integer subset used. `ref.py`: the reference model, an interpreter of those
  words written from `third_party/pcsx2/pcsx2` (`R5900OpcodeImpl.cpp`: SLL..DSRAV, SLT*, MOVZ, MOVN;
  `Interpreter.cpp`: BEQ..BGEZALL with `_SetLink`/`_BranchTarget_` from `R5900.h`; every function is named in
  the comments). A register is a (low, high) pair; like the reference, 64-bit instructions write the low half only.
  It executes the very same words the guest function holds, delay slots included.
- `families.py`: a family builds guest functions (one per instruction form and variant) and the cases (start states
  of the 32 registers; every register has a distinct pattern in both halves so a stray write shows).
- `harness.py`: lays the functions in a tiny MIPS ELF, runs the recompiler (`DON_RECOMP`, or the binary
  `tools/pipeline/generate.sh` uses), compiles the generated files as one translation unit with `driver.cpp`
  against `third_party/PS2Recomp/ps2xRuntime/include` (`-std=gnu++20 -O2 -march=x86-64-v3 -ffp-contract=off`;
  the generated code of straight-line functions needs no runtime library), in `build/conformance/<family>/`
  (cached by content; one compile of about 2 s per family), runs the driver and compares all 32 registers (128 bits).
- `test_conformance.py`: one test per instruction, plus that the assembler's words decode (rabbitizer) as the
  instruction they claim, and that the table is current. Open defects are `xfail(strict)` with the patch that fixes them.
- `make_table.py` writes `docs/knowledge/data/instruction_conformance.tsv`; `census.py` counts the sites of each
  mnemonic in 0x100000-0x3902D4 of `disc/SLPS_256.75` with rabbitizer (the tests never read it). The `sll` count
  does not include `nop` (rabbitizer names the all-zero word `nop`).
- Run: `uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/conformance -q`
  (skipped when no recompiler binary exists). Note: `single_file_output = true` of the recompiler fails
  ("combined output completion queue is missing index"), so the harness uses the multi-file output and a unity file.

## Cases
- Operand classes (13): 0, 1, 0x7FFFFFFF, 0x80000000 (bit 31 set, upper half zero), 0xFFFFFFFF, 0x1_00000000 (upper
  half set, lower zero), 0x1_80000000, 0xFFFFFFFF_80000000, -1, the most positive and most negative 64-bit values,
  most negative + 1, and an arbitrary pattern.
- A, branches (16 mnemonics): beq/bne/beql/bnel with all 169 operand pairs; blez, bgtz, bltz, bgez, their likely forms,
  bltzal, bgezal, bltzall, bgezall with the 13 values; each with three delay slots: a marker, one that changes the
  tested register `a0` (`nor`), one that changes `a1`. The delay slot must not change the decision; for likely
  branches it must run only when taken; for link forms `$ra` must be written either way.
- B, comparisons: slt, sltu, movz, movn with 169 pairs; slti, sltiu with immediates -1, 0, 1, 0x7FFF, -0x8000.
- C, shifts: sll, srl, sra, dsll, dsrl, dsra, dsll32, dsrl32, dsra32 with sa 0, 1, 31 (so 32, 33, 63 for the
  `32` forms); the variable forms with amounts 0, 1, 31, 32, 33, 63, 64, 65, and three with upper bits set.

## Result (37 instructions, 4,395 cases)
All 37 equal to the reference except four:

| Instruction | Sites in the game | Difference | Patch |
|---|---|---|---|
| bltzall, bgezall | 0, 0 | `$ra` is written only when the branch is taken; the reference writes it always (18 of 26 and 8 of 26 cases) | 0043 (`control_flow_emitter.cpp`) |
| movz, movn | 527, 778 | `SET_GPR_VEC` copies all 128 bits; the reference moves the low 64 only, the upper half of `rd` must stay (13 of 169 and 156 of 169 cases) | 0044 (`special_translator.cpp`) |

The recompiler relinked with both patches passes all 41 tests (checked with `DON_RECOMP` pointing at it; the
`xfail(strict)` entries then fail on purpose: delete `KNOWN_DEFECTS` entries when a patch enters the series).
The sign branches (journal 019) and everything else in the three families are equal to the reference. The game does
not use bltzall/bgezall/bltzal/bgezal at all; movz/movn matter only where a 128-bit register's upper half is read
afterwards, which is not known: apply 0044 only with a regeneration and a regression run.
Not verified: that this harness would have caught the old `GPR_S32` branches (the cases cover them by design:
0x80000000 with a zero upper half, but the pre-0039 recompiler was not run on them).

## Next pieces, in the order I would do them
Done: branches, comparisons, shifts (piece 1); integer arithmetic and logic, multiplication and division with their
result registers (piece 2); loads and stores with their unaligned halves and lq/sq (piece 3, see the follow-up of
2026-10-13 below; the multimedia set, piece 4, see its follow-up at the end). Left:
1. The floating-point coprocessor (add.s... cvt, c.*, bc1*, the 32 fprs, `fcr31` flags and the PS2's
   non-IEEE rules: no NaN/infinity, clamped results, denormals flushed): the driver prints the fprs and fcr31; the
   model follows `FPU.cpp`; the cases need denormals, overflow and division by zero. The coprocessors' loads and
   stores (lwc1, swc1, lqc2, sqc2) belong here and in item 2, since they move bytes between the test block and
   the fprs or VU0 registers.
2. VU0 instructions used inline (vadd.. vmaddabc, vdiv, vsqrt, qmfc2/qmtc2, cfc2/ctc2, vcallms): needs the VU0
   state (vf, vi, ACC, Q, P, flags) in the context and the comparison, and `VUops.cpp`/`COP2.cpp`; last because
   the game also runs VU1 microcode through microVU, which is covered by other tests.
Open from the pieces: put patches 0043, 0044, 0045 and 0046 in the series (regenerate and run a regression), then
delete the six `KNOWN_DEFECTS` entries; the special-address path of the loads and stores (hardware registers,
scratchpad, uncached windows: `runtime->Load*`/`Store*`) is not tested, the test block is ordinary RAM.


## Follow-up 2026-10-13: families D (arithmetic and logic) and E (multiplication and division)
Result: **all 40 instructions equal the reference; no difference, so no new patch file** (0045 is free).
Run: 79 passed, 4 xfailed (the four open defects of the first piece, unchanged). The two families are 203 forms
(163 + 40) and 24,335 non-overflowing cases (8,951 + 15,384); the table now has 77 instructions and 28,730 cases.
Sensitivity check: with the model changed on purpose (DIVU by zero, LUI without sign extension) the tests of
`divu`, `divu1` and `lui` fail, so equal means equal.

### What the harness gained
- Start state and comparison are 36 registers: r0..r31 (low and high halves) and HI, LO, HI1, LO1
  (`driver.cpp`, `harness.run_cases`, `ref.Machine`; indexes `ref.HI..LO1` = 32..35; `state(hi=..., lo1=...)`).
- `asm.py`: SPECIAL add..dsubu, and/or/xor, mfhi..divu; I-type addi, andi, ori, xori, lui, daddi, daddiu; the MMI
  opcode (`asm.mmi`) for madd/maddu/madd1/maddu1, mfhi1..mtlo1, mult1..divu1.
- `ref.py`: every function names its reference (R5900OpcodeImpl.cpp, MMI.cpp lines in the comments): the
  corner values of DIV (0x80000000 / -1 gives LO = 0xFFFFFFFF80000000, HI = 0, line 388; division by zero gives LO = 1
  for a negative dividend and -1 otherwise, HI = the dividend, line 399) and DIVU (by zero: LO = -1, HI = the
  dividend sign-extended, line 417); MADD forms accumulate only the low words of HI and LO (MMI.cpp 20-50).
- Overflowing instructions (add, addi, sub, dadd, daddi, dsub): the model raises `ref.Overflow`; those cases are not
  run and are counted in the new last column of the table (`overflow_cases_not_run`; 536 cases). `driver.cpp` defines
  `PS2Runtime::SignalException` as a stub that aborts, only to satisfy the linker.
- Register shapes per form: all distinct, `rd = rs`, `rs = rt`, `rd = $zero` (and `rt = $zero` for immediates), because
  the translator has special code when the destination is $zero or the operands coincide. Accumulators for madd: six
  pairs whose upper halves are not sign extensions; the other result registers start with distinct patterns, so a
  write to the wrong pair shows.

### Overflow: what the port does, and the sites
Generated code for add, addi, sub, dadd, daddi, dsub tests the overflow exactly (same classification as the model:
32-bit sum or difference outside the range; 64-bit likewise) and calls `runtime->SignalException(ctx,
EXCEPTION_INTEGER_OVERFLOW)` instead of writing the destination. The runtime (`ps2_runtime.cpp:1475`, `raiseCop0Exception`)
sets EPC (the branch address in a delay slot), Cause.ExcCode (with BD) and Status.EXL and sets `ctx->pc` to the
exception vector. The console then runs the handler. In the port the generated function goes on with the next
instruction (each statement sets `ctx->pc` itself), so the vector is never entered: the destination keeps its old value
and execution continues. Equal to the console only if no site ever overflows. This was read from the source, not run.
Sites in the game: add 12, addi 29; sub, dadd, dsub, daddi 0. Whether any of those 41 can overflow at run time was not
measured (rabbitizer counts decodable words, some may be data). One difference in the reference itself, not in the
port: PCSX2 computes SUB and DSUB as `rs + (-rt)` on the truncated operand (R5900OpcodeImpl.cpp 347, 356), which misses
the overflow of `0 - 0x80000000` and `0 - (-2^63)`; the model uses the architecture's exact rule; sub and dsub have 0 sites.

### Table summary for the two families
arith (20): add 12, addu 15,467, sub 0, subu 2,344, dadd 0, daddu 50,463, dsub 0, dsubu 134, and 3,807, or 5,144, xor 216,
nor 119, addi 29, addiu 81,900, daddi 0, daddiu 97, andi 10,000, ori 10,271, xori 469, lui 33,504: all equal.
muldiv (20): mult 1,581, multu 9, mult1 67, multu1 1, madd 31, maddu/madd1/maddu1/divu1 0, div 332, divu 176, div1 8,
mfhi 324, mflo 359, mfhi1 6, mflo1 5, mthi 2, mtlo 24, mthi1 2, mtlo1 2: all equal.
Not covered: the translator also has cases for MSUB and MSUBU, which the reference emulator does not implement (they are
not R5900 instructions; MMI function 0x04 is PLZCW). Nothing to compare against.

## Follow-up 2026-10-13: family F (loads and stores)
Result: **one difference, in lq and sq with an address that is not a multiple of 16; the other 19 instructions
are equal to the reference.** Patch `patches/ps2recomp/0045-lq-sq-align-address-down.patch` (not in `series`); the
tests of lq and sq are `xfail(strict)` in `KNOWN_DEFECTS`. Run: 99 passed, 6 xfailed; with a recompiler relinked
with 0043, 0044 and 0045 (`DON_RECOMP`) the 22 memory tests pass, and the six expected failures turn into passes
(strict xfail then fails, as designed). 341 forms, 39,144 cases; the table has 98 instructions.

### How the harness gives the generated code its memory
The generated loads and stores are `READ8..READ128` / `WRITE8..WRITE128` of `ps2_runtime_macros.h`. For an address
that `Ps2IsSpecialAddress` does not flag they index `rdram + (addr & PS2_RAM_MASK)` and never touch the `runtime`
pointer, so no real `PS2Runtime` is needed: the driver allocates the 32 MB `rdram` (it was 4 MB), keeps passing a
null runtime, and defines stubs of `PS2Runtime::Load8..128` / `Store8..128` that abort (they are only on the
special-address branch, which the link needs). The test block is 4 KB at 0x00300000 (ordinary RAM); the driver
fills it with a known pattern before every case (`pattern(i)`, the same formula in `ref.py`), runs the function,
and prints the count and (offset, value) of every byte that differs from the pattern. The harness compares those
bytes with the model's, so a store that writes one byte too many or too few shows. The model's memory is a
bytearray in `ref.Machine`; an access outside the block raises, so a bad test cannot hide.

### What is covered
- Aligned: lb, lbu, lh, lhu, lw, lwu, ld, lq, sb, sh, sw, sd, sq, each with five immediates (0, 7, -7, 0x7FFF,
  -0x8000), the base register derived from the target address (so the 32-bit wrap-around is exercised) and with
  garbage in its upper half, three data-register shapes (`rt` = a1, `rt` = the base register, `rt` = $zero),
  addresses at the start, the end and the middle of the block, the 13 value classes for stores and for the merge
  forms.
- Unaligned halves lwl, lwr, swl, swr (offsets 0 to 3) and ldl, ldr, sdl, sdr (0 to 7) at three places of the
  block, same shapes and immediates.
- The compiler's idiom as a pair on the same register, in both orders: `lwr`+`lwl`, `lwl`+`lwr`, `ldr`+`ldl`,
  `ldl`+`ldr`, `swr`+`swl`, `swl`+`swr`, `sdr`+`sdl`, `sdl`+`sdr` (second immediate = first + width - 1). The table
  counts a pair under its first instruction, so e.g. the `lwr` row includes the `lwr`+`lwl` pairs.
- Model, with the reference lines: R5900OpcodeImpl.cpp LB 520 ... SQ 872; the masks and shifts of LWL/LWR/LDL/LDR/
  SWL/SWR/SDL/SDR are copied from the tables there (:625, :710, :782, :838). lwl sign-extends the merged word, lwr
  does so only at offset 0 and otherwise keeps the upper half of the register, ldl/ldr merge all 64 bits.
- Not covered on purpose: lh/lhu/lw/lwu/ld/sh/sw/sd at addresses that are not aligned (the console raises an
  address error; the reference only logs it), the special addresses, and `ld` into $zero (the reference writes r0
  there, a quirk of the reference; the model keeps r0 at zero, and the port does too: equal).

### The difference
`lq` and `sq` at an address that is not a multiple of 16: the reference clears the low four bits (as the console
does); the port reads or writes the 16 bytes at the address as given. 24 of the 294 lq cases and 24 of the 984 sq
cases differ, all of them in the form "unaligned address" (offsets 1, 4, 8, 15); all aligned cases are equal. Fix: mask
the register-based address of the two instructions with `& ~0xFu` in the translator (0045); a constant address from
the memory-hint path is not masked (not expected). Sites in the game: lq 11,841, sq 12,399. A site only matters if its
address is not a multiple of 16 at run time; compilers emit them aligned, and an unaligned one would be a game bug,
so I expect no change to the game. Not measured.

### Table summary for this family
lb 1,220, lbu 5,233, lh 1,892, lhu 1,779, lw 66,663, lwu 204, ld 15,310, lq 11,841, sb 3,670, sh 1,872, sw 37,831,
sd 12,715, sq 12,399, lwl 159, lwr 159, swl 155, swr 154, ldl 1,101, ldr 1,101, sdl 1,259, sdr 1,262 sites in the game;
all equal except lq and sq (unaligned form).

### Sensitivity check
With the model changed on purpose, the tests fail: LWL_MASK[1] altered gives a failure of `lwl`; SH writing one byte
instead of two gives "570 of 570 cases differ" for `sh` (the first: `memory: [0x1] got 00 want 8a`); SB writing two bytes
fails with an out-of-block access error at the end of the block. The model was restored afterwards.

## Follow-up 2026-10-13: family G (the multimedia set and the shift-amount register)
94 mmi rows (every MMI instruction the recompiler translates and the reference implements, plus mfsa, mtsa, mtsab,
mtsah). Registers are 128 bits in the start state, the model and the comparison; the state is now 37 pairs (SA
added, in bytes). The driver catches a throw (an untranslated instruction) and SIGFPE per case; none of the 94 is
untranslated. Model: `tests/conformance/mmi_ref.py`, written from `MMI.cpp` (and `R5900OpcodeImpl.cpp` for the SA
instructions). Operands: lanes with the sign bit set and clear, the saturation boundaries of each lane width, zero,
all ones, vectors in which every lane differs; every shift amount of the immediate shifts; sa 0 to 15 for qfsrv.

### Result that matters
52 rows equal, 42 not. Of the 30 multimedia instructions the game uses (paddh, paddub, padduw, pand, pcgth, pcpyh,
pcpyld 819 sites, pcpyud, pexew, pextlb, pextlw, pextub, pextuw, pmaxh, pminh, pnor, por, ppacb, ppacw 101, psllh,
psrah, psrlh, psubb, psubw, pxor, mfsa, ...), 26 are equal to the reference through the recompiler, with the sites
in the table. The four that differ are the shift-amount ones: qfsrv (42 sites), mtsab (32), mtsah (1), mtsa (1).

### The shift-amount difference
The reference keeps SA in bytes (`cpuRegs.sa`; MFSA/MTSA copy it, MTSAB stores `(rs ^ imm) & 0xF`, MTSAH
`((rs ^ imm) & 7) << 1`, QFSRV shifts by `sa << 3`). The port keeps it in bits: mtsab stores `... << 3`, mtsah
`... << 4`, qfsrv uses `sa & 0x7F`, mfsa returns the bits (8x) and mtsa masks the value to 7 bits. The game cannot
see it: mtsab/mtsah only feed qfsrv, and the port is consistent with itself there (qfsrv equal to the reference for
every amount when the amount is given in the port's unit); the three sites that read or write SA directly are the
interrupt-handler context save and restore: mfsa at 0x1160ac and 0x1162ec store SA next to HI/LO/HI1/LO1, mtsa at
0x11619c reloads it, and the value goes back unchanged (a value of 0..0x78 in bits survives the 7-bit mask). Fix in
patch 0046 (recompiler, emitted text: special_translator.cpp, regimm_translator.cpp, mmi_translation_helpers.cpp,
about 5 lines); with it all five rows are equal. Not in the series. The driver passes SA raw, so qfsrv, mtsa, mtsab and
mtsah are `xfail(strict)` until 0046 is in the series (mfsa passes raw).

### Differences in instructions the game never uses (not patched)
All have 0 sites in the game and are `xfail(strict)` with "no site in this game; not patched". A warning for any other
game built on this runtime:
- Wrong translation: padduh and psubuh (wrap instead of saturating), psllvw/psrlvw/psravw (value taken from rs, all four
  lanes, no sign extension to 64 bits; the reference uses Rt, Rs & 31 for lanes 0 and 2 only), pinth, pinteh, pabsw and
  pabsh (read rs instead of rt; no clamp of 0x80000000/0x8000), pexeh, prevh, prot3w, pexch, pexcw (wrong lane
  permutations), plzcw (upper half clobbered).
- The HI/LO group: pmaddw, pmsubw, pmultw, pmultuw, pmadduw, pmaddh, pmsubh, pmulth, phmadh, phmsbh, pdivw, pdivuw,
  pdivbw, pmfhi, pmflo, pmthi, pmtlo, pmfhl.{lw,uw,slw,lh,sh}, pmthl.lw. The port keeps 64-bit HI/LO and handles only lane
  0 (HI1/LO1 are not used for lane 2, results are truncated to 32 bits); the reference has 128-bit HI/LO. Fixing it needs
  runtime helpers taking the context; not done. The model for these was transliterated from MMI.cpp and read for
  PMADDW, PMSUBW, PMSUBH, PDIVBW and PMFHI; for the others the structural gap above is enough to call the port wrong,
  but the exact numbers (e.g. the 0x70000000 correction of PMADDW) are "model unverified" beyond the transliteration.

### Table summary for this family
Sites of the equal rows are in `docs/knowledge/data/instruction_conformance.tsv` (new `note` column: the patch or the
"no site" reason).

### Sensitivity check
Model changed on purpose: PCPYLD with its two operands swapped fails `pcpyld`; PPACW/PPACB taking the odd lanes of Rs
fail `ppacw` and `ppacb`. The model was restored (byte-identical).

### Left
Floating point is done in the follow-up below; VU0 (item 2) is left.

## Follow-up 2026-10-13 (T-210 piece 5): the floating-point coprocessor (family H, `fpu`)

**What was run.** `tests/conformance`, family `fpu`: 34 instructions, about 54,000 cases (add/sub/mul/div/max/min/rsqrt 2,648 each, the
accumulator forms 2,209-2,405, the compares 4,418 each, the rest 8-282). The state of a case grew from 37 to 71
(low, high) pairs: F0..F31, the accumulator and FCR31 follow SA. The driver sets the host's control register to 0xFFC0
(round toward zero, flush-to-zero, denormals-are-zero: src/host/fp_env.cpp) before each case and restores it after, as the
game thread does. Each case starts with a different FCR31 (the legal bits in several combinations) so a flag that is not
cleared shows. Reference: `fpu_ref.py` from `third_party/pcsx2/pcsx2/FPU.cpp` (each instruction has its function cited).
`test_fpu_model.py` tests the model's own arithmetic on values worked out by hand (7 tests).

**How the model rounds.** No host float is used. Operands become exact `Fraction`s (after the reference's `fpuDouble`: exponent 0 is a
zero of the same sign, exponent 255 is the largest finite number); add, sub, mul and div are exact; the square root is
`isqrt` of the radicand scaled by a power of four that leaves more than 26 bits, and rsqrt is `isqrt` of the exact square of the
quotient. The exact value is then truncated to 24 significant bits (`encode`); a result at or above 2^128 is the largest finite number (truncation never
reaches infinity), one below 2^-126 is a zero with the sign of the exact result (flush), and the interpreter's own overflow and
underflow checks therefore never fire in this mode and only clear their flags. Zero results: the sign is negative only when
both operands of an addition are negative zeros or the signs of a product or quotient differ. Hand-computed checks: 1 + 3·2^-25 is
1.0 (nearest would be 1 + ulp), 1/3 is 0x3EAAAAAA, sqrt(5) is 0x400F1BBC (nearest ...BD), 1 / -0 is -largest with D|SD,
0/0 is the largest with I|SI, 2^31 converts to 0x7FFFFFFF, -2^31 - ulp to 0x80000000.

**Result of the run (shipping recompiler, no patch applied).** Equal: abs.s, neg.s, mov.s, cvt.s.w, c.f.s, bc1f, bc1t, bc1fl, bc1tl,
mfc1, mtc1, cfc1, ctc1, lwc1, swc1. Different: the ones below. With 0047-0050 built in (`DON_RECOMP`, `DON_RT_DIR`), every
remaining failing case of every row has an operand whose exponent field is all ones, except rsqrt.s (0 sites).

### Differences
1. **Division by zero (div.s, 1,981 sites; patch 0047, recompiler, emitted text `fpu_translator.cpp`).** The port tests
   `ft == 0.0f`, sets a flag bit (0x100000) the console does not have and writes `copysignf(INFINITY, fs * 0.0f)`: an
   infinity, and a NaN for 0/0. The console writes the largest finite number with the sign of the operands (D|SD set;
   I|SI when the dividend is zero too), and a denormal divisor counts as zero (the port divides by the flushed zero and gets an infinity
   too). A fight reaches it (a zero length, a null vector normalised), and the infinity then travels through every add, mul and
   compare that follows; this is the difference that matters most. After 0047 only exponent-255 operands differ.
2. **cvt.w.s of 2^31 and beyond (911 sites; patch 0048, runtime header `ps2_runtime_macros.h`, macro `FPU_CVT_W_S`).** The port returns
   0x80000000 for a positive value that does not fit (x86's answer); the console returns 0x7FFFFFFF. Negative values already agreed.
   Reachable if a counter or position grows past 2^31 before the conversion. 14 of 61 cases differed; none after.
3. **min.s and max.s with signed zeros or denormal patterns (6 and 23 sites; patch 0049, recompiler plus two helpers in the
   runtime header).** The console orders the bit patterns as signed integers (-0 < +0; a denormal pattern is a number); the port
   used std::min/std::max on floats. Needs a signed zero or a denormal operand: unlikely, cheap to fix.
4. **sqrt.s of -0 or a denormal (96 sites; patch 0050, recompiler).** The console gives a zero with the operand's sign; the port gives +0.
   Rabbitizer does not know sqrt.s and names the word `c1`; the census (`census.py`) now counts it as sqrt.s, which is why an
   earlier count of 96 `c1` sites in the game is these.
5. **Operands with the exponent field all ones (add.s, sub.s, mul.s, div.s, adda/suba/mula, madd/msub/madda/msuba, c.eq/c.lt/c.le,
   sqrt.s; about 30,000 sites together). Not patched.** The console reads such a pattern as a very large number (the largest finite
   number, with the sign) and the port lets the host treat it as an infinity or a NaN (an infinity input gives an infinity or NaN
   output, a compare with a NaN is false). The only operands of this kind a fight could meet are bit patterns of data read as floats
   (an integer or a packed value moved by mtc1/lwc1 and used before conversion): no arithmetic produces one under round-toward-zero
   (overflow gives the largest finite number) and, since 0047, neither does a division by zero. Fixing it means clamping both
   operands of every arithmetic instruction, a cost on the hottest code of the game for a case not seen. Left as a known gap; each of
   those rows is `xfail(strict)` with "exponent-all-ones operands are not clamped".
6. **rsqrt.s (0 sites).** Division by zero gives an infinity or NaN; also the double rounding of `fs / sqrtf(|ft|)` against the reference's
   `fs / sqrt(|ft|)` in double (196 cases ambiguous, below). Not patched: "no site in this game".

Sticky flags: add/sub/mul/div/min/max/sqrt/abs/neg and the accumulator forms differ from the reference in the O, U (and the
D/I) flags of FCR31 in 500-880 cases per row because the port never touches them; the game has no `cfc1` site (0 sites) and
one `ctc1`, so nothing reads them. Reported as "differ only in the sticky exception flags" in the `note` column, not as a failure;
bc1* read the C bit, which is compared (the compares and the branches are equal for every operand but those of item 5).

### Reference ambiguous (counted apart, never as a failure)
- **div.s, sqrt.s, rsqrt.s with inexact results.** The interpreter runs under `FPUFPCR` (round toward zero) but the recompiler
  (`x86/iFPU.cpp:1098-1164`) switches to `FPUDivFPCR` around divisions, whose default is round to nearest (`Pcsx2Config.cpp:601`): two
  readings. The model's third reading for rsqrt (the square root truncated to a float first, then divided: it depends on which
  overload of `sqrt` FPU.cpp resolves to). A case where the port differs from the interpreter but agrees with one of the other readings
  is counted as ambiguous (196, all rsqrt.s); none in div.s or sqrt.s, whose port output equals the interpreter's for normal operands.
- **cfc1 and ctc1 with FCR31 bits outside 0x0083C078 and cfc1 of a register other than 31.** The interpreter returns all 32 bits,
  `0x2E00` for FCR0 and 0 for the others; the recompiler (`iFPU.cpp:104-124`) masks with 0x0083C078 and sets 0x01000001, and returns
  FCR0 for fs below 16. The port writes `& 0x0183FFFF`. All such cases (10 of cfc1, 18 of ctc1) are marked ambiguous; the
  legal-bit cases agree. The one ctc1 in the game is at one site; whether it writes anything but legal bits is not measured here.
- **Underflow and overflow flags.** With host flush-to-zero the interpreter's checks never fire (see "How the model rounds"), so U and O
  are never set by arithmetic in the model; real hardware sets them. Nothing reads them.

### Sensitivity check
Model changed on purpose: the product rounded to nearest instead of truncated fails mul.s (78 extra cases with only normal operands) and
mula.s (68); `c.lt` taken as `<=` fails c.lt.s on 162 more cases of zero, denormal and normal operands. Restored (byte-identical).

### Patches and what is in the series
0047, 0048, 0049, 0050 are not in `series` (the series is changed only after a regeneration and a regression run, as 0043-0046).
`KNOWN_DEFECTS` in `test_conformance.py` points at them; when one enters the series and the recompiler is rebuilt, `xfail(strict)` turns
into a failure for cvt.w.s, min.s and max.s and the entry is deleted (div.s and sqrt.s stay failing because of item 5). Without the
patches: `175 passed, 67 xfailed`.

### Left
VU0 (item 2 of the list). Decision pending for the owner: clamp exponent-255 operands (item 5) or accept it.

## Follow-up 2026-10-13 (T-210 piece 6): the vector unit 0 instructions run inline (family I, `vu0`)

**What was run.** `tests/conformance`, family `vu0`: 105 instruction names, about 129,000 cases, in 8 to 12 forms each (every destination
mask the game uses plus xyzw, xyz-style single lanes and the non-contiguous xz/yw; the shapes fd=fs, fd=ft, fs=ft and fd=VF0; the full
50 x 50 operand-pair set at xyzw for the instructions the game uses, a 14 x 14 set elsewhere; one broadcast value per case with four
different fs lanes). All four lanes of every register differ. The state of a case grew from 71 to 132 pairs: VF0..31, VI0..15, ACC, Q, P, I,
R, the status, MAC and clip flags, TPC, CMSAR0, FBRST, VPU_STAT, CMSAR1. Reference: `vu_ref.py` (VUops.cpp, VUflags.cpp, VU0.cpp, the
function codes from R5900OpcodeTables.cpp, each operation cited); the arithmetic is `fpu_ref`'s (exact, truncated, clamp of the unit).
Also run, although the game has no site: vftoi, vitof, vdiv, vrsqrt, vnop. Not run: vclip, the integer operations, vmfir, vmtir, vr*
(no site; the model has them, the forms do not), vlqi/vsqi/vlqd/vsqd (they address the unit's data memory, which is not in the harness
state; the game has 1 site each, at 0x102e84 and 0x102e5c) and microprograms.

**Flags.** The port computes no MAC, status or clip flags. The game reads them at one site each (a context save, `cfc2`) and nothing
consumes them; there is no `vclip` and the `bc2` branches are not in the census. Those registers are not compared by the arithmetic forms
(`Form.ignore`); the cases that differ only there are counted apart as "flags nobody reads".

**Result (shipping recompiler, no patch).** Equal: vnop, vitof0/4/12/15, qmfc2, sqc2, cfc2, ctc2 (9 of 105 pass). With
0051-0053 built in, the only differences left are:

| Instruction | Generated statement or macro | Console | Sites in the game | Reaches a fight's 3D path? |
|---|---|---|---|---|
| every arithmetic form, vopmula, vopmsub, vsqrt, with an operand whose exponent is all ones | `_mm_add_ps` and the like on the raw operand | the operand is the largest finite number (vuDouble) | all | as piece 5: the pattern is what a division by zero or an overflow leaves; not measured; left unpatched (`NO_CLAMP` decision) |
| vftoi0/4/12/15 | `_mm_cvttps_epi32` (a value at or above 2^31 gives 0x80000000) | saturates to 0x7FFFFFFF | 0 | no site |
| vdiv, vrsqrt | quotient 0 when the divisor is zero | the largest number of the sign, status D / I flags | 0 | no site |

Fixed by the patches (not in `series`): VF0 written (0051), vmax/vmini on bit patterns (0052), vsqrt of a negative number (0053).

### Differences (before the patches)
1. **VF0 is writable** (0051). `qmtc2 rt, vf0` emits `ctx->vu0_vf[0] = ...`, `lqc2 vf0` the same, and vmove / vmr32 / vabs / vftoi / vitof
   and every Special1 operation with fd = 0 write the destination whatever it is. The console drops all of them. Sites: the census finds
   one write to VF0 in the executable, an `lqc2` at 0x102cbc (a restore of a saved VF0); no arithmetic site has fd = 0 and no qmtc2
   has rd = 0. A saved VF0 written by this runtime is (0,0,0,1), so the effect is nil unless a save holds anything else.
2. **vmax / vmini** (0052). `_mm_max_ps(a, b)` returns the second operand when they compare equal and flushes denormals: max(+0, -0)
   gave -0, min(-0, +0) gave +0, and a denormal pattern counted as zero. The console orders the bit patterns as signed integers (as
   `max.s`, patch 0049). Sites: vmax 87 and vmini 87 (mask 0x3), vmaxx 1, vminix 1; reached only with a signed zero or a denormal operand.
3. **vsqrt** (0053). `sqrtf(max(0, ft))` gives 0 for a negative operand; the console takes `sqrt(|ft|)` and sets the status sign flag. Site: 1 (0x1b805c).
   Whether a fight's 3D path takes a negative operand there was not measured.
4. **vftoi, vdiv, vrsqrt**: not patched (no site).

### Reference ambiguous (counted apart, never as a failure)
cfc2 of TPC (the port returns `tpc >> 3`, the reference the stored value; no program runs, TPC is read-only; 1 site); ctc2 of R (the port stores
the raw 32 bits in all four lanes, the reference `v & 0x7FFFFF | 0x3F800000`; only vrget / vrnext / vrxor could tell, and the game has none, while
cfc2 R returns the same masked value in both); values wider than the console's register for the integer registers (16 bits), status (12), clip (24),
CMSAR0 (16) and FBRST's reset bits; unaligned lqc2 / sqc2 addresses (the reference rounds down, the console faults). cfc2 and ctc2 on the other
registers match: the sign extension of cfc2 into the upper word, the untouched upper 64 bits of the destination, the read-only MAC / TPC / VPU_STAT.

### vcallms and vcallmsr
Not modelled. Microprograms: `vcallms` and `vcallmsr` (1 site, 0x102b14) and `ctc2` to CMSAR1 (1 site) run a program of the vector unit; the
port emits a statement for vcallms(r) (the function at 0x102B08 is translated with an out-of-range register index, journal 013). To test them
the harness would need a micro-memory image and an interpreter for the micro instruction set (the reference's VU0microInterp) plus the unit's data
memory; none of that is in the state.

### Sensitivity check
Model changed on purpose: the mask bits of the destination read in the wrong order for `vsub` at mask 0xA fails vsub (29 extra cases with only normal
operands) and every other sub form that has that mask; the broadcast `x` taken from lane y for `vmulx` fails vmulx (1,960 extra) and vmulax (1,700).
Restored (byte-identical, sha1 672fe8fa...).

### Patches and what is in the series
0051 (VF0 never written), 0052 (vmax/vmini on bit patterns), 0053 (vsqrt) are not in `series`.
`KNOWN_DEFECTS` points at them; when one enters the series and the recompiler is rebuilt, `xfail(strict)` turns into a failure for vabs, vmove, vmr32,
lqc2, qmtc2 (0051) and the twelve vmax / vmini names (0052) and the entries are deleted. Without the patches: `185 passed, 163 xfailed`
(`tests/conformance`, 5 min 13 s).

## Closing: the six families (T-210 pieces 1-6)

**Totals** (`docs/knowledge/data/instruction_conformance.tsv`): 331 instructions and about 328,000 cases; 163 instructions do not match
the reference with the shipping recompiler (96 vu0, 42 mmi, 19 fpu, 2 each in compare, branches, memory). With patches 0043-0053 built in, what is left differs only for operands
with the exponent all ones (fpu, vu0) or is an instruction with no site in the game (mmi, vftoi, vdiv, vrsqrt).

**Patch files, in the order they should enter the series** (all are in `patches/ps2recomp/`; none of 0043-0053 is in `series`):
0043 likely-link branches, 0044 movz/movn, 0045 lq/sq alignment, 0046 shift-amount register in bytes, 0047 div.s by zero, 0048 cvt.w.s,
0049 min/max.s (adds the helpers 0052 imitates), 0050 sqrt.s of zero, 0051 VF0, 0052 vmax/vmini, 0053 vsqrt. 0048, 0049 and 0052 add helpers to the same header, `ps2_runtime_macros.h`: apply them in this order. Each one must pass a regeneration and a
regression run before it enters the series.

**What the suite does not cover.** Interrupts, exceptions (only the overflow ones the generated code raises, counted apart), the TLB and the cache
instructions, the COP0 registers, the GS / VIF / DMA registers, the instruction timing, every instruction of the vector unit's micro set (the
suite runs only the macro-mode ones), the unit's data memory (vlqi, vsqi, vlqd, vsqd), microprograms, the MAC / status / clip flags of the
arithmetic (no site reads them), the exponent-all-ones operands once patched (a decision of the owner), and any instruction sequence (each case is one instruction; interactions through the accumulator or the flags are not run).

## Follow-up 2026-10-13 (T-210 closing): regeneration checklist for patches 0043-0053

**Order.** Apply on top of everything in `patches/ps2recomp/series`: 0043, 0044, 0045, 0046, 0047, 0048, 0049, 0050, 0051, 0052, 0053. Checked in a
scratch worktree (`git -C third_party/PS2Recomp worktree add --detach work/conf/PS2Recomp-stack HEAD`, series applied and committed as base, then
the eleven with `git apply`, one commit each). One conflict: **0046** did not apply after 0044 (its `special_translator.cpp` hunk had the old
movz/movn lines as context; 0044 rewrites them). Its diff was regenerated on top of 0044, the header kept and a line added. The other ten apply
as they were. Applying the eleven files from the series state gives a tree identical to the one that was built.

**What each one needs rebuilt.** "Text" = it changes what the recompiler emits (rebuild `ps2_recomp` and regenerate). "Header" = it changes
`ps2_runtime_macros.h`, which the generated code includes (rebuild what includes it; the generated text does not change for a header-only patch).

| Patch | Files | Text | Header |
|---|---|---|---|
| 0043 likely-link branches | control_flow_emitter.cpp | yes | no |
| 0044 movz/movn | special_translator.cpp | yes | no |
| 0045 lq/sq alignment | instruction_translator.cpp | yes | no |
| 0046 SA in bytes | special_translator.cpp, regimm_translator.cpp, mmi_translation_helpers.cpp | yes | no |
| 0047 div.s by zero | fpu_translator.cpp | yes | no |
| 0048 cvt.w.s | ps2_runtime_macros.h (the `FPU_CVT_W_S` macro) | no | yes |
| 0049 min.s / max.s | fpu_translator.cpp, ps2_runtime_macros.h (`ps2_fpu_max_bits`, `ps2_fpu_min_bits`) | yes | yes |
| 0050 sqrt.s of zero | fpu_translator.cpp | yes | no |
| 0051 VF0 | vu_translator.cpp, instruction_translator.cpp | yes | no |
| 0052 vmax / vmini | vu_translation_helpers.cpp, ps2_runtime_macros.h (`ps2_vu_max_ps`, `ps2_vu_min_ps`) | yes | yes |
| 0053 vsqrt | vu_translation_helpers.cpp | yes | no |

Only 0048 is header-only; 0049 and 0052 need both. 0048, 0049 and 0052 add their helpers to the same header. The regeneration therefore has to
rebuild the recompiler (ten patches change its text) and recompile the game against the new header.

**The check (two commands, from the repository root).**
1. Shipping state, the recompiler built from the committed series:
   `uv run --no-project --with rabbitizer --with pytest python -I -m pytest tests/conformance -q`
   must print `185 passed, 163 xfailed`.
2. After the regeneration, with the recompiler and runtime headers rebuilt from the tree that has the eleven, and a list of patches that
   names them (the series plus the eleven):
   `DON_RECOMP=<ps2_recomp built from that tree> DON_RT_DIR=<that tree> DON_CONF_SERIES=<the list> uv run --no-project --with rabbitizer --with pytest python -I -m pytest tests/conformance -q`
   must print `214 passed, 1 skipped, 133 xfailed` (the skipped test is the table, which records the shipping state). Measured here
   with the recompiler built from `work/conf/PS2Recomp-stack` and a list of the series plus the eleven. Once the eleven are in `series` and the table has been regenerated
   (`make_table.py`), the first command alone prints `215 passed, 133 xfailed`.

**How the expected failures retire themselves.** `KNOWN_DEFECTS` in `test_conformance.py` maps an instruction to (the patch files that fix it,
what is left when they are in force or none). A failure is expected when something is left or when one of its patches is not in the list
(`DON_CONF_SERIES`, default `patches/ps2recomp/series`). An instruction whose patches are all in the list and which has nothing left must pass;
if it fails, the test fails with "the recompiler was not rebuilt with them". An instruction that passes while one of its patches is missing from
the list fails as an unexpected pass (`xfail(strict)`). When a patch enters `series` nothing has to be edited by hand.

**Result with the eleven (recompiler built from the stacked tree, list = series + eleven): 214 passed, 1 skipped, 133 xfailed; no unexpected pass, no failure.**
- No instruction that passed in the shipping state fails with the eleven (no regression between patches).
- 30 flip to pass: 0043 bltzall bgezall; 0044 movz movn; 0045 lq sq; 0046 mtsa mtsab mtsah qfsrv; 0048 cvt.w.s; 0049 min.s max.s; 0051 vabs vmove vmr32
  lqc2 qmtc2.ni; 0052 vmax vmaxx vmaxy vmaxz vmaxw vmaxi vmini vminix vminiy vminiz vminiw vminii.
- 133 still fail, with the reason: 45 have no site in this game (38 multimedia, rsqrt.s, vftoi0/4/12/15, vdiv, vrsqrt); 88 differ only for an operand with the
  exponent field all ones (15 of the floating-point coprocessor, 73 of the vector unit; div.s, sqrt.s and vsqrt are among them, with 0047, 0050 and 0053 in force: what
  is left in them is that operand). The flags nobody reads and the cases where the reference is ambiguous are counted apart and never fail.

**Not run, sites in the game (census of the executable).** vclip 0; integer operations (viadd, viaddi, viand, vior, visub, vilwr, viswr) 0; vlqi 1; vsqi 1;
vlqd 0; vsqd 0; vmfir 0; vmtir 0; vrget, vrnext, vrinit, vrxor 0; vcallms 0; vcallmsr 1; vwaitq 1.

**Correction 2026-10-13 (T-210, knowledge file 21).** The closing section above ("Closing: the six families") first said that the
instructions with no site in the game were "mmi: 41". That number was never measured and the sentence was rewritten during piece 6 without
a note. Measured from `docs/knowledge/data/instruction_conformance.tsv`: of the 42 multimedia instructions that differ, 4 have sites in the
game (qfsrv, mtsab, mtsah, mtsa) and **38** have none. The 38 are listed in `docs/knowledge/21-instruction-conformance.md`.

## Follow-up 2026-10-13 (T-210): the regeneration with patches 0043-0053

**What was run (shared tree, in this order).** The eleven patch names appended to `patches/ps2recomp/series` (after 0054);
`bash tools/setup/fetch_ps2recomp.sh --reset` (the whole series applied from clean, 0041 and 0054 included);
`bash tools/pipeline/build_tools.sh` (13 s); `bash tools/pipeline/generate.sh` (6 min 15 s; `sync: 0 added, 8213 changed, 0 removed, 49511 left alone`;
audit exit code 0); `DON_JOBS=8 bash tools/pipeline/build.sh linux-dev` (6 min 01 s, peak 6,676,762,624 bytes; 1947 build steps). The binary
before the regeneration is `build/linux-dev/don.pre-regen` (identical sha1 prefix b77c359e to the `don` it was copied from).
Side finding: running `fetch_ps2recomp.sh` again *without* `--reset` prints `DOES NOT APPLY: 0002-fpu-sqrt-rsqrt-operands-pr277.patch`, because that
script tests each patch alone against the already patched tree and 0049/0050 changed the same lines; with `--reset` the series applies in order.

**Suites.**
- `tests/conformance` (plain command, no variables): `215 passed, 133 xfailed in 320.85s`. The figure derived earlier was right. The table
  was regenerated first (`make_table.py`; it had to be fixed: it printed the `KNOWN_DEFECTS` values, which are tuples since the series mechanism, now
  `test_conformance._expectation`). The table now records: equal 168 -> 168+30 = 198 of 331 (fpu 18 of 34, mmi 56 of 94, vu0 26 of 105,
  branches, compare and memory all equal).
- `tests/runtime`: `1 failed, 21 passed`; the failure is `unwind_not_return_test` (`k=7: finished=0 overrun=1 ... native=1001`), the one the orchestrator announced for the
  not-yet-integrated patch 0055; nothing else fails.
- `tests/host tests/vuglue tests/input`: `25 passed in 64.61s`.

**The game (headless, software renderer, one run at a time, new binary).**
- a. `work/control/ctl.txt`, Japanese, movies on, shots every 60: exit 0; 84 `sNNNN` shots (6680-7510), all different from each other; 217 shots in all.
- b. Idle title, English voices folder, dialog answered (LEFT at 270, CIRCLE at 330), Start at 6000, quit at 7300: exit 0; the 52 shots from 4200 to 7260
  all differ from the previous one; the shot at 7260 is the main menu (BATTLE STADIUM MODE! highlighted).
- c. `--sound on`, `DON_PLAY_MOVIES=1`, `to_match_1v1.txt`, Japanese: exit 0 at blank 7800, 24,960,960 bytes of WAV; of the 91 shots from 2400 on, two consecutive
  pairs are identical (3240 and 4620), as the first time.

**Old against new, without sound (`to_match_1v1.txt`, `DON_PLAY_MOVIES=1`, shot every 60, 142 shots each): all 142 are byte-identical (`cmp`).** The same
for route a (ctl.txt): 217 of 217 byte-identical between `don.pre-regen` and the new binary. Nothing differs on screen in any of the 359 shot pairs.
Against the console's replays (`work/gsdump/*-replay.png`, `image_parity.py`), old and new give the same numbers: title (frame 2700) SSIM 0.9592 / MAD 5.33;
main menu (`main_menu`, frame 3060) 0.9102 / 9.65; character select (`charselect`) 0.8329 / 14.27 (it fails the 0.90 threshold as before; journal 013).

**Determinism.** `DON_TRACE_STATE=60` on `work/det/quit70.txt`, twice with the new binary: the `state blank=60` lines are identical
(`eeCycle=334387800 iopCycles=40651185 iopInstructions=36027780 ramHash=ca5662b5aee5b6b2`).

**Something to know.** The old binary's line at blank 60 differs only in the RAM hash: `ramHash=d90637f64c5726b2` (also stable on two runs); the cycle counts are
equal. So guest memory differs by blank 60 though no shot differs. Not investigated (no bisecting was asked). A candidate to check first: the
interrupt context save writes SA with mfsa (0046 changed the unit from bits to bytes); other candidates are the 128-bit register halves moved by movz/movn (0044).

**Not verified.** That any fight uses an input the eleven patches fix (the 3D and fight shots are byte-identical in these routes); behaviour in modes
other than the two routes; the sound's content (only that the run ended and wrote a WAV).

## Follow-up 2026-10-14 (T-210): the memory image at blank 60, and the idempotence check of the fetch script

**The memory image.** `DON_TRACE_STATE=60 DON_STATE_DUMP=<file> DON_STATE_DUMP_AT=60` on `work/det/quit70.txt` (headless, software), with
`build/linux-dev/don.pre-regen` and with the present binary; the 32 MiB images compared word by word (a few lines of Python in `work/regen/ramcmp.py`).
Result: **4 words of 8,388,608 differ**, all in one range, `0x01f7f608-0x01f7f61f` (the top of RAM, a stack). They are the upper halves of two
consecutive 128-bit quadwords: `0x01f7f600` and `0x01f7f610` hold the bytes `data.afs\0` in their low 64 bits (identical in both binaries) and, in
the upper 64 bits, `0x0001010101010101` before the regeneration and `0` now. The first candidate (SA, eight times apart) is **wrong**: no word
is a multiple of another and the cycle counts and the code are equal. What the values say: a 128-bit register whose low half is the file name
was saved with a stale upper half; the old binary's upper half is a byte-wise comparison result (`0x01` per byte), which looks like another
register's upper half copied whole. The patch that stops copying upper halves is 0044 (movz / movn move the low 64 bits); that is the
likely cause, **not proved** (no run with 0044 alone; no bisecting was asked). The stack slot is not read by any code that changes a picture:
the 359 pictures of the earlier follow-up are byte-identical.

**What a correct check in `tools/setup/fetch_ps2recomp.sh` would be** (to give as a small task; the script is not changed here):
1. Do not test each patch alone against the final tree: later patches rewrite the same lines (0049/0050 against 0002), so "does not apply" is the
   expected answer for an earlier patch of a fully patched tree.
2. Decide "already applied" for the whole series at once: keep a marker (the hash of `series` plus the pinned commit) in the tree after a successful
   run, or check with `git apply --check` in reverse of the *whole series in reverse order* (last patch first), which succeeds exactly on a fully patched tree.
3. If the marker or the reverse check fails, say "the tree is not at the series; run with --reset" instead of printing one line per patch.
