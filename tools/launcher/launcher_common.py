"""Constants and errors shared by the launcher modules."""
from __future__ import annotations

ELF = "SLPS_256.75"
DATA_FILES = ("DATA.AFS", "BGM.AFS", "SE.AFS")
JP_FILES = ("SLPS_256.75", "SYSTEM.CNF", "DIR.DIR", "MODULES", "MOVIE.AFS")

# code -> (label shown, flag, setting that names its sources, checkout default)
VARIANTS = {
    "jp": ("日本語", "jp", None, None),
    "en-jpvoice": ("English · Japanese voices (日本語音声)", "uk", "DON_EN_JPVOICE", "en_patch/jpvoice"),
    "en-envoice": ("English · English voices", "uk", "DON_EN_ENVOICE", "en_patch/envoice"),
}
CODES = list(VARIANTS)

TITLE = "Battle Stadium D.O.N"
PROMPT = "Choose the language / 言語を選択"
HELP = "In the game: Enter = Start, L = confirm, K = back, arrows move."

AVAILABLE, PREPARE, MISSING = "available", "needs preparing", "missing"


class Cancelled(Exception):
    pass


class LauncherError(Exception):
    pass

