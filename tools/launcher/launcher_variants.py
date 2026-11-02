"""Which variants exist and what each one still needs (nothing is made here)."""
from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

from launcher_common import AVAILABLE, DATA_FILES, JP_FILES, MISSING, PREPARE, VARIANTS, CODES
from launcher_paths import variants_dir

if TYPE_CHECKING:
    from launch import Settings


def has_files(folder: Path, names) -> bool:
    return all((folder / n).exists() for n in names)


def jp_base(settings: Settings):
    """(folder with the Japanese disc's files or None, image to extract from or None)."""
    folder = settings.path("DON_DISC_DIR", "disc")
    if folder and has_files(folder, JP_FILES + DATA_FILES):
        return folder, None
    iso = settings.path("DON_ISO")
    if iso and iso.is_file():
        return None, iso
    return None, None


def english_source(settings: Settings, code: str):
    """The path holding an English variant's data (image or folder), or None."""
    _label, _flag, setting, default = VARIANTS[code]
    base = settings.path(setting, default)
    if base is None:
        return None
    if base.is_file():
        return base
    if base.is_dir():
        if (base / "DATA.AFS").is_file():
            return base
        for sub in ("disc.iso", "x"):
            if (base / sub).is_file() or (base / sub / "DATA.AFS").is_file():
                return base / sub
    return None


def variant_folder(settings: Settings, code: str) -> Path:
    return variants_dir(settings.environ) / code


def folder_ready(folder: Path, code: str) -> bool:
    marker = folder / ".make_variant_dir.json"
    try:
        info = json.loads(marker.read_text())
    except (OSError, ValueError):
        return False
    return info.get("variant") == code and has_files(folder, JP_FILES + DATA_FILES)


def variant_state(settings: Settings, code: str):
    """(state, detail) for one variant. Nothing is made."""
    folder = variant_folder(settings, code)
    if folder_ready(folder, code):
        return AVAILABLE, str(folder)
    base, iso = jp_base(settings)
    problems = []
    if base is None and iso is None:
        problems.append("the Japanese disc's files (set DON_DISC_DIR or DON_ISO)")
    if base is None and iso is not None and not shutil.which("7z"):
        problems.append("7z (to read the Japanese image)")
    if code != "jp":
        source = english_source(settings, code)
        if source is None:
            problems.append(f"the English data (set {VARIANTS[code][2]})")
        elif source.is_file() and not shutil.which("7z"):
            problems.append("7z (to read the English image)")
    if problems:
        return MISSING, "missing " + " and ".join(problems)
    return PREPARE, "the folder is made on first use"


def variant_table(settings: Settings):
    return [(code, VARIANTS[code][0]) + variant_state(settings, code) for code in CODES]

