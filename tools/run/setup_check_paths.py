"""The paths of the fast check (T-704): one `Path_` per run of `don`, the screen keys it presses and what it expects.

Imported by tools/run/setup_check.py, which puts this folder on sys.path (scripts run with `python -I`).
"""
from dataclasses import dataclass, field

# s: a path that has not exited by then hangs (its script ends in QUIT). Measured under AddressSanitizer (T-713): the paths
# that stop at 8 % of the preparing bar (drop-disc, disc-use-stop-early) take 56-84 s; 90 s timed out once on a warm run.
DEFAULT_PATH_TIMEOUT = 180
FULL_TIMEOUT = 3000         # s: preparing the game to the end
SECONDS_PER_KEY = 1.1       # measured: 40 frames of 16 ms plus the drawing
HOLD_EXTRA = 3.0            # s after the last key before a held program is stopped
QUIT_AT_DEFAULT = "8"       # percent of the preparing bar at which default mode closes the window (the disc stage is 5 %)


@dataclass
class Path_:
    name: str
    script: str
    home: str = "empty"            # "empty" | "disc" | "folders" | "gamefolder": which fake HOME; "{home}" in script = its path
    expected: list | None = None   # screen keys in order (None = not checked)
    quit_at: str | None = None     # DON_SETUP_QUIT_AT
    timeout: int = DEFAULT_PATH_TIMEOUT
    hold: float | None = None      # keep the program running this many s after its start, then stop it (no QUIT in the script)
    min_frames: int = 0            # distinct screen grabs the keys must have produced
    resume_of: str | None = None   # this path starts again on the user folder the named path left
    full: bool = False             # only with --full
    args: list = field(default_factory=list)   # command line of `don`
    env: dict = field(default_factory=dict)

    def __post_init__(self):
        if self.hold == 0.0:   # AUTO
            self.hold = (self.script.count(",") + 1 if self.script else 0) * SECONDS_PER_KEY + HOLD_EXTRA


# S1 has CHOOSE DISC IMAGE and QUIT (a third item, USE THIS DISC, first when discovery found a disc). The system file
# chooser fails on the virtual screen (no desktop portal), so CHOOSE opens the in-app browser (A4 E02): that is BROWSE.
# The browser is reached with BROWSE. The start places of the fake HOME are, in order, Downloads,
# Documents, /media/<user>, "/". Documents holds the folders a-empty, b-noperm, c-odd (spaces, accents, Japanese),
# d-loop (a link to its parent), e-many (a-sub and 5000 files), then the files junk.iso, notes.txt, trunc.iso.
AUTO = 0.0   # hold: as long as the keys need
BROWSE = "confirm"
DOCS = "down,confirm"   # in the places list: open Documents
IN = "down,confirm"     # a-empty is the first row of Documents; one "down" per row after it
PATHS = [
    Path_("s1-quit", "down,confirm", expected=["welcome"]),
    Path_("s1-keys", "left,right,up,up,down,up,back,down,up", expected=["welcome"], hold=AUTO),
    Path_("s1-choose-dialog", "confirm", expected=["welcome", "welcome-browser"], hold=AUTO),
    Path_("s1-choose-then-back", "confirm,back", expected=["welcome", "welcome-browser", "welcome"], hold=AUTO),
    Path_("browse-places", f"{BROWSE},down,down,down,up", home="folders", expected=["welcome", "welcome-browser"],
          hold=AUTO, min_frames=6),
    Path_("browse-in-out-back", f"{BROWSE},{DOCS},back,back", home="folders",
          expected=["welcome", "welcome-browser", "welcome"], hold=AUTO, min_frames=5),
    Path_("browse-empty-folder", f"{BROWSE},{DOCS},confirm,back", home="folders",
          expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    Path_("browse-unreadable-folder", f"{BROWSE},{DOCS},down,confirm,back", home="folders",
          expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    Path_("browse-odd-name-folder", f"{BROWSE},{DOCS},down,down,confirm,down,back", home="folders",
          expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    Path_("browse-loop-link", f"{BROWSE},{DOCS},down,down,down,confirm,back", home="folders",
          expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    Path_("browse-5000-files", f"{BROWSE},{DOCS},down,down,down,down,confirm,{','.join(['down'] * 25)},{','.join(['up'] * 5)},back",
          home="folders", expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    # T-713: confirm a folder row inside the large listing, go back to it, and do it again (the T-701 use after free read
    # the row's path after open_dir had cleared the entries; a-sub is row 0 of e-many, folders sort first).
    Path_("browse-5000-files-enter-folder", f"{BROWSE},{DOCS},down,down,down,down,confirm,confirm,back,confirm,back",
          home="folders", expected=["welcome", "welcome-browser"], hold=AUTO, min_frames=5),
    Path_("browse-escape-to-places-and-up", f"{BROWSE},up,up,back", home="folders",
          expected=["welcome", "welcome-browser", "welcome"], hold=AUTO),
    # The error codes are the ones the model gives (src/setup/disc_image.cpp): a file that is no disc image (notes.txt,
    # random junk.iso) is E15 = E-DISC-3; trunc.iso has a volume longer than the file: E08 = E-FILE-4.
    Path_("browse-choose-non-disc-file", f"{BROWSE},{DOCS},{','.join(['down'] * 6)},confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-DISC-3"]),
    Path_("browse-choose-junk-iso", f"{BROWSE},{DOCS},{','.join(['down'] * 5)},confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-DISC-3"]),
    Path_("browse-choose-truncated-iso", f"{BROWSE},{DOCS},{','.join(['down'] * 7)},confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-FILE-4"]),
    Path_("error-choose-another", f"{BROWSE},{DOCS},{','.join(['down'] * 5)},confirm" + ",confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-DISC-3", "welcome", "welcome-browser"]),
    Path_("error-copy-report", f"{BROWSE},{DOCS},{','.join(['down'] * 5)},confirm" + ",down,confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-DISC-3"]),
    Path_("error-dismiss-back", f"{BROWSE},{DOCS},{','.join(['down'] * 5)},confirm" + ",back", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-DISC-3", "welcome"]),
    Path_("error-try-again", f"{BROWSE},{DOCS},{','.join(['down'] * 7)},confirm" + ",confirm", home="folders", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-FILE-4", "checking", "checking-error E-FILE-4"]),
    Path_("disc-use-stop-early", "confirm", home="disc", expected=["welcome", "welcome-offer", "checking", "preparing"], quit_at=QUIT_AT_DEFAULT),
    Path_("resume-show", "", home="disc", resume_of="disc-use-stop-early", expected=["resume"], hold=AUTO),
    Path_("resume-continue", "confirm", home="disc", resume_of="disc-use-stop-early", hold=AUTO),
    # T-709: what the keys cannot reach. now:<key> is fed while busy, drop:<path> is a file dropped on the window.
    # STOP while preparing = confirm on the first item (STOP - CONTINUE LATER), which the model answers with the resume screen.
    Path_("stop-while-preparing", "confirm,now:confirm", home="disc", hold=AUTO,
          expected=["welcome", "welcome-offer", "checking", "preparing", "resume"]),
    Path_("drop-disc", "drop:{home}/Downloads/disc.iso", home="disc", quit_at=QUIT_AT_DEFAULT,
          expected=["welcome", "welcome-offer", "checking", "preparing"]),
    Path_("drop-not-disc", "drop:{home}/Documents/notes.txt", home="folders", hold=AUTO,
          expected=["welcome", "checking", "checking-error E-DISC-3"]),
    # Documents of the "gamefolder" home holds only g-game/ with a SYSTEM.CNF: the first row of Documents. Kind
    # DirectoryWithGame is chosen as a path, and the model answers E03 (disc_image.cpp: no game files in it).
    Path_("choose-game-folder", f"{BROWSE},{DOCS},confirm", home="gamefolder", hold=AUTO,
          expected=["welcome", "welcome-browser", "checking", "checking-error E-PICK-3"]),
    # --full: prepare to the end, press PLAY, wait for the first state line of the game (DON_TRACE_STATE prints one every
    # N blanks). --headless: the setup screens still use the window; the game then runs without one.
    Path_("full-prepare-and-play", "confirm,confirm", home="disc", full=True, timeout=FULL_TIMEOUT,
          args=["--headless"], env={"DON_TRACE_STATE": "60"},
          expected=["welcome", "welcome-offer", "checking", "preparing", "ready"]),
]
