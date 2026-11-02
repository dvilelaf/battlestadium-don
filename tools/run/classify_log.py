#!/usr/bin/env python3
"""Classify the log of a smoke run of the port (tools/run/smoke.sh).

Usage: classify_log.py <log>

Prints `RESULT: <category> <detail>`, then a short summary (boot milestones, counters, the
most frequent warning and error lines), and writes `<log>.json` next to the log with the
category and the last 30 lines. Exits with 0 only for OK_VSYNC_LOOP.

Categories, in order of priority (the runtime text each one matches is in PATTERNS below):
MISSING_FUNCTION, MISSING_BRANCH_TARGET, UNIMPLEMENTED_SYSCALL, IOP_UNHANDLED, IOP_ERROR,
STARTUP_FAILED, CRASH, then OK_VSYNC_LOOP, FRAME_LOOP_UNPACED, NO_PROGRESS.

Patterns are searched anywhere in the text, not at the start of a line: the runtime writes
stdout and stderr to the same pipe without line discipline, so lines can run together.
"""
import csv
import json
import re
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
FUNCTIONS_CSV = REPO / "config" / "functions.ghidra.csv"

FRAME_THRESHOLD = 300
VSYNC_THRESHOLD = 300

# Source of each text: third_party/PS2Recomp/ps2xRuntime/src/ (lines of the pinned commit).
PATTERNS = {
    # lib/ps2_runtime.cpp:1151
    "missing_function": re.compile(r"No exact recompiled function for guest PC 0x([0-9a-fA-F]+)"),
    # lib/ps2_runtime.cpp:1279 (reported by PS2Runtime::reportMissingFunction)
    "missing_branch": re.compile(r"\[guest-branch:missing-target\][^\n]*?target=0x([0-9a-fA-F]+)"),
    # lib/Kernel/Syscalls/System.cpp:349
    "unimplemented": re.compile(r"Unimplemented PS2 syscall called\.[^\n]*"),
    # lib/Kernel/Syscalls/System.cpp:329
    "syscall_todo": re.compile(r"\[Syscall TODO\][^\n]*"),
    # lib/Kernel/Syscalls/System.cpp:380
    "unknown_syscall": re.compile(r"Unknown syscallId=0x([0-9a-fA-F]+)"),
    # lib/Kernel/Syscalls/RPC.cpp:129
    "iop_unhandled": re.compile(r"\[IOP/RPC trace:unhandled\][^\n]*"),
    # lib/ps2_iop_host.cpp:529
    "iop_error": re.compile(r"\[ps2xIOP:error\][^\n]*"),
    # lib/ps2_runtime.cpp:722-754, 767-835; main.cpp:225; src/game/don_overrides.cpp
    "startup_failed": re.compile(
        r"(Failed to load ELF file|Failed to initialize PS2 (?:memory|runtime)|"
        r"Failed to bind runtime core subsystems|Failed to open ELF file|Invalid ELF magic|"
        r"Unsupported ELF|\[don\] could not (?:replace|install)[^\n]*)"
    ),
    # main.cpp:99-119 and :240-244; the shell and the C runtime
    "crash": re.compile(
        r"(Segmentation fault|Aborted|terminate called[^\n]*|\[terminate\][^\n]*|"
        r"\[main\] fatal exception[^\n]*|Bus error|Illegal instruction|"
        r"Floating point exception|AddressSanitizer[^\n]*)"
    ),
    "frame": re.compile(r"\[don\] frame (\d+)"),
    "vsync": re.compile(r"\[don\] vsync (\d+)"),
    # written by smoke.sh when the run ends
    "smoke_status": re.compile(r"\[smoke\] exit status (\d+)"),
}

SIGNALS = {132: "SIGILL", 133: "SIGTRAP", 134: "SIGABRT", 135: "SIGBUS", 136: "SIGFPE",
           139: "SIGSEGV"}

# Boot milestones from docs/knowledge/16-boot-sequence.md that show up in a log.
MILESTONES = [
    ("ELF loaded", re.compile(r"ELF file loaded successfully")),
    ("don overrides applied", re.compile(r"\[don\] override applied")),
    ("IOP reset (sceSifRebootIop)", re.compile(r"sceSifRebootIop|IOP reset|reboot-iop", re.I)),
    ("sceCdSearchFile ok", re.compile(r"\[sceCdSearchFile:ok\]")),
    ("sceCdRead", re.compile(r"\[sceCdRead\]")),
    ("module loaded by the game", re.compile(r"\[SIF module\] load-buffer")),
    ("IOP module log", re.compile(r"\[ps2xIOP\]")),
    ("thread created", re.compile(r"CreateThread|thread created", re.I)),
    ("sceSifBindRpc/CallRpc", re.compile(r"\[SifCallRpc\]|\[SifRegisterRpc\]")),
    ("GS vsync callback set", re.compile(r"\[sceGsSyncVCallback:set\]")),
    ("sceDmaSend", re.compile(r"\[sceDmaSend")),
    ("game text output ([don-tty])", re.compile(r"\[don-tty\]")),
    ("frame loop counter", re.compile(r"\[don\] frame \d+")),
    ("vsync counter", re.compile(r"\[don\] vsync \d+")),
]
# The modules of docs/knowledge/16-boot-sequence.md section 2, in load order.
MODULES = ["sio2man", "mtapman", "mcman", "mcserv", "padman", "cdvdstm", "libsd", "modhsyn",
           "modmidi", "modmsin", "sdrdrv", "cri_adxi", "sndfi"]
FILES = ["IOPRP300.IMG", "0000.fpk", "1000.fpk", "dir.dir", "movie.afs", "system.dat",
         "battle.dat", "fpack/cmn/0000", "OP.sfd"]

# A progress line begins with one of these prefixes and the bracket closes right after it. The prefix
# match is exact on purpose: [don-ram] (DON_DUMP_RAM) and [don-gs] (DON_TRACE_GS_REGS) are diagnostics
# that are printed after or during a run, not progress of the game, and a log that ends with them must
# keep reporting the last real progress line.
PROGRESS_LINE = re.compile(r"\[(?:don|don-tty|SIF module|sceCdSearchFile:ok|sceCdRead|SifCallRpc|"
                           r"ps2xIOP|sceDmaSend|sceGsSyncVCallback:set)\]")
NOISE_LINE = re.compile(r"^(INFO|WARNING|DEBUG): ")
ISSUE_LINE = re.compile(r"(warning|error|failed|unknown|unimplemented|unhandled|todo|invalid|"
                        r"missing)", re.I)


def last_number(regex, text):
    values = [int(m.group(1)) for m in regex.finditer(text)]
    return max(values) if values else 0


def function_row(address):
    """The row of config/functions.ghidra.csv whose range contains the address."""
    if not FUNCTIONS_CSV.is_file():
        return None
    try:
        with FUNCTIONS_CSV.open(newline="") as handle:
            for row in csv.reader(handle):
                if len(row) < 3:
                    continue
                try:
                    start, end = int(row[1], 16), int(row[2], 16)
                except ValueError:
                    continue
                if start <= address < end:
                    return ",".join(row)
    except OSError:
        return None
    return None


def last_progress_line(lines):
    for line in reversed(lines):
        if NOISE_LINE.match(line):
            continue
        if PROGRESS_LINE.search(line):
            return line.strip()[:200]
    return "no progress line in the log"


def first_line_with(regex, text):
    match = regex.search(text)
    return match.group(0).strip()[:200] if match else ""


def classify(text):
    """Returns (category, detail) for the text of a log."""
    lines = text.splitlines()
    frames = last_number(PATTERNS["frame"], text)
    vsyncs = last_number(PATTERNS["vsync"], text)

    match = PATTERNS["missing_function"].search(text)
    if match:
        return "MISSING_FUNCTION", "0x" + match.group(1).lower()
    match = PATTERNS["missing_branch"].search(text)
    if match:
        return "MISSING_BRANCH_TARGET", "0x" + match.group(1).lower()
    for key in ("unimplemented", "syscall_todo"):
        match = PATTERNS[key].search(text)
        if match:
            unknown = PATTERNS["unknown_syscall"].search(text)
            detail = match.group(0).strip()[:160]
            if unknown:
                detail = f"syscallId=0x{unknown.group(1)} | {detail}"
            return "UNIMPLEMENTED_SYSCALL", detail
    match = PATTERNS["iop_unhandled"].search(text)
    if match:
        return "IOP_UNHANDLED", match.group(0).strip()[:200]
    match = PATTERNS["iop_error"].search(text)
    if match:
        return "IOP_ERROR", match.group(0).strip()[:200]
    match = PATTERNS["startup_failed"].search(text)
    if match:
        return "STARTUP_FAILED", match.group(0).strip()[:200]

    # CRASH: a crash message in the tail of the log, or a fatal status written by smoke.sh.
    tail = "\n".join(lines[-40:])
    match = PATTERNS["crash"].search(tail)
    if match:
        return "CRASH", match.group(0).strip()[:200]
    status = PATTERNS["smoke_status"].findall(text)
    if status:
        code = int(status[-1])
        if code in SIGNALS:
            return "CRASH", f"{SIGNALS[code]} (exit status {code})"
        if code not in (0, 124, 137, 141, 143) and code > 128:
            return "CRASH", f"signal {code - 128} (exit status {code})"
        if code not in (0, 124, 137, 141, 143) and code != 0:
            return "CRASH", f"exit status {code}"

    if frames >= FRAME_THRESHOLD and vsyncs >= VSYNC_THRESHOLD:
        return "OK_VSYNC_LOOP", f"{frames} {vsyncs}"
    if frames >= FRAME_THRESHOLD:
        return "FRAME_LOOP_UNPACED", f"{frames}"
    return "NO_PROGRESS", last_progress_line(lines)


def summary(text, category, detail):
    out = []
    lines = text.splitlines()
    out.append(f"frames={last_number(PATTERNS['frame'], text)} "
               f"vsyncs={last_number(PATTERNS['vsync'], text)} lines={len(lines)}")

    out.append("boot milestones seen:")
    for name, regex in MILESTONES:
        count = len(regex.findall(text))
        out.append(f"  {'yes' if count else 'no ':3} {name}" + (f" (x{count})" if count else ""))
    seen = [m for m in MODULES if re.search(r"\b" + re.escape(m) + r"(?:\.irx)?\b", text)]
    out.append("  modules named in the log: " + (", ".join(seen) if seen else "none"))
    files = [f for f in FILES if f.lower() in text.lower()]
    out.append("  files named in the log: " + (", ".join(files) if files else "none"))

    counter = Counter()
    for line in lines:
        if NOISE_LINE.match(line) or not ISSUE_LINE.search(line):
            continue
        key = re.sub(r"0x[0-9a-fA-F]+|\b\d+\b", "N", line.strip())[:120]
        counter[key] += 1
    out.append("most frequent warning/error lines (numbers folded to N):")
    if counter:
        for key, count in counter.most_common(15):
            out.append(f"  {count:7} {key}")
    else:
        out.append("  none")

    if category in ("MISSING_FUNCTION", "MISSING_BRANCH_TARGET"):
        address = int(detail, 16)
        row = function_row(address)
        out.append("row of config/functions.ghidra.csv containing the address: "
                   + (row if row else "none (the address is in no listed function)"))
    return out


def main(argv):
    if len(argv) != 2:
        print("usage: classify_log.py <log>", file=sys.stderr)
        return 2
    path = Path(argv[1])
    text = path.read_text(encoding="utf-8", errors="replace")
    category, detail = classify(text)
    lines = text.splitlines()

    print(f"RESULT: {category} {detail}".rstrip())
    for line in summary(text, category, detail):
        print(line)

    report = {"category": category, "detail": detail,
              "last_lines": [line[:300] for line in lines[-30:]]}
    path.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if category == "OK_VSYNC_LOOP" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
