#!/usr/bin/env python3
"""Launcher and language selector for the Battle Stadium D.O.N port.

Adapted from the owner's other project, repository `f7gs`, file `runtime/scripts/launch.py`
(commit 539b594, "Merge integ/ep1: D321-D325, --save-at ..."), at the request of the owner of both
projects. Kept from that file: the Tk picker with a terminal fallback, flags drawn without image
files, the remembered choice, a code on the command line that skips the picker, `--print`, the
pass-through of the arguments after `--`, the environment overrides, cancelling without starting
anything, and the structure that lets a test replace the pieces. Dropped: its seven languages and
disc sets, the icon read out of a Windows executable, and everything about that engine's options.

    launch.py                 # picker (the last choice is preselected), then start
    launch.py en-jpvoice      # skip the picker, remember the choice, start
    launch.py --print [code]  # print the command instead of starting (nothing is prepared)
    launch.py --list          # the variants and their state, for a terminal without a display
    launch.py jp -- --headless --seconds 20     # what follows `--` goes to the port unchanged
    launch.py jp --host [--port P] [--players N] [--delay D]   # host an online session (port 7877 by default)
    launch.py jp --join HOST[:PORT]             # join a session (port 7877 by default); nothing to install

Variants (the names tools/disc/verify_disc.py and make_variant_dir.py use): `jp` (the Japanese disc),
`en-jpvoice` (English, Japanese voices), `en-envoice` (English, English voices). The executable is the
same for the three; the variant selects the folder of game files the port is started on.

Where things are looked for, for each setting, in this order:
  1. the launcher's settings file, `<user>/launcher.conf` (`NAME=value` lines; <user> = the one
     user folder of src/host/user_dir.h, DON_USER_DIR first);
  2. the environment;
  3. `local.env` at the repository root (read as text, never executed; both `NAME=value` and the
     `: "${NAME=value}"` form of local.env.example);
  4. the checkout's conventional places.
Settings: DON_BIN (the port's executable; default build/player/don-linux/don when that player folder exists,
else build/linux-dev/don), DON_DISC_DIR (the Japanese
disc's extracted files; default disc/), DON_ISO (the Japanese disc image, used to extract the files
when DON_DISC_DIR lacks them), DON_EN_JPVOICE and DON_EN_ENVOICE (an English variant's image, a folder
with its extracted DATA.AFS, or a folder holding disc.iso; default en_patch/jpvoice and en_patch/envoice).
Two more, for tests and unusual setups: DON_VARIANTS_DIR (where the variant folders are made; default
<user>/variants, <user> = the one user folder of src/host/user_dir.h, so DON_USER_DIR moves it too) and
DON_CONFIG_DIR (the folder of launcher.conf; default the user folder).
DON_MC_DIR is the folder of the memory cards (it holds mc0 and mc1; default <user>/memcards):
the same cards for the three variants, passed to the port as `--mc-dir`. A save made before this
change sits in a variant's own folder (`<variant folder>/mc0`); the launcher says so in one line when
the shared folder has none, and neither copies nor deletes anything. The launcher's own choice is saved in launcher.conf as
`LAST_VARIANT=<code>`.

A variant's folder is made, the first time it is chosen, by tools/disc/make_variant_dir.py imported as
a module (so its errors come back as exceptions, no second interpreter is needed and a test can replace
it). Cancelling the picker (Cancel, closing the window, empty input) exits 0 WITHOUT starting anything.

The command is `<port> <variant folder>/SLPS_256.75 [arguments after --]`: no `--iso`. The port reads
every byte from the folder (journal 021), and the IOP's CD code tries an image first, at sector numbers
it derived from the folder, so a wrong image (the Japanese one named by DON_ISO) could poison an
English run. The launcher therefore removes DON_ISO from the port's environment. To give an image
anyway, put `--iso <file>` after `--`.
"""
from __future__ import annotations

import importlib.util
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

# `python -I` keeps this folder off sys.path, and the tests load this file by path.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from launcher_common import (AVAILABLE, CODES, DATA_FILES, ELF, HELP, JP_FILES, MISSING, PREPARE,  # noqa: E402,F401
                             PROMPT, TITLE, VARIANTS, Cancelled, LauncherError)
from launcher_net import NET_OPTIONS, local_ipv4_addresses, net_options  # noqa: E402,F401
from launcher_paths import (conf_dir, parse_settings, read_settings_file, user_dir,  # noqa: E402,F401
                            variants_dir)
from launcher_ui import make_flag, pick, pick_gui, pick_terminal  # noqa: E402,F401
from launcher_variants import (english_source, folder_ready, has_files, jp_base, variant_folder,  # noqa: E402,F401
                               variant_state, variant_table)

ROOT = Path(__file__).resolve().parent.parent.parent
CONF_NAME = "launcher.conf"
LAST_KEY = "LAST_VARIANT"



class Settings:
    """Look a setting up: launcher.conf, environment, local.env, then the checkout's default."""

    def __init__(self, environ=None, root: Path | None = None):
        self.environ = os.environ if environ is None else environ
        self.root = ROOT if root is None else Path(root)
        self.conf_path = conf_dir(self.environ) / CONF_NAME
        self._migrate_conf()
        self.file = read_settings_file(self.conf_path)
        self.local = read_settings_file(self.root / "local.env")

    def _migrate_conf(self):
        # T-524: copy-only — the old ~/.config/don/launcher.conf moves to the user folder (never moved).
        # Only for the default location; an explicit DON_CONFIG_DIR (tests) means no migration.
        if os.name == "nt" or self.environ.get("DON_CONFIG_DIR"):
            return
        if self.conf_path.exists():
            return
        base = self.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
        old = Path(base) / "don" / CONF_NAME
        if old.is_file():
            try:
                self.conf_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(old, self.conf_path)
            except OSError:
                pass

    def get(self, name: str, default: str | None = None) -> str | None:
        for source in (self.file, self.environ, self.local):
            if source.get(name):
                return source[name]
        return default

    def path(self, name: str, default: str | None = None) -> Path | None:
        value = self.get(name)
        if value is None and default is not None:
            return self.root / default
        return Path(value).expanduser() if value else None

    def memcards(self) -> Path:
        """The folder that holds mc0 and mc1: DON_MC_DIR, else <user>/memcards (the program's own default)."""
        return self.path("DON_MC_DIR") or user_dir(self.environ) / "memcards"

    def last(self) -> str:
        value = self.file.get(LAST_KEY, "")
        return value if value in VARIANTS else ""   # an unknown variant in the file is ignored

    def remember(self, code: str) -> None:
        lines = []
        try:
            lines = [ln for ln in self.conf_path.read_text(encoding="utf-8").splitlines()
                     if not ln.strip().startswith(LAST_KEY + "=")]
        except OSError:
            pass
        lines.append(f"{LAST_KEY}={code}")
        try:
            self.conf_path.parent.mkdir(parents=True, exist_ok=True)
            self.conf_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        except OSError:
            pass
        self.file[LAST_KEY] = code


# ---------------------------------------------------------------- variants

def load_make_variant_dir():
    """tools/disc/make_variant_dir.py as a module (imported by path: `-I` keeps its folder off sys.path)."""
    path = ROOT / "tools" / "disc" / "make_variant_dir.py"
    spec = importlib.util.spec_from_file_location("don_make_variant_dir", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module



def say_now(text: str) -> None:
    print(text, flush=True)


def prepare(settings: Settings, code: str, say=say_now) -> Path:
    """Make the variant's folder when it is not there. Returns the folder."""
    folder = variant_folder(settings, code)
    if folder_ready(folder, code):
        return folder
    maker = load_make_variant_dir()
    base, iso = jp_base(settings)
    if base is None:
        if iso is None:
            raise LauncherError("the Japanese disc's files are missing (set DON_DISC_DIR or DON_ISO)")
        base = variants_dir(settings.environ) / "_jp-files"
        say(f"Extracting the Japanese disc's files into {base} (first start only) ...")
        extract_japanese(iso, base)
    source = base if code == "jp" else english_source(settings, code)
    if source is None:
        raise LauncherError(f"the English data are missing (set {VARIANTS[code][2]})")
    say(f"Preparing the folder for {VARIANTS[code][0]} in {folder} (first start only) ...")
    try:
        found, _made = maker.make(base, source, folder)
    except (maker.MakeError, OSError) as error:
        raise LauncherError(str(error)) from error
    if found != code:
        raise LauncherError(f"{source} holds the variant {found or 'unknown'}, not {code}")
    return folder


def extract_japanese(iso: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(["7z", "x", "-y", f"-o{dest}", str(iso), *JP_FILES, *DATA_FILES],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as error:
        raise LauncherError(f"cannot run 7z: {error}") from error
    if result.returncode != 0:
        raise LauncherError(f"7z could not read {iso}")


def default_bin(settings: Settings) -> Path:
    found = settings.path("DON_BIN")
    if found:
        return found
    name = "don.exe" if os.name == "nt" else "don"
    player = settings.root / "build" / "player" / "don-linux" / name
    if os.name != "nt" and player.is_file():
        return player          # the player folder (tools/pipeline/package_linux.py): changes only when it is rebuilt
    return settings.root / "build" / "linux-dev" / name


def describe_bin(settings: Settings, binary: Path) -> str:
    """One line for the player: which binary runs and, for the player folder, the commit it was built from."""
    built = binary.parent / "BUILT_FROM.txt"
    if built.is_file():
        lines = dict(line.split(" ", 1) for line in built.read_text().splitlines() if " " in line)
        return f"port: {binary} (player folder, commit {lines.get('commit', '?')[:12]}: {lines.get('subject', '')[:70]})"
    if settings.path("DON_BIN"):
        return f"port: {binary} (DON_BIN)"
    return f"port: {binary} (the development build of this checkout, not a player folder)"


def gs_cache(environ=None) -> Path:
    """The GS library's shader/pipeline cache: <user>/gs-cache (the program's own default)."""
    return user_dir(environ) / "gs-cache"


def build_command(settings: Settings, code: str, passthrough, folder: Path | None = None, net=()):
    """(argv, environment) of the port for a variant. The port gets no DON_ISO and no --mc-dir /
    DON_GS_DATA_DIR: the program decides those from its own user folder. `net` is the port's net
    options (from net_options), placed before the `--` arguments."""
    folder = folder or variant_folder(settings, code)
    argv = [str(default_bin(settings)), str(folder / ELF), *net, *passthrough]
    env = {k: v for k, v in os.environ.items() if k != "DON_ISO" and k != "DON_GS_DATA_DIR"}
    return argv, env


def has_card_files(folder: Path) -> bool:
    return any(p.is_file() for card in ("mc0", "mc1") for p in (folder / card).rglob("*"))


def legacy_saves_note(settings: Settings):
    """One line when a variant's own folder holds a save from before the shared cards and the shared
    folder has none; None otherwise. Nothing is copied or deleted."""
    if has_card_files(settings.memcards()):
        return None
    old = [variant_folder(settings, c) for c in CODES if has_card_files(variant_folder(settings, c))]
    if not old:
        return None
    return (f"note: a save from before the shared cards is in {old[0]} (mc0); the cards are now in "
            f"{settings.memcards()} and that save is not used: copy mc0/mc1 there to keep it")


def format_command(argv) -> str:
    return subprocess.list2cmdline(argv) if os.name == "nt" else shlex.join(argv)


# ---------------------------------------------------------------- main

def split_args(argv):
    """(launcher arguments, arguments after the first `--`); argv[0] is the program."""
    rest = list(argv[1:])
    if "--" in rest:
        i = rest.index("--")
        return rest[:i], rest[i + 1:]
    return rest, []


def main(argv) -> int:
    own, passthrough = split_args(argv)
    if "-h" in own or "--help" in own:
        print(__doc__)
        return 0
    want_print, want_list = "--print" in own, "--list" in own
    words = [a for a in own if not a.startswith("-")]
    unknown = [a for a in own if a.startswith("-") and a not in ("--print", "--list", "-h", "--help")
               and a not in NET_OPTIONS]
    if unknown:
        print(f"unknown option {unknown[0]} (arguments for the port go after `--`)", file=sys.stderr)
        return 2
    try:
        net, host_port = net_options(own)
    except LauncherError as error:
        print(str(error), file=sys.stderr)
        return 2
    settings = Settings()

    if want_list:
        for code, label, state, detail in variant_table(settings):
            print(f"{code:<11} {state:<15} {label}  -- {detail}")
        print(f"memory cards: {settings.memcards()} (DON_MC_DIR)")
        note = legacy_saves_note(settings)
        if note:
            print(note)
        return 0

    table = variant_table(settings)
    states = {code: (state, detail) for code, _l, state, detail in table}
    if words:
        code = words[0].lower()
        if code not in VARIANTS:
            print(f"unknown variant {words[0]!r}; choose one of: {', '.join(CODES)}", file=sys.stderr)
            return 2
    elif want_print:
        usable = [c for c in CODES if states[c][0] != MISSING]
        code = settings.last() if settings.last() in usable else (usable[0] if usable else "")
        if not code:
            print("no variant is available (see --list)", file=sys.stderr)
            return 1
    else:
        try:
            code = pick(table, settings.last())
        except Cancelled:
            return 0                           # cancelled: do NOT start
    if states[code][0] == MISSING:
        print(f"{VARIANTS[code][0]} is not available: {states[code][1]}", file=sys.stderr)
        return 1

    if want_print:
        argv_out, _env = build_command(settings, code, passthrough, net=net)
        print(format_command(argv_out))
        print("(DON_ISO is removed from the port's environment; the folder is made on first start "
              "if it does not exist)", file=sys.stderr)
        return 0

    try:
        folder = prepare(settings, code)
    except LauncherError as error:
        print(f"cannot prepare {code}: {error}", file=sys.stderr)
        return 1
    settings.remember(code)
    argv_out, env = build_command(settings, code, passthrough, folder, net=net)
    if not os.path.exists(argv_out[0]):
        print(f"the port is not at {argv_out[0]} (set DON_BIN)", file=sys.stderr)
        return 1
    note = legacy_saves_note(settings)
    if note:
        print(note, flush=True)
    print(describe_bin(settings, Path(argv_out[0])), flush=True)
    print(f"Starting: {VARIANTS[code][0]} ({code})", flush=True)
    if host_port is not None:
        addresses = local_ipv4_addresses()
        for address in addresses:
            print(f"players on this LAN can join at {address}:{host_port}", flush=True)
        if not addresses:
            print("(no non-loopback IPv4 address found on this PC)", flush=True)
    try:
        return subprocess.call(argv_out, env=env)
    except OSError as error:
        print(f"launch failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
