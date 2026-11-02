"""Write docs/knowledge/data/instruction_conformance.tsv from a run of the conformance families.

  uv run --no-project --with pytest --with rabbitizer python -I tests/conformance/make_table.py

The "sites in the game" column comes from disc/SLPS_256.75 when it exists; otherwise the numbers already in the
table are kept ("n/a" for a new row).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import census  # noqa: E402
import families  # noqa: E402
import harness  # noqa: E402
import test_conformance  # noqa: E402   (KNOWN_DEFECTS: the note column)

TSV = harness.ROOT / "docs/knowledge/data/instruction_conformance.tsv"
HEADER = ["instruction", "family", "sites_in_game", "cases_run", "equal_to_reference", "smallest_failing_case",
          "overflow_cases_not_run", "note"]


def read_old() -> dict[str, str]:
    if not TSV.is_file():
        return {}
    rows = [line.split("\t") for line in TSV.read_text().splitlines()[1:]]
    return {r[0]: r[2] for r in rows}


def main() -> int:
    elf = harness.ROOT / "disc/SLPS_256.75"
    sites = census.count_sites(elf) if elf.is_file() else None
    old = read_old()
    rows = []
    for fam in sorted(families.FAMILIES):
        results, _ = families.run_family(fam)
        for r in results:
            n = str(sites.get(r.mnemonic, 0)) if sites is not None else old.get(r.mnemonic, "n/a")
            rows.append([r.mnemonic, fam, n, str(r.cases), "no" if r.failures else "yes",
                         r.failures[0] if r.failures else "", str(r.overflow),
                         "; ".join(x for x in (
                             test_conformance._expectation(r.mnemonic) or "",
                             f"{len(r.ambiguous)} cases reference ambiguous" if r.ambiguous else "",
                             f"{len(r.flags_only)} cases differ only in the sticky exception flags (no site reads them)" if r.flags_only else "",
                             "failing classes: " + ", ".join(f"{k}={v}" for k, v in sorted(r.classes.items())) if r.classes and len(r.classes) <= 6 else "")
                             if x)])
    TSV.parent.mkdir(parents=True, exist_ok=True)
    TSV.write_text("\n".join("\t".join(r) for r in [HEADER, *rows]) + "\n")
    print(f"wrote {TSV} ({len(rows)} instructions; sites {'from the disc' if sites is not None else 'kept'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
