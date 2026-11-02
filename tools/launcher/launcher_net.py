"""The launcher's net options and LAN addresses."""
from __future__ import annotations

import socket

from launcher_common import LauncherError


# The launcher's own net options (before `--`); their values follow, so they are not in `unknown`.
NET_OPTIONS = ("--host", "--port", "--players", "--delay", "--join")


def net_options(own):
    """The port's net arguments from the launcher's own options (before `--`).

    `--host [--port P] [--players N] [--delay D]` -> `--net-host P --net-players N` (+ `--net-delay D`);
    `--join HOST[:PORT]` -> `--net-join HOST:PORT` (port 7877 when not given).
    Returns (port_args, host_port): the arguments to prepend to the port's command, and the --host port as a
    string (for printing the LAN addresses) or None. Raises LauncherError on a conflict or a missing value.
    """
    host = False
    port = "7877"
    players = "2"
    delay = None
    join = None
    i = 0
    while i < len(own):
        a = own[i]
        if a == "--host":
            host = True
        elif a in ("--port", "--players", "--delay", "--join"):
            i += 1
            if i >= len(own):
                raise LauncherError(f"{a} needs a value")
            if a == "--port":
                port = own[i]
            elif a == "--players":
                players = own[i]
            elif a == "--delay":
                delay = own[i]
            else:
                join = own[i]
        i += 1
    if host and join is not None:
        raise LauncherError("--host and --join cannot be given together")
    if host:
        args = ["--net-host", port, "--net-players", players]
        if delay is not None:
            args += ["--net-delay", delay]
        return args, port
    if join is not None:
        target = join if ":" in join else f"{join}:7877"
        return ["--net-join", target], None
    return [], None


def local_ipv4_addresses():
    """The non-loopback IPv4 addresses of this PC, best effort (for `--host`): the source address chosen to
    reach a non-routable TEST-NET address, plus every non-loopback IPv4 the hostname resolves to."""
    addresses = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 80))
        addresses.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.append(info[4][0])
    except OSError:
        pass
    seen = []
    for address in addresses:
        if address and not address.startswith("127.") and address not in seen:
            seen.append(address)
    return seen

