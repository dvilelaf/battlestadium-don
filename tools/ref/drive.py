#!/usr/bin/env python3
"""Drives the reference PCSX2 started by tools/ref/pcsx2_auto.sh on its virtual display.

This is the only place that knows keyboard keys; the route file speaks in PS2 buttons.

Subcommands (run with: uv run --no-project --with pillow --with numpy python -I tools/ref/drive.py ...):
  press BUTTON [HOLD_MS]     press and release a PS2 button
  grab [NAME]                save the virtual screen as work/ref/pcsx2/auto/grabs/NAME.png
  store NAME                 store the game picture as the expected screen NAME (for wait_screen)
  matches NAME               tell how far the current screen is from the stored screen NAME
  capture NAME [nocheck]     GS dump + screenshot (Shift+F8), savestate (F1), name the three files;
                             the dump is replayed with build/gs/gsreplay and retaken if it is poor
  probe N SECONDS            take N dumps SECONDS apart and print what each replays to (not named)
  loadstate                  load the savestate of slot 1 (F3) from the savestate folder
  route FILE                 play a route file (see tools/ref/route.txt)

Options: --raw DIR (where PCSX2 saves captures; default $DON_AUTO_RAW or work/ref/pcsx2/raw),
--out DIR (where the named captures go; default $DON_AUTO_OUT or work/ref/pcsx2).

Route file, one step per line, '#' starts a comment:
  press BUTTON [HOLD_MS]       press a PS2 button for HOLD_MS (default 100)
  wait SECONDS                 wait a fixed time
  wait_screen NAME [SECONDS]   wait until the screen looks like the stored screen NAME (default 180 s)
  capture NAME [nocheck]       take the three files of one of the six moments; "nocheck" skips the
                               check that the GS dump replays to a real picture
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# PCSX2's default keyboard mapping for pad 1 ([Pad1] in PCSX2.ini), as X keysyms.
BUTTON_KEYS = {
    "up": "Up",
    "down": "Down",
    "left": "Left",
    "right": "Right",
    "cross": "k",
    "circle": "l",
    "square": "j",
    "triangle": "i",
    "start": "Return",
    "select": "BackSpace",
    "l1": "q",
    "r1": "e",
    "l2": "1",
    "r2": "3",
}

CAPTURE_NAMES = (
    "01-title",
    "02-menu",
    "03-charselect",
    "04-match-1v1",
    "05-match-4p",
    "06-results",
)

# PCSX2 hotkeys ([Hotkeys] in PCSX2.ini)
KEY_GSDUMP_SINGLE_FRAME = "shift+F8"
KEY_SAVESTATE = "F1"
KEY_LOADSTATE = "F3"

# A GS dump is checked by replaying it with build/gs/gsreplay (software renderer): the replay must
# succeed and the image must have at least this many distinct colours. The good dumps of the six
# moments give 30,000 to 120,000; a frame in which the game draws almost nothing gives 1.
MIN_DUMP_COLOURS = 5000
# Seconds to wait before each retake of a poor dump (the first take is immediate).
RETAKE_DELAYS = (0.3, 0.7, 1.5, 3.0, 5.0, 8.0, 12.0, 20.0)

DEFAULT_HOLD_MS = 100
# A new capture file counts as complete when its size has not changed for this long.
SETTLE_S = 0.4
DEFAULT_WAIT_SCREEN_S = 180.0
# Mean absolute difference (0..255) between two 64x48 grey thumbnails below which two screens
# count as the same.
MATCH_THRESHOLD = 12.0
# The game picture inside the PCSX2 window (menu bar and status bar excluded), on a 1280x960
# virtual screen with the window at its default position.
GAME_BOX = (1, 45, 1051, 664)


class RouteError(ValueError):
    pass


@dataclass(frozen=True)
class Step:
    cmd: str
    args: tuple
    line: int


def button_key(button: str) -> str:
    """X keysym that PCSX2 maps to the PS2 button, or RouteError."""
    try:
        return BUTTON_KEYS[button.lower()]
    except KeyError:
        raise RouteError(f"unknown button '{button}' (use one of: {', '.join(BUTTON_KEYS)})") from None


def _number(text: str, what: str, line: int, minimum: float = 0.0) -> float:
    try:
        value = float(text)
    except ValueError:
        raise RouteError(f"line {line}: {what} must be a number, got '{text}'") from None
    if value < minimum:
        raise RouteError(f"line {line}: {what} must be at least {minimum}, got {text}")
    return value


def parse_route(text: str) -> list[Step]:
    """Parses a route file. Raises RouteError with the line number on the first mistake."""
    steps: list[Step] = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cmd, *args = line.split()
        cmd = cmd.lower()
        if cmd == "press":
            if not 1 <= len(args) <= 2:
                raise RouteError(f"line {number}: usage: press BUTTON [HOLD_MS]")
            try:
                button_key(args[0])
            except RouteError as e:
                raise RouteError(f"line {number}: {e}") from None
            hold = _number(args[1], "HOLD_MS", number, 1) if len(args) == 2 else DEFAULT_HOLD_MS
            steps.append(Step("press", (args[0].lower(), int(hold)), number))
        elif cmd == "wait":
            if len(args) != 1:
                raise RouteError(f"line {number}: usage: wait SECONDS")
            steps.append(Step("wait", (_number(args[0], "SECONDS", number),), number))
        elif cmd == "wait_screen":
            if not 1 <= len(args) <= 2:
                raise RouteError(f"line {number}: usage: wait_screen NAME [SECONDS]")
            timeout = _number(args[1], "SECONDS", number, 1) if len(args) == 2 else DEFAULT_WAIT_SCREEN_S
            steps.append(Step("wait_screen", (args[0], timeout), number))
        elif cmd == "capture":
            if not 1 <= len(args) <= 2:
                raise RouteError(f"line {number}: usage: capture NAME [nocheck]")
            if args[0] not in CAPTURE_NAMES:
                raise RouteError(f"line {number}: unknown capture '{args[0]}' (use one of: {', '.join(CAPTURE_NAMES)})")
            if len(args) == 2 and args[1] != "nocheck":
                raise RouteError(f"line {number}: the only option of capture is 'nocheck', got '{args[1]}'")
            steps.append(Step("capture", (args[0], len(args) == 1), number))
        else:
            raise RouteError(f"line {number}: unknown command '{cmd}'")
    return steps


# ---------------------------------------------------------------------------------------------
# Everything below talks to the emulator.


class Paths:
    def __init__(self, raw: Path | None = None, out: Path | None = None):
        self.root = ROOT
        self.state = ROOT / "work/ref/pcsx2/auto/state"
        self.grabs = ROOT / "work/ref/pcsx2/auto/grabs"
        self.screens = ROOT / "work/ref/pcsx2/auto/screens"
        self.default_raw = ROOT / "work/ref/pcsx2/raw"
        self.default_out = ROOT / "work/ref/pcsx2"
        self.raw = raw or Path(os.environ.get("DON_AUTO_RAW") or self.default_raw)
        self.out = out or Path(os.environ.get("DON_AUTO_OUT") or self.default_out)


def log(msg: str) -> None:
    print(f"[drive] {msg}", flush=True)


def virtual_display(paths: Paths) -> str:
    """The display of the Xvfb that pcsx2_auto.sh started. Never the user's display."""
    f = paths.state / "display"
    if not f.is_file():
        raise SystemExit("[drive] ERROR: no virtual display; run: bash tools/ref/pcsx2_auto.sh start")
    disp = f.read_text().strip()
    if not disp.startswith(":") or disp == ":0":
        raise SystemExit(f"[drive] ERROR: refusing display '{disp}'")
    return disp


def _env(paths: Paths) -> dict[str, str]:
    env = dict(os.environ)
    env["DISPLAY"] = virtual_display(paths)
    env.pop("XAUTHORITY", None)
    return env


def xdotool(paths: Paths, *args: str) -> None:
    subprocess.run(["xdotool", *args], env=_env(paths), check=True)


def focus(paths: Paths) -> None:
    wid = (paths.state / "window").read_text().strip()
    xdotool(paths, "windowfocus", wid)


def press(paths: Paths, button: str, hold_ms: int = DEFAULT_HOLD_MS) -> None:
    key = button_key(button)
    focus(paths)
    xdotool(paths, "keydown", key)
    time.sleep(hold_ms / 1000)
    xdotool(paths, "keyup", key)
    time.sleep(0.15)  # lets the game see the release before the next press


def hotkey(paths: Paths, combo: str) -> None:
    focus(paths)
    xdotool(paths, "key", combo)


def grab(paths: Paths, dest: Path) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["import", "-window", "root", str(dest)], env=_env(paths), check=True)
    return dest


def thumbnail(path: Path, box: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)):
    """64x48 grey thumbnail (numpy float array) of the game picture of a grab.

    `box` is a sub-rectangle of the game picture as fractions (x0, y0, x1, y1); a screen whose
    content depends on who is playing (the results) is stored and matched on the part that
    does not.
    """
    import numpy as np
    from PIL import Image

    x0, y0, x1, y1 = GAME_BOX
    w, h = x1 - x0, y1 - y0
    crop = (round(x0 + box[0] * w), round(y0 + box[1] * h), round(x0 + box[2] * w), round(y0 + box[3] * h))
    with Image.open(path) as im:
        picture = im.convert("L").crop(crop).resize((64, 48), Image.BILINEAR)
        return np.asarray(picture, dtype=np.float32)


def parse_box(text: str) -> tuple[float, float, float, float]:
    try:
        x0, y0, x1, y1 = (float(v) for v in text.split(","))
    except ValueError:
        raise RouteError(f"box must be four fractions 'x0,y0,x1,y1', got '{text}'") from None
    if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
        raise RouteError(f"box must satisfy 0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1, got '{text}'")
    return (x0, y0, x1, y1)


def distance(a: Path, b: Path, box: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)) -> float:
    import numpy as np

    return float(np.abs(thumbnail(a, box) - thumbnail(b, box)).mean())


def store_screen(paths: Paths, name: str, box: str | None = None) -> Path:
    paths.screens.mkdir(parents=True, exist_ok=True)
    dest = paths.screens / f"{name}.png"
    grab(paths, dest)
    sidecar = paths.screens / f"{name}.box"
    if box:
        parse_box(box)
        sidecar.write_text(box + "\n")
    else:
        sidecar.unlink(missing_ok=True)
    return dest


def screen_distance(paths: Paths, name: str) -> float:
    ref = paths.screens / f"{name}.png"
    if not ref.is_file():
        raise SystemExit(f"[drive] ERROR: no stored screen '{name}' ({ref}); store it with: drive.py store {name}")
    sidecar = paths.screens / f"{name}.box"
    box = parse_box(sidecar.read_text().strip()) if sidecar.is_file() else (0.0, 0.0, 1.0, 1.0)
    now = grab(paths, paths.grabs / "_now.png")
    return distance(now, ref, box)


def wait_screen(paths: Paths, name: str, timeout: float) -> float:
    start = time.monotonic()
    best = float("inf")
    while True:
        d = screen_distance(paths, name)
        best = min(best, d)
        if d < MATCH_THRESHOLD:
            # Twice in a row: a frame of a fade or a transition can look like the screen for an instant.
            time.sleep(0.7)
            d2 = screen_distance(paths, name)
            if d2 < MATCH_THRESHOLD:
                log(f"screen '{name}' after {time.monotonic() - start:.1f} s (distance {d:.1f}, {d2:.1f})")
                return time.monotonic() - start
        if time.monotonic() - start > timeout:
            raise SystemExit(
                f"[drive] ERROR: screen '{name}' did not appear in {timeout:.0f} s "
                f"(closest distance {best:.1f}, last grab {paths.grabs / '_now.png'})"
            )
        time.sleep(1.0)


def _files(raw: Path, suffix: str) -> dict[Path, float]:
    return {p: p.stat().st_mtime for p in raw.glob(f"*{suffix}") if p.is_file()}


def _wait_new(raw: Path, suffix: str, before: dict[Path, float], timeout: float) -> Path:
    """Waits for a file with the suffix that is new or newer than before, and complete."""
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        for p, mtime in _files(raw, suffix).items():
            if p not in before or mtime > before[p]:
                size = p.stat().st_size
                time.sleep(SETTLE_S)
                if size > 0 and p.stat().st_size == size:
                    return p
        time.sleep(0.1)
    raise SystemExit(f"[drive] ERROR: no new '*{suffix}' appeared in {raw} within {timeout:.0f} s")


def gsreplay_path() -> Path | None:
    exe = Path(os.environ.get("DON_GS_BUILD_DIR") or ROOT / "build/gs") / "gsreplay"
    return exe if exe.is_file() and os.access(exe, os.X_OK) else None


def replay_check(paths: Paths, gs: Path) -> tuple[bool | None, str]:
    """Replays a GS dump on the host. Returns (good, summary); good is None if it cannot be checked."""
    exe = gsreplay_path()
    if exe is None:
        return None, "build/gs/gsreplay not built"
    out = paths.grabs / "_replay.png"
    out.unlink(missing_ok=True)
    run = subprocess.run([str(exe), "--dump", str(gs), str(out), "--renderer", "software"],
                         capture_output=True, text=True, timeout=300)
    text = run.stdout + run.stderr
    if "error while loading shared libraries" in text:
        return None, "gsreplay cannot load on this host"
    m = re.search(r"transfer=(\d+).*?path1=(\d+)", text)
    summary = f"{m.group(1)} transfers, {m.group(2)} bytes" if m else "no summary"
    if run.returncode != 0 or not out.is_file():
        return False, f"replay failed (exit {run.returncode}): {summary}"
    from PIL import Image

    with Image.open(out) as im:
        colours = len(im.convert("RGB").getcolors(maxcolors=1 << 24) or ())
    return colours >= MIN_DUMP_COLOURS, f"{summary}, {colours} colours (need {MIN_DUMP_COLOURS})"


def take_dump(paths: Paths) -> tuple[Path, Path]:
    """One Shift+F8: returns the new GS dump and screenshot (in the raw folder)."""
    before = {s: _files(paths.raw, s) for s in (".png", ".gs.zst")}
    hotkey(paths, KEY_GSDUMP_SINGLE_FRAME)
    gs = _wait_new(paths.raw, ".gs.zst", before[".gs.zst"], 180)
    png = _wait_new(paths.raw, ".png", before[".png"], 60)
    return gs, png


def capture(paths: Paths, name: str, check: bool = True) -> None:
    if name not in CAPTURE_NAMES:
        raise SystemExit(f"[drive] ERROR: unknown capture '{name}'")
    paths.raw.mkdir(parents=True, exist_ok=True)
    before_p2s = _files(paths.raw, ".p2s")
    for attempt in range(len(RETAKE_DELAYS) + 1):
        gs, png = take_dump(paths)
        if not check:
            break
        good, summary = replay_check(paths, gs)
        if good is None:
            log(f"capture '{name}': dump not checked ({summary})")
            break
        if good:
            log(f"capture '{name}': dump replays to a picture ({summary})")
            break
        gs.unlink()
        png.unlink()
        if attempt == len(RETAKE_DELAYS):
            raise SystemExit(f"[drive] ERROR: capture '{name}': still a poor dump after {attempt + 1} takes ({summary})")
        delay = RETAKE_DELAYS[attempt]
        log(f"capture '{name}': poor dump ({summary}); retaking in {delay} s")
        time.sleep(delay)
    hotkey(paths, KEY_SAVESTATE)
    p2s = _wait_new(paths.raw, ".p2s", before_p2s, 120)
    log(f"capture '{name}': {png.name}, {p2s.name}, {gs.name}")
    name_capture(paths, name, png, p2s, gs)


def probe(paths: Paths, count: int, interval: float) -> None:
    """Takes COUNT dumps INTERVAL seconds apart and prints what each replays to; keeps nothing."""
    paths.raw.mkdir(parents=True, exist_ok=True)
    for i in range(count):
        gs, png = take_dump(paths)
        good, summary = replay_check(paths, gs)
        log(f"probe {i + 1}/{count}: {summary}" + ("" if good is None else " -> good" if good else " -> POOR"))
        gs.unlink()
        png.unlink()
        time.sleep(interval)


def name_capture(paths: Paths, name: str, png: Path, p2s: Path, gs: Path) -> None:
    if paths.raw.resolve() == paths.default_raw.resolve() and paths.out.resolve() == paths.default_out.resolve():
        subprocess.run(["bash", str(ROOT / "tools/ref/name_capture.sh"), name], check=True, cwd=ROOT)
        return
    # Same behaviour as tools/ref/name_capture.sh, for another folder.
    paths.out.mkdir(parents=True, exist_ok=True)
    for src, ext in ((png, "png"), (p2s, "p2s"), (gs, "gs.zst")):
        shutil.move(str(src), str(paths.out / f"{name}.{ext}"))


def play_route(paths: Paths, steps: list[Step]) -> None:
    started = time.monotonic()
    for step in steps:
        log(f"[{time.monotonic() - started:6.1f} s] line {step.line}: {step.cmd} {' '.join(map(str, step.args))}")
        if step.cmd == "press":
            press(paths, step.args[0], step.args[1])
        elif step.cmd == "wait":
            time.sleep(step.args[0])
        elif step.cmd == "wait_screen":
            wait_screen(paths, step.args[0], step.args[1])
        elif step.cmd == "capture":
            capture(paths, step.args[0], step.args[1])
    log(f"route finished in {time.monotonic() - started:.0f} s")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Drives the reference PCSX2 on its virtual display.")
    p.add_argument("--raw", type=Path, help="folder where PCSX2 saves captures")
    p.add_argument("--out", type=Path, help="folder for the named captures")
    sub = p.add_subparsers(dest="command", required=True)
    s = sub.add_parser("press")
    s.add_argument("button")
    s.add_argument("hold_ms", nargs="?", type=int, default=DEFAULT_HOLD_MS)
    s = sub.add_parser("grab")
    s.add_argument("name", nargs="?", default="grab")
    s = sub.add_parser("store")
    s.add_argument("name")
    s.add_argument("--box", help="match only this part of the picture: x0,y0,x1,y1 as fractions")
    s = sub.add_parser("matches")
    s.add_argument("name")
    s = sub.add_parser("capture")
    s.add_argument("name")
    s.add_argument("--nocheck", action="store_true")
    s = sub.add_parser("probe")
    s.add_argument("count", type=int)
    s.add_argument("seconds", type=float)
    sub.add_parser("loadstate")
    s = sub.add_parser("route")
    s.add_argument("file", type=Path)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    paths = Paths(args.raw, args.out)
    if args.command == "press":
        button_key(args.button)
        press(paths, args.button, args.hold_ms)
    elif args.command == "grab":
        log(str(grab(paths, paths.grabs / f"{args.name}.png")))
    elif args.command == "store":
        log(str(store_screen(paths, args.name, args.box)))
    elif args.command == "matches":
        log(f"distance {screen_distance(paths, args.name):.2f} (threshold {MATCH_THRESHOLD})")
    elif args.command == "capture":
        capture(paths, args.name, not args.nocheck)
    elif args.command == "probe":
        probe(paths, args.count, args.seconds)
    elif args.command == "loadstate":
        focus(paths)
        hotkey(paths, KEY_LOADSTATE)
    elif args.command == "route":
        try:
            steps = parse_route(args.file.read_text())
        except RouteError as e:
            raise SystemExit(f"[drive] ERROR: {args.file}: {e}") from None
        play_route(paths, steps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
