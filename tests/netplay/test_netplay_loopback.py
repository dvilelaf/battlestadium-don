"""T-510: loopback runs of the whole game in net mode, two and four players.

N headless instances of the port on 127.0.0.1: the host's input script navigates the menus to the fight, the others
join and drive their own local player. Each case runs five minutes and is checked for three things: every instance
exits with 0, no log holds a desync, and every instance printed the same state lines (the host and the clients must
have hashed the same guest RAM at the same blank; see docs/knowledge/22-netplay.md section 6).

    uv run --no-project --with pytest python -I -m pytest tests/netplay -m slow -q

The game binary is build/linux-dev/don ($DON_BINARY) and the English variant en_patch/jpvoice/disc/SLPS_256.75
($DON_NETPLAY_EXE); the suite is skipped, with the reason, when either is missing. Logs stay in work/netplay/<case>/.
DON_NETPLAY_SECONDS (default 300) is the time box of each case, DON_NETPLAY_STATE_EVERY (default 240) the blank
interval of the state lines, DON_NETPLAY_RENDERER (default software) the GS renderer.

Two things this suite measured on the machine it was written on, both in docs/journal/040:

- `software` is not a preference: with the default Vulkan renderer two instances at once die at the same blank with
  SIGSEGV inside libvulkan_radeon (`CreateTFXPipeline`), and a fourth instance started with the others sometimes
  aborts on PCSX2's fixed `/GS.mem` name (`GS.cpp:1004`, `shm_open(..., O_CREAT | O_EXCL)`).
- The `4p-sim` case does not pass on this build. `DON_NET_SIM_LOSS=0.05` drops the clients' HASH datagrams, and the
  HASH is the one message the session never repeats: the client sends it once (`netplay.cpp:1085`) and the host waits
  `abort_after_ms` (30 s) for every one of them without asking again (`netplay.cpp:1090-1121`), so the host ends with
  "timed out waiting for the clients' hashes" and the clients with "timed out waiting for frame N". Every stall seen
  was at N = a multiple of `hash_every` (120) plus the session's delay. The case is left switched on: the lossy
  path is the card's, and the failure is a finding, not a flake to hide (AGENTS.md, rule 4).

The card says a client "presses attack buttons in the fight". That is not enough: a client's guest comes up on the
same menus as the host's, so its script must also press SELECT on the ENTRY EDITING screen and Circle at the
character select (tools/run/scripts/net_client_2p.txt, net_client4_*.txt); without them that player stays COM and
the fight is never reached. The 4-player scripts come from the four-player route of journal 026 (T-306).
"""
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent.parent
SUFFIX = ".exe" if sys.platform == "win32" else ""
SECONDS = int(os.environ.get("DON_NETPLAY_SECONDS", "300"))
STATE_EVERY = os.environ.get("DON_NETPLAY_STATE_EVERY", "240")
RENDERER = os.environ.get("DON_NETPLAY_RENDERER", "software")
LOGS = ROOT / "work" / "netplay"
STATE_LINE = re.compile(r"blank=(\d+).*ramHash=([0-9a-f]+)")

pytestmark = pytest.mark.slow

CASES = {
    "2p": {
        "players": 2,
        "host": "net_host_2p.txt",
        "clients": ["net_client_2p.txt"],
        # the named shots of the host's script: their absence means the match was never reached
        "shots": ["net_fight_a", "net_fight_b", "net_fight_c", "net_fight_d"],
    },
    "4p": {
        "players": 4,
        "host": "net_host_4p.txt",
        "clients": ["net_client4_2.txt", "net_client4_3.txt", "net_client4_4.txt"],
        "shots": ["match_end_a", "match_end_b"],
    },
    "4p-sim": {
        "players": 4,
        "host": "net_host_4p.txt",
        "clients": ["net_client4_2.txt", "net_client4_3.txt", "net_client4_4.txt"],
        "shots": ["match_end_a", "match_end_b"],
        "sim": True,
    },
}


def binary():
    return Path(os.environ.get("DON_BINARY", ROOT / "build" / "linux-dev" / f"don{SUFFIX}"))


def variant_exe():
    return Path(os.environ.get("DON_NETPLAY_EXE", ROOT / "en_patch" / "jpvoice" / "disc" / "SLPS_256.75"))


def skip_reason():
    if not binary().is_file():
        return f"no game binary at {binary()} (build it, or set DON_BINARY)"
    if not variant_exe().is_file():
        return f"no variant folder at {variant_exe()} (docs/RUNNING.md, the long way; or set DON_NETPLAY_EXE)"
    return None


def free_port():
    """A free UDP port for 127.0.0.1: the host binds it and every client joins it, so a case never leaves this PC."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def state_lines(log):
    """{blank: the rest of the state line} of `log` (DON_TRACE_STATE): the emulated cycle and instruction counters
    and the guest RAM hash the run printed at that blank. Two instances of one session must agree on all of it."""
    found = {}
    for line in log.read_text(errors="replace").splitlines():
        if "[don] state" not in line:
            continue
        match = STATE_LINE.search(line)
        if match:
            found[int(match.group(1))] = line.strip().split(f"blank={match.group(1)}", 1)[1].strip()
    return found


def run_case(case, spec, log_dir):
    """Starts the host and its clients, lets them play for SECONDS, stops them and returns [(name, log path, exit
    code)]. The clients are stopped first so the host's session ends the way a real player leaving does it."""
    port = free_port()
    log_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["DON_TRACE_STATE"] = STATE_EVERY
    env["TZ"] = "UTC"
    env["DON_USER_DIR"] = str(LOGS / "user-dir")  # T-524: never the owner's ~/.local/share/don
    env.pop("DON_MC_DIR", None)
    if spec.get("sim"):
        # the same shape the unit tests use (tests/net/test_net_netplay.cpp): 40 ms one way +- 10, 5% loss
        env["DON_NET_SIM_LATENCY_MS"] = "40"
        env["DON_NET_SIM_JITTER_MS"] = "10"
        env["DON_NET_SIM_LOSS"] = "0.05"
    else:
        for name in ("DON_NET_SIM_LATENCY_MS", "DON_NET_SIM_JITTER_MS", "DON_NET_SIM_LOSS"):
            env.pop(name, None)

    scripts = ROOT / "tools" / "run" / "scripts"
    # the scripts quit on their own at the end of their fight (~18000 blanks); --seconds is only the hard cap
    common = [str(binary()), "--headless", "--renderer", RENDERER, "--sound", "on", "--mute",
              "--seconds", str(SECONDS + 180)]

    def start(name, args, script):
        log = log_dir / f"{name}.log"
        with open(log, "wb") as out:
            proc = subprocess.Popen(common + args + ["--input-script", str(scripts / script),
                                                     str(variant_exe())], cwd=ROOT, env=env, stdout=out,
                                    stderr=subprocess.STDOUT)
        return name, log, proc

    started = []
    host = start("host", ["--net-host", str(port), "--net-players", str(spec["players"]),
                          "--shots", str(log_dir / "shots")], spec["host"])
    started.append(host)
    time.sleep(2)  # the host must be listening before the first client's hello
    for index, script in enumerate(spec["clients"]):
        started.append(start(f"client{index + 2}", ["--net-join", f"127.0.0.1:{port}"], script))
        time.sleep(0.5)

    deadline = time.monotonic() + SECONDS + 240
    while time.monotonic() < deadline:
        if all(proc.poll() is not None for _, _, proc in started):
            break
        time.sleep(1)
    for _, _, proc in reversed(started):  # clients first, then the host
        if proc.poll() is None:
            proc.terminate()
    for _, _, proc in started:
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return [(name, log, proc.returncode) for name, log, proc in started]


@pytest.mark.parametrize("case", list(CASES), ids=list(CASES))
def test_loopback(case):
    reason = skip_reason()
    if reason:
        pytest.skip(reason)
    spec = CASES[case]
    log_dir = LOGS / case
    runs = run_case(case, spec, log_dir)

    failures = [f"{name}: exited with {code} (log {log})" for name, log, code in runs if code != 0]
    for name, log, _ in runs:
        text = log.read_text(errors="replace")
        for line in text.splitlines():
            if "desync" in line.lower():
                failures.append(f"{name}: {line.strip()} (log {log})")

    lines = {name: state_lines(log) for name, log, _ in runs}
    common_blanks = set.intersection(*(set(found) for found in lines.values())) if lines else set()
    if len(common_blanks) < 2:
        failures.append("the instances share fewer than two state lines: "
                        + ", ".join(f"{name} {len(found)}" for name, found in lines.items()))
    else:
        host_lines = lines["host"]
        for name, found in lines.items():
            for blank in sorted(common_blanks):
                if found[blank] != host_lines[blank]:
                    failures.append(f"{name}: at blank {blank} it printed '{found[blank]}', the host printed "
                                    f"'{host_lines[blank]}' (the {len(common_blanks)} state lines they share are "
                                    f"compared; logs in {log_dir})")
                    break
    for shot in spec["shots"]:
        if not (log_dir / "shots" / f"{shot}.png").is_file():
            failures.append(f"the host never took {shot}.png: the script did not reach that point of the match "
                            f"(logs in {log_dir})")
    assert not failures, f"{case}: " + "\n".join(failures)
