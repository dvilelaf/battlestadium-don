#!/usr/bin/env python3
"""Measure the game thread in a 4-player fight, headless, and check that the run is deterministic.

What: runs the port for 130 s of emulated time with the multiplayer_ougi input script (menus to a
4-player fight, then the ougi of P1; the fight is blanks 6000-7800). While it runs, /proc/<pid>/task/*
is sampled every second (utime+stime and the thread name from comm). The DON_TRACE_STATE=60 lines
("[don] state blank=N ...") give the blank the run is at, so the CPU of each thread is taken over the
fight only. At the end it prints the per-thread CPU (% of one core), the busiest thread, the run's fps
and worst 10 s window (from the "[don] exit:" line) and a sha256 of all state lines: two runs of the
same build with the same script must print the same hash (the run is a function of its inputs).

Why: the optimisation tasks need one number that says how busy the game thread is in the part of the
game that is slow, measured the same way each time, without a window and without the sound card.

How (from the repository root):

    uv run --no-project python -I tools/run/perf_fight.py
    uv run --no-project python -I tools/run/perf_fight.py --binary build/player/don-linux/don --json
    uv run --no-project python -I tools/run/perf_fight.py --renderer vulkan
    uv run --no-project python -I tools/run/perf_fight.py --sound on

The full log of the run is kept in work/perf/ (work/ is git-ignored); the user folder of the program is
a fresh temporary directory (DON_USER_DIR), so the owner's saves and settings are never touched. The
display (DISPLAY, WAYLAND_DISPLAY) is removed from the environment and --headless is passed: the game
opens no window. --sound off (the default) passes "--sound off" to the game; --sound on passes
"--sound on", which costs the sound thread's CPU in the measurement, but the headless run opens no
audio device, so nothing plays. --user-dir DIR starts from a temporary copy of a prepared user folder
(module builds need one, made with `don --prepare`); DIR itself is never modified.
"""

import argparse
import fnmatch
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = "en_patch/jpvoice/disc/SLPS_256.75"
SCRIPT = "tools/run/scripts/multiplayer_ougi.txt"
FIGHT = (6000, 7800)
SECONDS = 200  # the fight ends at blank 7800: 130 s at 60 fps, more on a slow or busy machine
TRACE_INTERVAL = 60

STATE_RE = re.compile(r"^\[don\] state blank=(\d+) ")
EXIT_RE = re.compile(r"^\[don\] exit: .*\((?P<secs>[\d.]+)s, (?P<frames>\d+) frames(?:, worst 10s window (?P<worst>[\d.]+) fps)?\)")
CLK_TCK = os.sysconf("SC_CLK_TCK")


BUILD_TOOLS = {"ninja", "make"}
BUILD_SCRIPTS = ("build.sh", "build_gs*", "package_*")
INTERPRETERS = re.compile(r"^(python[0-9.]*|bash|sh|dash|zsh)$")


def is_build(argv):
    """A build tool, or an interpreter running a build script (not `bash -c "..."`, not an editor or grep)."""
    if not argv:
        return False
    name = Path(argv[0]).name
    if name in BUILD_TOOLS:
        return True
    if not INTERPRETERS.match(name):
        return False
    # Only the interpreter's own options come before the script; a "-c" after it belongs to the script.
    options = list(itertools.takewhile(lambda a: a.startswith("-"), argv[1:]))
    if "-c" in options:
        return False
    script = next((a for a in argv[1:] if not a.startswith("-")), None)
    return script is not None and any(fnmatch.fnmatch(Path(script).name, p) for p in BUILD_SCRIPTS)


def shown_path(path):
    """The path relative to this clone when it is inside it, else absolute (a binary may live in another checkout)."""
    try:
        return str(Path(path).relative_to(ROOT))
    except ValueError:
        return str(path)


def fight_reached(last_blank, frames):
    """True when the run got to the fight end. The game's exit line counts frames, which reach FIGHT[1] at the end;
    the state trace (every TRACE_INTERVAL blanks) can stop one interval short of it."""
    if frames is not None:
        return frames >= FIGHT[1]
    return last_blank >= FIGHT[1]


def real_build_pids(procs, excluded=()):
    """The pids of the processes in procs (pid, argv list) that are real builds, not in excluded."""
    return {pid for pid, argv in procs if pid not in excluded and is_build(argv)}


def list_processes():
    """(pid, argv) of every process readable in /proc (Linux); processes that exit meanwhile are skipped."""
    procs = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            raw = (entry / "cmdline").read_bytes()
        except OSError:
            continue
        procs.append((int(entry.name), [a.decode(errors="replace") for a in raw.split(b"\0") if a]))
    return procs


def own_lineage():
    """This process and its parents: the guard must not wait for itself or for the shell that started it."""
    pids, pid = set(), os.getpid()
    while pid > 1 and pid not in pids:
        pids.add(pid)
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
        except OSError:
            break
        pid = int(stat[stat.rindex(")") + 2:].split()[1])  # field 4, after the parenthesised comm
    return pids


def wait_for_builds():
    """Do not measure while a build runs: it would take the cores the game thread needs."""
    announced = False
    while real_build_pids(list_processes(), own_lineage()):
        if not announced:
            print("waiting: a build is running (ninja, make, build.sh, build_gs*, package_*)", file=sys.stderr)
            announced = True
        time.sleep(10)


def read_threads(pid):
    """{tid: (name, ticks)} from /proc/<pid>/task; threads that exit meanwhile are skipped."""
    threads = {}
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return threads
    for tid in tids:
        try:
            name = Path(f"/proc/{pid}/task/{tid}/comm").read_text().strip()
            stat = Path(f"/proc/{pid}/task/{tid}/stat").read_text()
        except OSError:
            continue
        # The comm field is in parentheses and may contain spaces: count from the last ')'.
        fields = stat[stat.rindex(")") + 2:].split()
        utime, stime = int(fields[11]), int(fields[12])
        threads[int(tid)] = (name, utime + stime)
    return threads


def game_command(binary, renderer, sound, seconds, game_module=None):
    """The command line of the game: the options of the fight script, then the ELF; --game-module if given."""
    command = [
        str(binary), "--headless", "--renderer", renderer, "--sound", sound, "--players", "4",
        "--input-script", SCRIPT, "--seconds", str(seconds),
    ]
    if game_module:
        command += ["--game-module", str(game_module)]
    return command + [ELF]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--binary", default="build/linux-dev/don", help="the port's executable")
    parser.add_argument("--renderer", default="vulkan", help="renderer passed to the game")
    parser.add_argument("--seconds", type=int, default=SECONDS,
                        help="seconds to run; the fight ends at blank 7800 (130 s at 60 fps, more when slower)")
    parser.add_argument("--sound", choices=("on", "off"), default="off",
                        help="value of the game's --sound option (default off); --headless opens no audio device anyway")
    parser.add_argument("--json", action="store_true", help="print one JSON object instead of text")
    parser.add_argument("--game-module", default=None,
                        help="path of the game module (libdon_game.so), passed to the game as --game-module")
    parser.add_argument("--user-dir", default=None,
                        help="a prepared user folder (module builds need one, made with `don --prepare`); each run "
                             "uses a temporary copy of it, so the folder is never modified; default: an empty one")
    args = parser.parse_args()

    os.chdir(ROOT)
    binary = Path(args.binary).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        sys.exit(f"perf_fight: no executable at {args.binary}")
    if not Path(ELF).exists():
        sys.exit(f"perf_fight: {ELF} is missing (the game's ELF, see docs/knowledge/01-game-and-discs.md)")

    wait_for_builds()

    if args.user_dir:
        source = Path(args.user_dir)
        if not source.is_dir():
            sys.exit(f"perf_fight: --user-dir {args.user_dir} is not a folder")
        user_dir = tempfile.mkdtemp(prefix="don-perf-user-")
        shutil.copytree(source, user_dir, dirs_exist_ok=True, symlinks=True)
    else:
        user_dir = tempfile.mkdtemp(prefix="don-perf-user-")
    work = ROOT / "work" / "perf"
    work.mkdir(parents=True, exist_ok=True)
    log_path = work / f"perf-fight-{time.strftime('%Y%m%d-%H%M%S')}.log"

    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env["DON_USER_DIR"] = user_dir
    env["DON_TRACE_STATE"] = str(TRACE_INTERVAL)
    command = game_command(binary, args.renderer, args.sound, args.seconds, args.game_module)

    state_lines = []
    exit_line = ""
    blank = [0]  # the latest blank seen in a state line, shared with the reader thread
    samples = []  # (wall seconds, blank, {tid: (name, ticks)})

    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True, errors="replace")

        def reader():
            nonlocal exit_line
            for line in proc.stderr:
                log.write(line)
                match = STATE_RE.match(line)
                if match:
                    blank[0] = int(match.group(1))
                    state_lines.append(line.rstrip("\n"))
                if line.startswith("[don] exit:"):
                    exit_line = line.rstrip("\n")

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        start = time.monotonic()
        next_sample = start
        while proc.poll() is None:
            now = time.monotonic()
            if now >= next_sample:
                samples.append((now - start, blank[0], read_threads(proc.pid)))
                next_sample += 1.0
            time.sleep(0.05)
        code = proc.wait()
        thread.join(timeout=5)
    shutil.rmtree(user_dir, ignore_errors=True)

    # CPU of each thread over the fight: the samples whose blank lies in FIGHT, consecutive pairs.
    in_fight = [s for s in samples if FIGHT[0] <= s[1] <= FIGHT[1]]
    per_name = {}  # name -> [ticks, wall seconds, blanks]
    for (t0, b0, th0), (t1, b1, th1) in zip(in_fight, in_fight[1:]):
        wall = t1 - t0
        for tid, (name, ticks1) in th1.items():
            if tid in th0:
                ticks0 = th0[tid][1]
                entry = per_name.setdefault(name, [0, 0.0, 0])
                entry[0] += ticks1 - ticks0
                entry[1] += wall
                entry[2] += b1 - b0
    cpu = {}
    # CPU milliseconds per blank: the work itself, which, unlike % or fps, does not change when other programs
    # load the machine; the number to compare before and after an optimisation.
    ms_per_blank = {}
    for name, (ticks, wall, blanks) in per_name.items():
        if wall > 0:
            cpu[name] = 100.0 * ticks / CLK_TCK / wall
        if blanks > 0:
            ms_per_blank[name] = 1000.0 * ticks / CLK_TCK / blanks
    cpu = dict(sorted(cpu.items(), key=lambda kv: -kv[1]))
    busiest = next(iter(cpu), None)

    exit_info = EXIT_RE.match(exit_line)
    fps = frames = worst = None
    if exit_info:
        frames = int(exit_info.group("frames"))
        fps = frames / float(exit_info.group("secs"))
        worst = float(exit_info.group("worst")) if exit_info.group("worst") else None

    digest = hashlib.sha256("\n".join(state_lines).encode()).hexdigest()
    last_blank = blank[0]
    fight_seconds = in_fight[-1][0] - in_fight[0][0] if len(in_fight) > 1 else 0.0

    result = {
        "binary": shown_path(binary),
        "exit_code": code,
        "last_blank": last_blank,
        "reached_fight_end": fight_reached(last_blank, frames),
        "fight_seconds_sampled": round(fight_seconds, 1),
        "cpu_percent_of_core": {k: round(v, 1) for k, v in cpu.items()},
        "cpu_ms_per_blank": {k: round(v, 2) for k, v in sorted(ms_per_blank.items(), key=lambda kv: -kv[1])},
        "busiest_thread": busiest,
        "fps": round(fps, 2) if fps is not None else None,
        "frames": frames,
        "worst_10s_fps": worst,
        "state_lines": len(state_lines),
        "state_sha256": digest,
        "log": str(log_path.relative_to(ROOT)),
        "exit_line": exit_line,
    }

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"binary: {result['binary']}  exit code {code}  last blank {last_blank}  "
              f"reached fight end: {result['reached_fight_end']}")
        print(f"fight blanks {FIGHT[0]}-{FIGHT[1]}: {fight_seconds:.1f} s sampled")
        print("thread                 CPU % of one core   CPU ms per blank")
        for name, value in cpu.items():
            per_blank = ms_per_blank.get(name)
            print(f"{name:<22} {value:6.1f}   {per_blank:16.2f}" if per_blank is not None else f"{name:<22} {value:6.1f}")
        print(f"busiest thread: {busiest}")
        print(f"fps: {result['fps']}  worst 10 s window: {worst} fps  (frames {frames})")
        print(f"state hash: sha256 of {len(state_lines)} state lines = {digest}")
        print(f"log: {result['log']}")
        print(f"exit line: {exit_line}")
    return 0 if code == 0 and result["reached_fight_end"] else 1


if __name__ == "__main__":
    sys.exit(main())
