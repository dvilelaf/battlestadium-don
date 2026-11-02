"""The launcher's folder rules and settings-file parsing."""
from __future__ import annotations

import os
from pathlib import Path


def user_dir(environ=None) -> Path:
    """The one user folder, the same rule as the program's src/host/user_dir.h (keep the two in step):
    DON_USER_DIR; Windows %APPDATA%\\don (else %USERPROFILE%\\AppData\\Roaming\\don); Linux
    $XDG_DATA_HOME/don (else $HOME/.local/share/don). Read from `environ` only, as the C++ does."""
    environ = os.environ if environ is None else environ
    if environ.get("DON_USER_DIR"):
        return Path(environ["DON_USER_DIR"])
    if os.name == "nt":
        if environ.get("APPDATA"):
            return Path(environ["APPDATA"]) / "don"
        if environ.get("USERPROFILE"):
            return Path(environ["USERPROFILE"]) / "AppData" / "Roaming" / "don"
        return Path(".") / "don"
    if environ.get("XDG_DATA_HOME"):
        return Path(environ["XDG_DATA_HOME"]) / "don"
    if environ.get("HOME"):
        return Path(environ["HOME"]) / ".local" / "share" / "don"
    return Path(".") / ".local" / "share" / "don"


def conf_dir(environ=None) -> Path:
    environ = os.environ if environ is None else environ
    if environ.get("DON_CONFIG_DIR"):
        return Path(environ["DON_CONFIG_DIR"])
    return user_dir(environ)  # launcher.conf lives in the one user folder (user_dir.h)


def variants_dir(environ=None) -> Path:
    environ = os.environ if environ is None else environ
    if environ.get("DON_VARIANTS_DIR"):
        return Path(environ["DON_VARIANTS_DIR"])
    return user_dir(environ) / "variants"  # the one user folder (user_dir.h), never a second rule


def parse_settings(text: str) -> dict[str, str]:
    """`NAME=value` lines; also `export NAME=value` and `: "${NAME=value}"`. Never executed."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith(":"):
            line = line[1:].strip()
            if line.startswith('"${') and line.endswith('}"'):
                line = line[3:-2]
            elif line.startswith("${") and line.endswith("}"):
                line = line[2:-1]
        if line.startswith("export "):
            line = line[7:].strip()
        if "=" not in line:
            continue
        name, value = line.split("=", 1)
        name, value = name.strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if name.replace("_", "").isalnum():
            out[name] = value
    return out


def read_settings_file(path: Path) -> dict[str, str]:
    try:
        return parse_settings(path.read_text(encoding="utf-8"))
    except OSError:
        return {}
