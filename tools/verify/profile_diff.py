#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Symbol-by-symbol diff of two Windows DON_PROFILE files (T-766).

Usage:
  uv run --no-project --with pefile python -I tools/verify/profile_diff.py \
      A.profile A.map B.profile B.map [--winsys DIR] [--blanks N] [--top N]

The exe is symbolized with its MSVC/lld link map (the module whose path ends in .exe);
system DLLs with the export table of a copy in --winsys (needs pefile; without it they are
grouped as module+offset-bucket). Samples are converted to ms per blank with the sampler's
scale cpu_us / samples_taken, so the result is CPU time of the sampled thread per blank.
Samples include the sampler's parking bias (journal 059): read CPU symbols, not ntdll waits.
"""
import argparse
import bisect
import re
import sys
from dataclasses import dataclass
from pathlib import Path

BLANK_HZ = 60000 / 1001  # NTSC video blank rate; default blanks = duration * BLANK_HZ
BUCKET = 0x1000  # grouping of unsymbolized addresses


@dataclass
class Row:
    module: str
    symbol: str
    samples_a: int
    samples_b: int
    ms_a: float
    ms_b: float

    @property
    def delta(self):
        return self.ms_b - self.ms_a


def read_profile(path):
    header, modules, samples = {}, [], {}
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split(" ", 4)
        if parts[0] == "module":
            modules.append((int(parts[1], 16), int(parts[2], 16), parts[4]))
        elif parts[0].startswith("0x") and len(parts) == 2:
            samples[int(parts[0], 16)] = int(parts[1])
        elif len(parts) == 2:
            header[parts[0]] = parts[1]
    return header, modules, samples


def read_map(path):
    """Return (preferred_base, sorted [(va, name)]) from an MSVC-style link map."""
    base, syms = 0, []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
            continue
        m = re.match(r"\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s", line)
        # "$..." entries (e.g. $ehgcr_29_34) are labels inside the previous function, not functions
        if m and not m.group(1).startswith("$"):
            syms.append((int(m.group(2), 16), m.group(1)))
    syms.sort()
    return base, syms


def read_exports(path):
    import pefile  # lazy: only needed for system DLL names

    pe = pefile.PE(str(path), fast_load=False)
    ex = sorted((s.address, s.name.decode()) for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name)
    return ex


def basename(p):
    return p.replace("\\", "/").rsplit("/", 1)[-1]


def symbolize(profile_path, map_path, winsys, export_cache):
    """Return ({(module, symbol): samples}, total ms scale, header)."""
    header, modules, samples = read_profile(profile_path)
    base, syms = read_map(map_path)
    keys = [a for a, _ in syms]
    agg = {}
    for addr, count in samples.items():
        mod = next((m for m in modules if m[0] <= addr < m[1]), None)
        if mod is None:
            key = ("(outside modules)", f"{addr >> 32:x}")
        else:
            name = basename(mod[2])
            rva = addr - mod[0]
            if name.lower().endswith(".exe"):
                i = bisect.bisect_right(keys, base + rva) - 1
                key = (name, syms[i][1] if i >= 0 else f"+0x{rva:x}")
            else:
                key = (name, None)
                ex = None
                if winsys is not None and (Path(winsys) / name).exists():
                    if name not in export_cache:
                        export_cache[name] = read_exports(Path(winsys) / name)
                    ex = export_cache[name]
                if ex:
                    i = bisect.bisect_right([a for a, _ in ex], rva) - 1
                    key = (name, ex[i][1] if i >= 0 else f"+0x{rva:x}")
                else:
                    key = (name, f"{name}+0x{rva // BUCKET * BUCKET:x}")
        agg[key] = agg.get(key, 0) + count
    return agg, header


def diff(profile_a, map_a, profile_b, map_b, blanks=None, winsys=None):
    cache = {}
    agg_a, ha = symbolize(profile_a, map_a, winsys, cache)
    agg_b, hb = symbolize(profile_b, map_b, winsys, cache)
    if blanks is None:
        blanks = round(int(ha["duration_us"]) / 1e6 * BLANK_HZ)

    def scale(h):  # ms of thread CPU time per sample, per blank
        return int(h["cpu_us"]) / int(h["samples_taken"]) / 1000 / blanks

    sa, sb = scale(ha), scale(hb)
    rows = []
    for key in set(agg_a) | set(agg_b):
        a, b = agg_a.get(key, 0), agg_b.get(key, 0)
        rows.append(Row(key[0], key[1], a, b, a * sa, b * sb))
    rows.sort(key=lambda r: r.delta, reverse=True)
    return rows, {"blanks": blanks, "scale_a": sa, "scale_b": sb,
                  "total_ms_a": sum(r.ms_a for r in rows), "total_ms_b": sum(r.ms_b for r in rows)}


def render(rows, info, top):
    out = [f"blanks {info['blanks']}  total CPU ms/blank A {info['total_ms_a']:.3f}  B {info['total_ms_b']:.3f}"
           f"  delta {info['total_ms_b'] - info['total_ms_a']:+.3f}",
           f"{'delta':>8} {'A ms':>8} {'B ms':>8} {'A smp':>7} {'B smp':>7}  module  symbol"]
    for r in rows[:top]:
        out.append(f"{r.delta:+8.3f} {r.ms_a:8.3f} {r.ms_b:8.3f} {r.samples_a:7d} {r.samples_b:7d}  {r.module}  {r.symbol}")
    out.append("... biggest decreases:")
    for r in rows[::-1][:top // 2]:
        out.append(f"{r.delta:+8.3f} {r.ms_a:8.3f} {r.ms_b:8.3f} {r.samples_a:7d} {r.samples_b:7d}  {r.module}  {r.symbol}")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile_a"); ap.add_argument("map_a")
    ap.add_argument("profile_b"); ap.add_argument("map_b")
    ap.add_argument("--winsys", help="folder with copies of ntdll.dll, VCRUNTIME140.dll... for export names")
    ap.add_argument("--blanks", type=int, help="blanks in the run (default duration * 59.94)")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args(argv)
    rows, info = diff(a.profile_a, a.map_a, a.profile_b, a.map_b, a.blanks, a.winsys)
    print(render(rows, info, a.top))


if __name__ == "__main__":
    main()
