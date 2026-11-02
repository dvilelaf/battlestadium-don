# SPDX-License-Identifier: GPL-3.0-or-later
# T-305: what a Wayland compositor did with a program's pictures, read from a WAYLAND_DEBUG=1 log (stderr of the
# program): the gaps between the program's buffer commits (what it submitted) and between wl_buffer.release events
# (the compositor finishing with a buffer: the closest to "shown" a client can see). Prints median, p99, max and the
# histogram of the gaps in ms, from the given second of the log on.
#
#   uv run --no-project python -I tools/verify/wayland_present.py LOG [FIRST_SECOND]
import re
import sys
from collections import Counter

LINE = re.compile(r"^\[\s*(\d+)\.(\d+)\]\s+(->\s+)?(\S+)")


def gaps(times):
    return [b - a for a, b in zip(times, times[1:])]


def quantile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))] if values else 0.0


def main():
    path = sys.argv[1]
    first = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    commits, releases, dones = [], [], []
    t0 = None
    for line in open(path, errors="replace"):
        m = LINE.match(line)
        if not m:
            continue
        t = int(m.group(1)) + int(m.group(2).ljust(3, "0")[:3]) / 1000.0
        if t0 is None:
            t0 = t
        if t - t0 < first * 1000:
            continue
        if m.group(3) and re.sub(r"@\d+|\(.*", "", m.group(4)) == "wl_surface.commit":
            commits.append(t)
        elif not m.group(3) and re.sub(r"@\d+|\(.*", "", m.group(4)) == "wl_callback.done":
            dones.append(t)
        elif not m.group(3) and re.sub(r"@\d+|\(.*", "", m.group(4)) == "wl_buffer.release":
            releases.append(t)
    for name, times in (("commit", commits), ("buffer release", releases), ("frame callback", dones)):
        g = gaps(times)
        if not g:
            print(f"{name}: none")
            continue
        hist = Counter(round(x / 2.0) * 2 for x in g)
        print(f"{name}: {len(times)} events, gap median {quantile(g, .5):.1f} ms, p99 {quantile(g, .99):.1f}, max {max(g):.1f}")
        print("  gap histogram (2 ms bins):", " ".join(f"{k}:{v}" for k, v in sorted(hist.items()) if v >= max(3, len(g) // 100)))


main()
