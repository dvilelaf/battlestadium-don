#!/usr/bin/env python3
"""Audit the translation: summarize the recompiler log and decide whether it is clean.

Usage:
  audit.py [--log work/logs/recomp.log] [--layout config/layout.toml] [--out generated]
           [--exit-code 0] [--json work/reports/translation_audit.json]

Reads the log that ps2_recomp writes, counts what it translated and what it could not, and
writes the summary as JSON. Exits with 1 when an unhandled instruction lies inside the main
text, when a function with unhandled instructions lies in the data area, when the log lacks
the completion line, or when the recompiler exit code is not 0. Nothing is changed in the
configuration or in the generated code.
"""
import argparse
import json
import re
import sys
import tomllib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_JSON = ROOT / "work/reports/translation_audit.json"
COMPLETED_LINE = "Recompilation completed successfully"
ERROR_LINES_SHOWN = 20

RECOMPILING_RE = re.compile(r"recompiling (\d+) functions")
LEVEL_CATEGORY_RE = re.compile(r"^\s*\[(\w+)\]\s+([\w-]+)")
CONTROL_FLOW_RE = re.compile(r"^\s*\[warning\]\s+control-flow")
UNHANDLED = "unhandled-instruction"
FUNCTION_RE = re.compile(r"function=(\S+)")
ADDR_RE = re.compile(r"addr=0x([0-9A-Fa-f]+)")


def read_text_range(path):
    """(start, end) of the main code range in config/layout.toml."""
    d = tomllib.loads(Path(path).read_text())
    return d["text"]["start"], d["text"]["end"]


def audit_log(log_text, text, generated_files=0, recompiler_exit=0):
    """Summarize the log. text: (start, end) of the main code range; end is exclusive."""
    text_start, text_end = text
    functions = None
    completed = False
    counts = Counter()
    unhandled_total = 0
    unhandled_in_text = 0
    data_functions = set()
    control_flow = 0
    for line in log_text.splitlines():
        if functions is None:
            m = RECOMPILING_RE.search(line)
            if m:
                functions = int(m.group(1))
        if line.strip() == COMPLETED_LINE:
            completed = True
        m = LEVEL_CATEGORY_RE.match(line)
        if m:
            counts[f"{m.group(1)} {m.group(2)}"] += 1
        if CONTROL_FLOW_RE.match(line):
            control_flow += 1
        if UNHANDLED not in line:
            continue
        unhandled_total += 1
        a = ADDR_RE.search(line)
        if a is None:
            continue
        addr = int(a.group(1), 16)
        if text_start <= addr < text_end:
            unhandled_in_text += 1
        elif addr >= text_end:
            f = FUNCTION_RE.search(line)
            if f:
                data_functions.add(f.group(1))
    return {
        "functions": functions,
        "generated_files": generated_files,
        "unhandled_total": unhandled_total,
        "unhandled_in_text": unhandled_in_text,
        "functions_in_data": sorted(data_functions),
        "control_flow_warnings": control_flow,
        "log_counts": dict(sorted(counts.items())),
        "recompiler_exit": recompiler_exit,
        "completed": completed,
    }


def failure_reasons(report):
    reasons = []
    if report["unhandled_in_text"]:
        reasons.append(f"{report['unhandled_in_text']} unhandled instruction(s) inside the text")
    if report["functions_in_data"]:
        reasons.append(f"{len(report['functions_in_data'])} function(s) with unhandled "
                       "instructions in the data area")
    if not report["completed"]:
        reasons.append("the log does not contain the completion line")
    if report["recompiler_exit"] != 0:
        reasons.append(f"recompiler exit code {report['recompiler_exit']}")
    return reasons


def error_lines(log_text):
    return [line for line in log_text.splitlines() if "[error]" in line][:ERROR_LINES_SHOWN]


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", default=str(ROOT / "work/logs/recomp.log"))
    ap.add_argument("--layout", default=str(ROOT / "config/layout.toml"))
    ap.add_argument("--out", default=str(ROOT / "generated"))
    ap.add_argument("--exit-code", type=int, default=0)
    ap.add_argument("--json", default=str(DEFAULT_JSON))
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    log_text = Path(args.log).read_text(errors="replace")
    out = Path(args.out)
    generated = len(list(out.rglob("*.cpp"))) if out.is_dir() else 0
    report = audit_log(log_text, read_text_range(args.layout), generated, args.exit_code)

    json_path = Path(args.json)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

    reasons = failure_reasons(report)
    if not reasons:
        return 0
    print("audit: FAILED: " + "; ".join(reasons))
    for line in error_lines(log_text):
        print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
