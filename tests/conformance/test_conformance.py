"""T-210 part 1: the recompiler's translation of integer instructions against the reference model.

Run: uv run --no-project --with pytest --with rabbitizer python -I -m pytest tests/conformance -q
No game file is needed; the recompiler binary is (DON_RECOMP or third_party/PS2Recomp/out/build-tools).
"""
import os

import pytest

import asm
import families
import harness

rabbitizer = pytest.importorskip("rabbitizer")

_cache = {}


def family(name):
    if name not in _cache:
        try:
            _cache[name] = families.run_family(name)
        except harness.ToolsMissing as e:
            pytest.skip(str(e))
    return _cache[name]


def _mnemonics(name):
    prog = harness.Program(name)
    return sorted({f.mnemonic for f in families.FAMILIES[name](prog)})

# rabbitizer does not know sqrt.s (function 0x04 of the COP1 single format) and names the word "c1"
RABBITIZER_NAME = {"sqrt.s": "c1"}


@pytest.mark.parametrize("fam", sorted(families.FAMILIES))
def test_encoder_matches_rabbitizer(fam):
    """The words the tests assemble decode as the instruction they claim to be."""
    prog = harness.Program(fam)
    forms = families.FAMILIES[fam](prog)
    for f in forms:
        word = prog.funcs[f.func].words[0]
        ins = rabbitizer.Instruction(word, vram=harness.BASE, category=rabbitizer.InstrCategory.R5900)
        assert ins.getOpcodeName() == RABBITIZER_NAME.get(f.mnemonic, f.mnemonic), (f.mnemonic, hex(word))


# Open defects: instruction -> (patch files that fix it, what is left when they are all in force or None).
# What is in force is the list of patches/ps2recomp/series (DON_CONF_SERIES=<file> points at another list, for checking a
# recompiler built with more patches). A failure is expected when the instruction has a residual reason, or when one of its
# patches is not in the list. Otherwise it must pass: if it still fails, the recompiler was not rebuilt with the series.
PD = "patches/ps2recomp/"
P43, P44, P45, P46 = (PD + "0043-likely-link-branches-always-link.patch", PD + "0044-movz-movn-move-low-64-bits.patch",
                      PD + "0045-lq-sq-align-address-down.patch", PD + "0046-shift-amount-register-in-bytes.patch")
P47, P48, P49, P50 = (PD + "0047-div-s-by-zero-is-largest-number.patch", PD + "0048-cvt-w-s-saturates.patch",
                      PD + "0049-min-max-compare-bit-patterns.patch", PD + "0050-sqrt-s-of-zero-keeps-the-sign.patch")
P51, P52, P53 = (PD + "0051-vf0-is-never-written.patch", PD + "0052-vu-max-mini-compare-bit-patterns.patch",
                 PD + "0053-vsqrt-of-absolute-value.patch")
# Multimedia instructions that differ from the reference but have no site in this game: not patched (T-210 piece 4).
NO_SITE = "no site in this game; not patched"
# Floating point (T-210 piece 5). The arithmetic of the port is the host's, so an operand with the exponent all ones (a
# pattern the console reads as a very large number) is not clamped: fixing it would add work to every one of the ~30,000
# arithmetic sites for a pattern a fight hardly meets (the infinity that a division by zero produced is fixed by 0047).
NO_CLAMP = "exponent-all-ones operands are not clamped; not patched (journal 025, piece 5)"
# Vector unit 0 inline (piece 6): the same for its arithmetic; the operations the game does not use are not patched either.
VU_NO_CLAMP = "exponent-all-ones operands are not clamped; not patched (journal 025, pieces 5 and 6)"

KNOWN_DEFECTS = {}


def _defect(names, patches=(), residual=None):
    for m in (names.split() if isinstance(names, str) else names):
        KNOWN_DEFECTS[m] = (tuple(patches), residual)


_defect("bltzall bgezall", [P43])
_defect("movz movn", [P44])
_defect("lq sq", [P45])
_defect("mtsa mtsab mtsah qfsrv", [P46])
_defect("padduh psubuh psllvw psrlvw psravw pinth pinteh pabsw pabsh pexeh prevh prot3w pexch pexcw plzcw "
        "pmaddw pmsubw pmultw pmultuw pmadduw pmaddh pmsubh pmulth phmadh phmsbh pdivw pdivuw pdivbw "
        "pmfhi pmflo pmfhl.lw pmfhl.uw pmfhl.slw pmfhl.lh pmfhl.sh pmthi pmtlo pmthl.lw", residual=NO_SITE)
_defect("adda.s suba.s mula.s madd.s msub.s madda.s msuba.s add.s sub.s mul.s c.eq.s c.lt.s c.le.s", residual=NO_CLAMP)
_defect("cvt.w.s", [P48])
_defect("min.s max.s", [P49])
_defect("div.s", [P47], NO_CLAMP)
_defect("sqrt.s", [P50], NO_CLAMP)
_defect("rsqrt.s", residual=NO_SITE)
_FMAC = [f"v{o}{a}{b}" for o in ("add", "sub", "mul", "madd", "msub") for a in ("", "a") for b in ("", "x", "y", "z", "w", "q", "i")]
_defect(_FMAC + ["vopmula", "vopmsub"], residual=VU_NO_CLAMP)
_defect("vsqrt", [P53], VU_NO_CLAMP)
_defect("vftoi0 vftoi4 vftoi12 vftoi15 vdiv vrsqrt", residual=NO_SITE)
_defect("vmax vmaxx vmaxy vmaxz vmaxw vmaxi vmini vminix vminiy vminiz vminiw vminii", [P52])
_defect("vabs vmove vmr32 lqc2 qmtc2.ni", [P51])


def patches_in_force():
    """The patch file names (as patches/ps2recomp/NNNN-name.patch) the recompiler under test is built with."""
    series = harness.ROOT / os.environ.get("DON_CONF_SERIES", "patches/ps2recomp/series")
    names = [ln.strip() for ln in series.read_text().splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
    return {PD + n for n in names}


def _expectation(m):
    """None (must pass) or the reason a failure is expected."""
    if m not in KNOWN_DEFECTS:
        return None
    patches, residual = KNOWN_DEFECTS[m]
    missing = [p for p in patches if p not in patches_in_force()]
    if residual:
        return residual + (f" (also fixed by {', '.join(missing)} once in force)" if missing else "")
    return ("open defect, fixed by " + ", ".join(missing)) if missing else None


def _params():
    out = []
    for fam in sorted(families.FAMILIES):
        for m in _mnemonics(fam):
            marks = []
            why = _expectation(m)
            if why:
                marks.append(pytest.mark.xfail(strict=True, reason=why))
            out.append(pytest.param(fam, m, id=f"{fam}-{m}", marks=marks))
    return out


@pytest.mark.parametrize("fam,mnemonic", _params())
def test_instruction(fam, mnemonic):
    results, _ = family(fam)
    res = next(r for r in results if r.mnemonic == mnemonic)
    assert res.cases > 0
    if res.failures and mnemonic in KNOWN_DEFECTS and not _expectation(mnemonic):
        pytest.fail(f"{mnemonic}: its patches {KNOWN_DEFECTS[mnemonic][0]} are in force (series) but {len(res.failures)} cases differ: "
                    f"the recompiler was not rebuilt with them. First: {res.failures[0]}", pytrace=False)
    assert not res.failures, f"{mnemonic}: {len(res.failures)} of {res.cases} cases differ; first: {res.failures[0]}"


def test_table_is_current():
    """docs/knowledge/data/instruction_conformance.tsv lists what the families run now (regenerate with make_table.py)."""
    if "DON_CONF_SERIES" in os.environ:
        pytest.skip("the table records the shipping recompiler; DON_CONF_SERIES names another list of patches")
    rows = [line.split("\t") for line in (harness.ROOT / "docs/knowledge/data/instruction_conformance.tsv")
            .read_text().splitlines()[1:]]
    table = {r[0]: (r[1], r[3], r[4]) for r in rows}
    want = {}
    for fam in sorted(families.FAMILIES):
        results, _ = family(fam)
        for r in results:
            want[r.mnemonic] = (fam, str(r.cases), "no" if r.failures else "yes")
    assert table == want
