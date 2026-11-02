#!/usr/bin/env python3
"""Fast check of the real program (T-704): every setup screen, every button, every failure of one pass.

    uv run --no-project python -I tools/run/setup_check.py [--full] [--only NAME[,NAME]] [--keep]

Drives the real `don` (built with no game code linked) through the first-run screens on a private Xvfb (fixed display
number, killed by pid, never the user's desktop) with the test hooks of src/host/setup_run.h: DON_SETUP_SCRIPT feeds
keys, DON_SETUP_SHOTS writes one PNG per screen change, DON_TRACE_SETUP=1 prints "[don] setup screen: <key>" at each
change. One run of `don` per path. Never stops at the first failure: prints `PASS|FAIL <path> <what was seen>` for each
path and, last, `setup_check: N passed, M failed (T s)`; exit code 1 when M > 0.

Default mode stops preparation early (DON_SETUP_QUIT_AT); --full prepares to the end, presses PLAY and waits for the
game's first `[don] state blank` line.

Environment: DON_CHECK_BUILD_DIR (default build/check-asan), DON_CHECK_SANITIZE (default address,undefined; "" = plain), DON_JOBS (default 6), DON_CHECK_DISPLAY (default 97),
DON_CHECK_ISO (default en_patch/jpvoice/disc.iso), DON_RUNTIME_DIR (PS2Recomp tree, default: the pinned commit plus the patch series, as the release).
Selection moves inside one screen change no screen key, so they are not checked: only screen changes are.
"""
import argparse
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from setup_check_build import build, build_dir, ensure_install, sanitizer_env  # noqa: E402
from setup_check_paths import PATHS  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# A log line that matches one of these is a FAIL of its path; the reason says why it is not normal for a clean run.
BAD_LOG_PATTERNS = [
    (r"ERROR: AddressSanitizer", "AddressSanitizer found a memory error"),
    (r"runtime error:", "UndefinedBehaviorSanitizer found undefined behaviour"),
    (r"\bassert", "an assertion failed"),
    (r"\babort", "the program aborted"),
    (r"\bfatal", "a fatal error was reported"),
    (r"error", "an error line (the screens' own errors are shown in the window, not in the log)"),
]
CRASH_SIGNALS = {signal.SIGSEGV, signal.SIGABRT, signal.SIGBUS, signal.SIGILL, signal.SIGFPE}
SCREEN_LINE = re.compile(r"\[don\] setup screen: (\S+)(?: (\S+))?")   # key, and the error code of an error screen
GAME_LINE = "[don] state blank"
SCRIPT_FRAMES = "40"        # unchanged frames (16 ms each) before the next key (setup_run.h default is 180): a state stays
                            # on screen about 0.7 s, so a grab every SNAP_EVERY s sees it
SNAP_EVERY_FULL = 20.0      # s: the full run lasts minutes and the machine is busy compiling
SNAP_EVERY = 0.25           # s between two grabs of the virtual screen; identical grabs are kept once


def classify_log(text):
    """Every bad line of `text` as (line, reason); a line is reported once, for the first pattern it matches."""
    bad = []
    for line in text.splitlines():
        if SCREEN_LINE.search(line):   # the harness's own trace of a screen key ("checking-error" is a screen, not a log error)
            continue
        for pattern, reason in BAD_LOG_PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):   # glibc says "Assertion ... failed"
                bad.append((line.strip(), reason))
                break
    return bad


def screens_of(text):   # "checking-error E-PICK-3" for an error screen, "welcome" otherwise
    return [" ".join(filter(None, m)) for m in SCREEN_LINE.findall(text)]


def judge(name, returncode, timed_out, text, expected, shots_count, extra_bad=()):
    """The verdict of one path: (ok, what was seen). `expected` = the screen keys in order, None = not checked."""
    problems = []
    seen = screens_of(text)
    if timed_out:
        problems.append("did not exit in time (hung)")
    elif returncode is not None and returncode < 0:
        try:
            problems.append(f"crashed with {signal.Signals(-returncode).name}")
        except ValueError:
            problems.append(f"killed by signal {-returncode}")
    elif returncode:
        problems.append(f"exit code {returncode}")
    for line, reason in classify_log(text):
        problems.append(f"log: {reason}: {line[:160]}")
    problems.extend(extra_bad)
    if expected is not None and seen != expected:
        problems.append(f"screens {seen} but expected {expected}")
    if shots_count < len(seen):
        problems.append(f"{shots_count} screenshots for {len(seen)} screens")
    ok = not problems
    return ok, ("screens " + " > ".join(seen)) if ok else "; ".join(problems) + f" (screens {' > '.join(seen) or 'none'})"


def summary_line(passed, failed, seconds):
    return f"setup_check: {passed} passed, {failed} failed ({seconds:.0f} s)"


def make_home(base, kind, iso):
    """A fresh fake HOME. `disc` holds a copy (a hard link when the folders share a file system) of the disc image in
    Downloads (discovery ignores symlinks); `folders` holds the folders the browser paths walk through."""
    home = base / "home"
    for d in ("Downloads", "Documents"):
        (home / d).mkdir(parents=True)
    if kind == "disc":
        target = home / "Downloads" / "disc.iso"
        try:
            os.link(iso, target)
        except OSError:
            shutil.copyfile(iso, target)
    if kind == "gamefolder":
        (home / "Documents" / "g-game").mkdir()
        (home / "Documents" / "g-game" / "SYSTEM.CNF").write_text("BOOT2 = cdrom0:\\SLPS_256.75;1\nVER = 1.00\n")
    if kind == "folders":
        docs = home / "Documents"
        (docs / "a-empty").mkdir()
        (docs / "b-noperm").mkdir()
        (docs / "b-noperm" / "hidden.iso").write_bytes(b"x")
        (docs / "b-noperm").chmod(0)
        odd = docs / "c-odd dir ñ ü 日本"
        odd.mkdir()
        (odd / "not a disc é.txt").write_text("not a disc image\n")
        (docs / "d-loop").symlink_to(docs.parent)
        many = docs / "e-many"
        many.mkdir()
        (many / "a-sub").mkdir()   # T-713: a folder row inside the large listing (folders sort first: row 0)
        for i in range(5000):
            (many / f"f{i:04d}.bin").write_bytes(b"")
        (docs / "notes.txt").write_text("this is not a disc image\n")
        (docs / "junk.iso").write_bytes(os.urandom(1 << 20))
        with open(iso, "rb") as src, open(docs / "trunc.iso", "wb") as dst:
            dst.write(src.read(8 << 20))
    return home


def fix_modes(base):
    for p in base.rglob("*"):
        try:
            if p.is_dir() and not p.is_symlink():
                p.chmod(0o755)
        except OSError:
            pass


class Xvfb:
    def __init__(self, display):
        self.display = f":{display}"
        self.proc = None

    def __enter__(self):
        lock = Path(f"/tmp/.X{self.display[1:]}-lock")
        if lock.exists():
            sys.exit(f"setup_check: display {self.display} is taken ({lock}); set DON_CHECK_DISPLAY to a free number")
        self.proc = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1280x720x24", "-nolisten", "tcp"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            if lock.exists():
                return self
            time.sleep(0.1)
        self.proc.kill()
        sys.exit("setup_check: Xvfb did not start")

    def __exit__(self, *exc):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def grab(display, target):
    """One screenshot of the whole virtual screen; True when the file was written."""
    try:
        r = subprocess.run(["import", "-display", display, "-window", "root", str(target)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
    except subprocess.TimeoutExpired:   # the machine is busy (a build runs): the next grab tries again
        return False
    return r.returncode == 0 and target.is_file()


def run_don(exe, env, log_path, timeout, until=None, hold=None, args=(), snap_every=SNAP_EVERY):
    """Runs `don` and grabs the screen every SNAP_EVERY s (a grab equal to the last one is dropped: they are the states
    a key changed). Ends when the process exits, `until` appears in the log, `hold` seconds passed (the process is then
    stopped by the harness: it was still running, so it had not crashed) or `timeout` passed (a hang).
    Returns (returncode, timed_out, text, frames)."""
    frames, last_hash, last_grab = [], None, 0.0
    shots = log_path.parent
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([str(exe), *args], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        start, timed_out, held = time.time(), False, False
        while proc.poll() is None:
            now = time.time() - start
            if hold is None and now > timeout:
                timed_out = True
                break
            if hold is not None and now > hold:
                held = True
                break
            if until and until in log_path.read_text(errors="replace"):
                break
            if time.time() - last_grab < snap_every:
                time.sleep(min(snap_every, 0.5))
                continue
            last_grab = time.time()
            tmp = shots / "grab.png"
            if grab(env["DISPLAY"], tmp):
                digest = hashlib.sha1(tmp.read_bytes()).hexdigest()
                if digest != last_hash:
                    last_hash = digest
                    tmp.rename(shots / f"frame-{len(frames) + 1:02d}.png")
                    frames.append(digest)
                else:
                    tmp.unlink()
        if proc.poll() is None:
            proc.kill()   # not SIGTERM: SDL turns it into the window's close button, which asks "Stop preparing?"
        rc = proc.wait()
        if held:
            rc = 0
    text = log_path.read_text(errors="replace")
    return (0 if until and until in text else rc), timed_out, text, frames


def run_path(p, exe, display, tmp, shots_root, iso, libs, results, previous):
    base = tmp / p.name
    base.mkdir(parents=True)
    if p.resume_of:
        user = previous[p.resume_of]["user"]
        home = previous[p.resume_of]["home"]
    else:
        user = base / "user"
        user.mkdir()
        home = make_home(base, p.home, iso)
    shots = shots_root / p.name
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True)
    env = dict(os.environ, DISPLAY=display, HOME=str(home), DON_USER_DIR=str(user), DON_TRACE_SETUP="1",
               DON_SETUP_SHOTS=str(shots), DON_SETUP_FRAMES=SCRIPT_FRAMES, DON_SETUP_SCRIPT=p.script.replace("{home}", str(home)),
               LD_LIBRARY_PATH=str(libs), SDL_AUDIODRIVER="dummy", XDG_CONFIG_HOME=str(home / ".config"),
               XDG_DATA_HOME=str(home / ".local/share"))
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DBUS_SESSION_BUS_ADDRESS", None)
    env.update(sanitizer_env())
    env.update(p.env)
    if p.quit_at:
        env["DON_SETUP_QUIT_AT"] = p.quit_at
    log_path = shots / "run.log"
    until = GAME_LINE if p.full else None
    rc, timed_out, text, frames = run_don(exe, env, log_path, p.timeout, until, p.hold, p.args,
                                                 SNAP_EVERY_FULL if p.full else SNAP_EVERY)
    previous[p.name] = {"user": user, "home": home}
    n_shots = len(list(shots.glob("*.png")))
    extra = [f"only {len(frames)} distinct screen grabs, expected {p.min_frames}: a key changed nothing"] if len(frames) < p.min_frames else []
    ok, seen = judge(p.name, rc, timed_out, text, p.expected, n_shots, extra)
    results.append((ok, f"{'PASS' if ok else 'FAIL'} {p.name} {seen}"))
    print(results[-1][1], flush=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--full", action="store_true", help="prepare to the end, press PLAY, wait for the first game frame")
    ap.add_argument("--only", help="comma-separated path names")
    ap.add_argument("--keep", action="store_true", help="keep /tmp/don-check-<pid>/")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args(argv)
    t0 = time.time()
    results = []
    (ROOT / "work" / "check").mkdir(parents=True, exist_ok=True)
    if not args.skip_build:
        build_log = ROOT / "work" / "check" / "build.log"
        with open(build_log, "w") as log:
            built = build(log)
        if not built:
            print(f"FAIL build see {build_log}")
            print(summary_line(0, 1, time.time() - t0))
            return 1
    libs = build_dir() / "libs"
    with open(ROOT / "work" / "check" / "install.log", "w") as log:
        exe = ensure_install(log)
    if exe is None:
        print(f"FAIL install the staged install could not be made, see {ROOT / 'work' / 'check' / 'install.log'}")
        print(summary_line(0, 1, time.time() - t0))
        return 1
    iso = Path(os.environ.get("DON_CHECK_ISO", ROOT / "en_patch/jpvoice/disc.iso")).resolve()
    tmp = Path("/tmp") / f"don-check-{os.getpid()}"
    tmp.mkdir()
    shots_root = ROOT / "work" / "check"
    wanted = set(args.only.split(",")) if args.only else None
    previous = {}
    try:
        with Xvfb(os.environ.get("DON_CHECK_DISPLAY", "97")) as x:
            for p in PATHS:
                if (p.full and not args.full) or (wanted and p.name not in wanted):
                    continue
                run_path(p, exe, x.display, tmp, shots_root, iso, libs, results, previous)
    finally:
        fix_modes(tmp)
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    passed = sum(1 for ok, _ in results if ok)
    failed = len(results) - passed
    print(summary_line(passed, failed, time.time() - t0))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
