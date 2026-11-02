"""Runs the committed routes of tools/run/scripts/ with the recompiled game and checks their landmarks (T-209).

Used by test_routes.py (the check) and record.py (the deliberate re-recording). Nothing here is the game: the
table of landmarks (routes.json) holds blank numbers, names, md5 sums and one-line descriptions.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "verify"))

import liveness  # noqa: E402

TABLE = Path(__file__).resolve().parent / "routes.json"
SUFFIX = ".exe" if sys.platform == "win32" else ""
SHOTS = ROOT / "work" / "regression"
VARIANTS = {"en-jpvoice": "en_patch/jpvoice/disc/SLPS_256.75"}
TIMEOUT = 3600


def load_table():
    return json.loads(TABLE.read_text())["routes"]


def binary():
    return Path(os.environ.get("DON_BINARY", ROOT / "build" / "linux-dev" / f"don{SUFFIX}"))


def variant_exe(variant):
    return ROOT / os.environ.get("DON_REGRESSION_EXE", VARIANTS[variant])


def skip_reason(variant):
    """Why the suite cannot run on this machine, or None."""
    if not binary().is_file():
        return f"no game binary at {binary()} (build it, or set DON_BINARY)"
    exe = variant_exe(variant)
    if not exe.is_file():
        return f"no variant folder at {exe.parent} (docs/RUNNING.md, the long way; or set DON_REGRESSION_EXE)"
    return None


def card_dir(route):
    """The card folder of a route that has "card": true (its own, under work/regression/; never the owner's)."""
    return SHOTS / f"{route['name']}-card"


def run_route(route, sound=False, step=None):
    """Runs the route (or its "then" step, a dict with its own script and landmarks, run on the same card folder);
    returns (exit code, shots folder, log path). A route with "card": true gets `--mc-dir` on its own folder, emptied
    before the first step."""
    step = step or route
    tag = route["name"] + ("-sound" if sound else "") + ("-then" if step is not route else "")
    shots = SHOTS / tag
    shots.mkdir(parents=True, exist_ok=True)
    for old in shots.glob("*.png"):
        old.unlink()
    log = SHOTS / f"{tag}.log"
    # A fresh user folder for every run (D15): never the owner's, and never what an earlier run left in it (a save
    # there changes the title's memory card message, and so the pictures).
    user = SHOTS / f"{tag}-user"
    if user.exists():
        shutil.rmtree(user)
    user.mkdir(parents=True)
    env = dict(os.environ, DON_TRACE_FRAMES="1", DON_USER_DIR=str(user), **route.get("env", {}))
    # A route without DON_PLAY_MOVIES keeps its timing: the port skips the movies after 240 blanks (a build with
    # FFmpeg plays them and waits for the player's Start otherwise). A route with DON_PLAY_MOVIES=1 has no automatic
    # skip at all (no player probe is installed): the Start taps of its own script are what skip the movie.
    env.setdefault("DON_MOVIE_SKIP_AFTER", "240")
    env.pop("DON_MC_DIR", None)
    cmd = [str(binary()), "--headless", "--renderer", "software", "--sound", "on" if sound else "off",
           "--shots", str(shots), "--input-script", str(ROOT / step["script"]), "--seconds", str(TIMEOUT)]
    if os.environ.get("DON_GAME_MODULE"):
        cmd += ["--game-module", os.environ["DON_GAME_MODULE"]]
    if route.get("card"):
        card = card_dir(route)
        if step is route and card.exists():
            shutil.rmtree(card)
        card.mkdir(parents=True, exist_ok=True)
        cmd += ["--mc-dir", str(card)]
    cmd.append(str(variant_exe(route["variant"])))
    with open(log, "wb") as out:
        code = subprocess.run(cmd, cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    return code, shots, log


def md5_of(path):
    return hashlib.md5(Path(path).read_bytes()).hexdigest()


def alive(log):
    return liveness.main([str(log)])
