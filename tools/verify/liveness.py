#!/usr/bin/env python3
"""Liveness check of a run of the game: did the frame loop keep running? (T-304)

    uv run --no-project python -I tools/verify/liveness.py <log> [--max-gap N]

The run must have been made with DON_TRACE_FRAMES=1, which makes the game print `[don] frame <n>`
and `[don] vsync <n>` once every 60 events. The check walks the log in order and, for each frame
line, keeps the last vsync count seen before it. It reports the last counts, the vsync count at
which the frame counter last advanced and the largest gap in vsyncs between two advances (the tail,
from the last advance to the last vsync line, counts as a gap too). A log whose frame loop stopped
(the "stop") has a large tail gap. The default limit is five seconds of vertical blanks (300 vsyncs),
chosen from the logs of 2026-10-13: the largest gap of a healthy run of the route to a match, its loading
screens included, was 120; the shortest stop on record was a tail of 540 (a run that ended nine seconds
after its frame loop stopped). A run should go on for at least the limit after the part under test.

A last line that the end of the run cut in the middle is not read: only lines that end with a
newline count.

Exit codes: 0 when the largest gap is at most N; 1 when it is larger (the last line names the
vsync counts between which the frame loop did not run); 2 when the log has no frame or no vsync
line, or the file cannot be read.

Standard library only (`re`, `argparse`, `dataclasses`). `analyze()` is the API, `main()` the
command line; tests/test_liveness.py imports the first and runs the second.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

DEFAULT_MAX_GAP = 300

COUNTER = re.compile(r"\[don\] (frame|vsync) (\d+)")


@dataclass
class Liveness:
    path: Path
    last_frame: int | None      # the last frame count, None when the log has no frame line
    last_vsync: int | None      # the last vsync count, None when the log has no vsync line
    advance_vsync: int | None   # the vsync count at which the frame counter last advanced
    largest_gap: int            # the largest gap in vsyncs between two advances, or the tail
    gap_from: int | None        # the vsync count where the largest gap starts
    gap_to: int | None          # the vsync count where the largest gap ends


def analyze(path) -> Liveness:
    """Reads `path` and computes its figures.  Raises OSError when the file cannot be read."""
    path = Path(path)
    text = path.read_bytes().decode("utf-8", errors="replace")
    if not text.endswith("\n"):
        # The last line may have been cut by the end of the run: it is not read.
        text = text[: text.rfind("\n") + 1]

    vsync = None      # the last vsync count seen
    frame = None      # the last frame count seen
    advances = []     # the vsync count at each advance of the frame counter
    for match in COUNTER.finditer(text):
        count = int(match.group(2))
        if match.group(1) == "vsync":
            vsync = count
        elif frame is None or count > frame:
            frame = count
            advances.append(vsync or 0)

    if frame is None or vsync is None:
        return Liveness(path, frame, vsync, None, 0, None, None)

    # The gaps between consecutive advances, then the tail from the last advance to the last vsync.
    spans = list(zip(advances, advances[1:])) + [(advances[-1], vsync)]
    spans = [(start, max(start, end)) for start, end in spans]
    gap_from, gap_to = max(spans, key=lambda span: span[1] - span[0])
    return Liveness(path, frame, vsync, advances[-1], gap_to - gap_from, gap_from, gap_to)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="did the frame loop of a run keep running (T-304)")
    parser.add_argument("log", type=Path, help="the log of a run made with DON_TRACE_FRAMES=1")
    parser.add_argument(
        "--max-gap",
        type=int,
        default=DEFAULT_MAX_GAP,
        metavar="N",
        help=f"fail (exit 1) when the frame loop does not run for more than N vsyncs "
             f"(default {DEFAULT_MAX_GAP}, ten seconds)",
    )
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = analyze(args.log)
    except OSError as exc:
        print(f"[liveness] cannot read {args.log}: {exc.strerror or exc}")
        return 2
    missing = [name for name, value in (("frame", result.last_frame), ("vsync", result.last_vsync))
               if value is None]
    if missing:
        print(f"[liveness] {args.log}: no {' or '.join(missing)} line in the log "
              f"(was DON_TRACE_FRAMES=1 set?)")
        return 2

    failed = result.largest_gap > args.max_gap
    print(f"[liveness] {args.log}: frame={result.last_frame} vsync={result.last_vsync} "
          f"last frame advance at vsync {result.advance_vsync}; largest gap "
          f"{result.largest_gap} vsyncs (limit {args.max_gap}): {'FAIL' if failed else 'ok'}")
    if failed:
        print(f"[liveness] the frame loop did not run from vsync {result.gap_from} "
              f"to vsync {result.gap_to}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
