#!/usr/bin/env python3
"""Assemble the recompiler configuration (work/don.toml) and the filtered function map.

Usage:
  make_config.py [--csv config/functions.ghidra.csv] [--layout config/layout.toml]
                 [--policy config/stub_policy.toml] [--extra config/extra_entry_points.txt]
                 [--manual-extra config/manual_entry_points.txt]
                 [--patches config/instruction_patches.toml]
                 [--analyzer-config work/analyzer/config.toml] [--elf disc/SLPS_256.75]
                 [--out work/don.toml] [--filtered-csv work/functions.filtered.csv]
                 [--generated-dir generated]

Combines the Ghidra function map, the layout, the stub policy and the local output of the
analyzer into the TOML that ps2xRecomp reads (ConfigManager::loadConfig). Paths written in the
output are absolute, so the output is local and is not versioned. Nothing is written into
generated/ and the recompiler is not run. Prints a summary of what was kept and dropped.
"""
import argparse
import csv
import sys
import tomllib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ANALYZER_CMD = (
    "third_party/PS2Recomp/out/build-tools/ps2xAnalyzer/ps2_analyzer "
    "disc/SLPS_256.75 work/analyzer/config.toml"
)
CODE_KIND = "code"
CSV_HEADER = ["Name", "Start", "End", "Size"]


def parse_int(text):
    return int(str(text).strip(), 0)


def split_named(entry):
    """'name@0xADDR' -> (name, address)."""
    name, _, addr = entry.rpartition("@")
    if not name:
        raise ValueError(f"expected name@0xADDRESS, got {entry!r}")
    return name, parse_int(addr)


def fmt_named(name, addr):
    return f"{name}@0x{addr:08X}"


# ---------------------------------------------------------------- inputs

def read_layout(path):
    d = tomllib.loads(Path(path).read_text())
    text = (d["text"]["start"], d["text"]["end"])
    islands = [(i["start"], i["end"], i["kind"]) for i in d.get("island", [])]
    return text, islands


def read_policy(path):
    d = tomllib.loads(Path(path).read_text())
    return d.get("default", "recompile"), dict(d.get("policy", {}))


def read_extra_entry_points(path):
    """Addresses of the file: one per line, '#' starts a comment."""
    p = Path(path)
    if not p.exists():
        return []
    out = []
    for line in p.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.append(parse_int(line.split()[0]))
    return out


def read_native_overrides(path):
    """Addresses (as '0x%08x' strings) of config/native_overrides.txt: the functions the port replaces at run time."""
    return [f"0x{a:08x}" for a in sorted(set(read_extra_entry_points(path)))]


def read_patches(path):
    p = Path(path)
    if not p.exists():
        return []
    d = tomllib.loads(p.read_text())
    return [(parse_int(i["address"]), i["value"]) for i in d.get("instructions", [])]


def read_function_rows(path):
    with open(path, newline="") as f:
        return [(r["Name"], parse_int(r["Start"]), parse_int(r["End"]), r["Size"])
                for r in csv.DictReader(f)]


# ---------------------------------------------------------------- transformations

def filter_function_map(rows, text, islands):
    """Keep rows whose whole range [start, end) is in the main text or in a `code` island.

    rows: (name, start, end, size). Returns (kept, dropped) where dropped counts rows per
    island ("start-end kind" keys, with the rows that start inside it) and rows that fit nowhere.
    """
    ranges = [text] + [(s, e) for s, e, k in islands if k == CODE_KIND]
    kept = []
    dropped = Counter()
    for row in rows:
        _, start, end, _ = row
        if any(s <= start and end <= e for s, e in ranges):
            kept.append(row)
            continue
        for s, e, k in islands:
            if s <= start < e:
                dropped[f"island 0x{s:08X}-0x{e:08X} ({k})"] += 1
                break
        else:
            dropped["outside text and islands"] += 1
    return kept, dropped


def filter_stubs(stubs, default, policy):
    """Keep the stubs the policy marks `stub`. `review` and unlisted names use the default.

    Returns (kept, dropped, counts) with counts: effective decision -> number of entries.
    """
    kept, dropped, counts = [], [], Counter()
    for entry in stubs:
        name, _ = split_named(entry)
        decision = policy.get(name, default)
        if decision not in ("stub", "recompile"):
            decision = default if default in ("stub", "recompile") else "recompile"
        counts[decision] += 1
        (kept if decision == "stub" else dropped).append(entry)
    return kept, dropped, counts


def stubs_matching_other_rows(stubs, rows):
    """Stubs whose name equals a function-map row at a different address."""
    by_name = {}
    for name, start, _, _ in rows:
        by_name.setdefault(name, []).append(start)
    out = []
    for entry in stubs:
        name, addr = split_named(entry)
        others = [a for a in by_name.get(name, []) if a != addr]
        if others:
            out.append((entry, others))
    return out


def in_text(addr, text):
    return text[0] <= addr < text[1]


def filter_by_text(entries, text):
    """Split 'name@0xADDR' entries into (inside, outside) the main text."""
    inside, outside = [], []
    for e in entries:
        (inside if in_text(split_named(e)[1], text) else outside).append(e)
    return inside, outside


LABEL_PREFIXES = ("entry_", "caseD_", "default")


def classify_address(addr, rows):
    """Where an address falls in the function map: ("start", row name) if it is the start of a
    row, ("inside", innermost function row name) if it lies strictly inside a function row,
    ("none", None) otherwise. Labels (entry_, caseD_, default) are not function rows."""
    for name, start, _, _ in rows:
        if start == addr:
            return "start", name
    best = None
    for name, start, end, _ in rows:
        if name.startswith(LABEL_PREFIXES) and name != "entry":
            continue
        if start < addr < end and (best is None or end - start < best[2] - best[1]):
            best = (name, start, end)
    if best:
        return "inside", best[0]
    return "none", None


def merge_entry_points(analyzer_entries, extra_addresses, manual_addresses=()):
    """Merge by address (the analyzer's name wins). Returns (merged, stats)."""
    by_addr = {}
    for e in analyzer_entries:
        by_addr.setdefault(split_named(e)[1], e)
    analyzer_unique = len(by_addr)
    for addr in list(extra_addresses) + list(manual_addresses):
        by_addr.setdefault(addr, fmt_named(f"entry_{addr:08x}", addr))
    merged = [by_addr[a] for a in sorted(by_addr)]
    stats = {
        "analyzer": len(analyzer_entries),
        "analyzer_unique": analyzer_unique,
        "extra": len(extra_addresses),
        "manual": len(manual_addresses),
        "merged": len(merged),
    }
    return merged, stats


def filter_jump_tables(tables, text):
    """Keep tables with at least one entry whose targets all lie in the main text."""
    kept, dropped = [], []
    for t in tables:
        targets = [parse_int(e["target"]) for e in t.get("entries", [])]
        if targets and all(in_text(a, text) for a in targets):
            kept.append(t)
        else:
            dropped.append(t)
    return kept, dropped


def filter_mmio(mmio, text):
    """Keep entries whose instruction address lies in the main text. Sorted by address."""
    kept, dropped = {}, {}
    for k, v in mmio.items():
        (kept if in_text(parse_int(k), text) else dropped)[k] = v
    kept = dict(sorted(kept.items(), key=lambda kv: parse_int(kv[0])))
    return kept, dropped


# ---------------------------------------------------------------- output

def toml_str(s):
    s = str(s)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def toml_bool(b):
    return "true" if b else "false"


def write_toml(general, patches, mmio, jump_tables):
    """Render the configuration. general: dict with the keys of [general]; patches:
    list of (address, value); mmio: {instr: register} (strings); jump_tables: list of
    {address, entries[, base_register]}."""
    out = ["# PS2Recomp configuration for Battle Stadium D.O.N",
           "# Generated by tools/pipeline/make_config.py; local file (absolute paths), do not edit.",
           "",
           "[general]"]
    for key in ("input", "ghidra_output", "output"):
        out.append(f"{key} = {toml_str(general[key])}")
    for key in ("single_file_output", "patch_syscalls", "patch_cop0", "patch_cache"):
        out.append(f"{key} = {toml_bool(general[key])}")
    for key in ("stubs", "entry_points", "skip", "native_overrides"):
        items = general.get(key, [])
        if not items:
            out.append(f"{key} = []")
            continue
        out.append(f"{key} = [")
        out.extend(f"  {toml_str(i)}," for i in items)
        out.append("]")
    out.append("")
    out.append("[patches]")
    if not patches:
        out.append("instructions = []")
    else:
        out.append("instructions = [")
        for a, v in patches:
            out.append(f'  {{ address = "0x{a:x}", value = {toml_str(v)} }},')
        out.append("]")
    out.append("")
    out.append("[mmio]")
    for k, v in mmio.items():
        out.append(f"{toml_str(k)} = {toml_str(v)}")
    out.append("")
    out.append("[jump_tables]")
    for t in jump_tables:
        out.append("[[jump_tables.table]]")
        out.append(f"address = {toml_str(t['address'])}")
        if "base_register" in t:
            br = t["base_register"]
            out.append(f"base_register = {br if isinstance(br, int) else toml_str(br)}")
        out.append("entries = [")
        for e in t["entries"]:
            out.append(f"  {{ index = {int(e['index'])}, target = {toml_str(e['target'])} }},")
        out.append("]")
        out.append("")
    return "\n".join(out).rstrip("\n") + "\n"


def write_filtered_csv(path, rows, original_rows_text):
    """Write the kept rows, with the original Start/End/Size text."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(CSV_HEADER)
        for name, start, end, size in rows:
            w.writerow([name, original_rows_text.get((name, start), f"0x{start:08X}"),
                        f"0x{end:08X}", size])


# ---------------------------------------------------------------- main

def build(args):
    """Run the whole pipeline. Returns (toml_text, kept_rows, summary_lines, report)."""
    analyzer_path = Path(args.analyzer_config)
    if not analyzer_path.exists():
        raise SystemExit(f"error: {analyzer_path} does not exist. Produce it with:\n  {ANALYZER_CMD}")
    analyzer = tomllib.loads(analyzer_path.read_text())

    text, islands = read_layout(args.layout)
    default, policy = read_policy(args.policy)
    rows = read_function_rows(args.csv)
    kept_rows, dropped_rows = filter_function_map(rows, text, islands)

    a_general = analyzer.get("general", {})
    stubs_all = list(a_general.get("stubs", []))
    stubs_policy, stubs_policy_dropped, decisions = filter_stubs(stubs_all, default, policy)
    stubs, stubs_outside = filter_by_text(stubs_policy, text)

    entries_all = list(a_general.get("entry_points", []))
    entries_in, entries_out = filter_by_text(entries_all, text)
    extra_all = read_extra_entry_points(args.extra)
    extra_in = [a for a in extra_all if in_text(a, text)]
    extra_out = [a for a in extra_all if not in_text(a, text)]
    manual_all = read_extra_entry_points(args.manual_extra)
    manual_in = [a for a in manual_all if in_text(a, text)]
    manual_out = [a for a in manual_all if not in_text(a, text)]
    entry_points, ep_stats = merge_entry_points(entries_in, extra_in, manual_in)

    patches = read_patches(args.patches)
    native_overrides = read_native_overrides(args.native_overrides)

    tables = analyzer.get("jump_tables", {}).get("table", [])
    tables_kept, tables_dropped = filter_jump_tables(tables, text)
    mmio_kept, mmio_dropped = filter_mmio(analyzer.get("mmio", {}), text)

    general = {
        "input": str(Path(args.elf).resolve()),
        "ghidra_output": str(Path(args.filtered_csv).resolve()),
        "output": str(Path(args.generated_dir).resolve()) + "/",
        "single_file_output": False,
        "patch_syscalls": False,
        "patch_cop0": True,
        "patch_cache": True,
        "stubs": sorted(stubs, key=lambda e: (split_named(e)[1], e)),
        "entry_points": entry_points,
        "skip": [],
        "native_overrides": native_overrides,
    }
    toml_text = write_toml(general, patches, mmio_kept, tables_kept)

    lines = [
        f"function map: {len(rows)} rows read, {len(kept_rows)} kept, "
        f"{sum(dropped_rows.values())} dropped",
    ]
    for key in sorted(dropped_rows):
        lines.append(f"  dropped {dropped_rows[key]} in {key}")
    lines.append(f"stubs: {len(stubs_all)} read, {len(stubs)} kept")
    lines.append("  policy decisions: " + ", ".join(
        f"{k}={decisions[k]}" for k in sorted(decisions)))
    lines.append(f"  dropped by policy: {len(stubs_policy_dropped)}; "
                 f"dropped outside text: {len(stubs_outside)}")
    for e in stubs_outside:
        lines.append(f"    outside text: {e}")
    lines.append(f"entry points: analyzer {ep_stats['analyzer']} "
                 f"({len(entries_out)} outside text dropped, "
                 f"{len(entries_in)} kept), extra file {ep_stats['extra']} "
                 f"({len(extra_out)} outside text dropped), "
                 f"manual file {len(manual_all)} read "
                 f"({len(manual_out)} outside text dropped), "
                 f"after removing duplicates {ep_stats['merged']}")
    for e in entries_out:
        lines.append(f"    outside text: {e}")
    for a in extra_out:
        lines.append(f"    outside text (extra): 0x{a:08X}")
    for a in manual_out:
        lines.append(f"    outside text (manual): 0x{a:08X}")
    for a in manual_in:
        kind, name = classify_address(a, kept_rows)
        if kind == "start":
            lines.append(f"    manual 0x{a:08X}: start of row {name}")
        elif kind == "inside":
            lines.append(f"    manual 0x{a:08X}: inside function {name}")
        else:
            lines.append(f"WARNING: manual entry point 0x{a:08X} is in no row of the filtered "
                         f"map; add it to config/ghidra_seeds.txt so the map covers it")
    lines.append(f"instruction patches: {len(patches)}")
    lines.append(f"jump tables: {len(tables)} read, {len(tables_kept)} kept, "
                 f"{len(tables_dropped)} dropped")
    for t in tables_dropped:
        lines.append(f"    dropped table at {t.get('address')} (targets outside text or empty)")
    lines.append(f"hardware accesses (mmio): {len(mmio_kept) + len(mmio_dropped)} read, "
                 f"{len(mmio_kept)} kept, {len(mmio_dropped)} dropped")
    for k, v in mmio_dropped.items():
        lines.append(f"    dropped {k} = {v}")

    report = {
        "stub_name_clashes": stubs_matching_other_rows(stubs, kept_rows),
        "entry_points": entry_points,
        "kept_rows": kept_rows,
    }
    return toml_text, kept_rows, lines, report


def parse_args(argv):
    def d(p):
        return str(ROOT / p)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--csv", default=d("config/functions.ghidra.csv"))
    ap.add_argument("--layout", default=d("config/layout.toml"))
    ap.add_argument("--policy", default=d("config/stub_policy.toml"))
    ap.add_argument("--extra", default=d("config/extra_entry_points.txt"))
    ap.add_argument("--manual-extra", default=d("config/manual_entry_points.txt"))
    ap.add_argument("--patches", default=d("config/instruction_patches.toml"))
    ap.add_argument("--native-overrides", default=d("config/native_overrides.txt"))
    ap.add_argument("--analyzer-config", default=d("work/analyzer/config.toml"))
    ap.add_argument("--elf", default=d("disc/SLPS_256.75"))
    ap.add_argument("--out", default=d("work/don.toml"))
    ap.add_argument("--filtered-csv", default=d("work/functions.filtered.csv"))
    ap.add_argument("--generated-dir", default=d("generated"))
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    toml_text, kept_rows, lines, report = build(args)
    tomllib.loads(toml_text)  # the output must parse
    # Keep the original text of Start for each kept row (hex digits as in the source).
    with open(args.csv, newline="") as f:
        start_text = {(r["Name"], parse_int(r["Start"])): r["Start"] for r in csv.DictReader(f)}
    write_filtered_csv(args.filtered_csv, kept_rows, start_text)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(toml_text)
    print("\n".join(lines))
    print(f"wrote {args.filtered_csv} ({len(kept_rows)} rows)")
    print(f"wrote {args.out}")
    clashes = report["stub_name_clashes"]
    print(f"stubs whose name matches a function-map row at another address: {len(clashes)}")
    for entry, others in clashes:
        print(f"    {entry} also at " + ", ".join(f"0x{a:08X}" for a in others))
    starts = {r[1] for r in kept_rows}
    orphans = [e for e in report["entry_points"] if split_named(e)[1] not in starts]
    print(f"entry points that are not the start of a filtered row: {len(orphans)}")
    for e in orphans[:10]:
        print(f"    {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
