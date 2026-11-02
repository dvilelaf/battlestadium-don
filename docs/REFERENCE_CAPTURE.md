# Reference captures with PCSX2 (task T-017)

This guide is for the person who plays the game. No programming knowledge is needed.

Since 2026-10-11 the captures are taken by an agent that drives the emulator on a virtual
display (task T-017, `tools/ref/pcsx2_auto.sh`). This guide stays as the manual way of doing the
same thing, in case the automatic one cannot be used.

**What this is for.** PCSX2 (a PS2 emulator) runs the same disc as the port. At six moments of
the game you save three files: a **GS dump** (the exact drawing commands of one frame), the
**screenshot** PCSX2 saves together with it (what that image looks like), and a **savestate**
(a snapshot of the console memory). The port is later checked against them. Everything stays
on your computer in `work/ref/pcsx2/` and is never committed (it contains game data).

**You do not have to hit any exact moment.** Any instant of the title screen, of a match and
so on is as good as any other, and nobody will ask you to repeat the same one. What makes the
files useful is that they belong together: the GS dump holds the drawing commands of one
frame, and replaying those commands must give that frame again, whatever the frame is.

## Before you start (once)

1. Download the emulator (about 60 MB):

   ```bash
   bash tools/setup/fetch_pcsx2_ref.sh
   ```

2. You need the BIOS of a PS2 console (file `ps2-0200j-20040614.bin`; the 0210j file shows only black in this PCSX2). It is **not** part of
   this project and it is never copied into it. Tell the scripts where the BIOS folder and the
   game disc image are: copy the file `local.env.example` to `local.env` (in the repository
   root) and replace the two example paths (`DON_ISO` and `DON_PS2_BIOS_DIR`) with the real
   ones. Git ignores `local.env`. On the development machine this is already done.

   The scripts read `local.env` by themselves, so there is nothing to export in each terminal.

## The two keys

Click on the PCSX2 game window first so that it has the keyboard focus.

| Key | What it does | File that appears in `work/ref/pcsx2/raw/` |
|---|---|---|
| **Shift + F8** | Saves a GS dump of **one** frame, and a screenshot of that same instant | `<game name>_<date>.gs.zst` and `<game name>_<date>.png` (same name, two files) |
| **F1** | Saves the savestate in slot 1 | `SLPS-25675 (xxxxxxxx).01.p2s` (extension `.p2s`) |

Notes:

- These are the default PCSX2 v2.8.2 hotkeys; nothing was changed. (Do not press Shift+Ctrl+F8:
  that records many frames.)
- **Do not press F8 alone.** It takes one more screenshot, of a later instant, and the script
  of step 3 would pick that one instead of the one that belongs to the dump. PCSX2 saves the
  dump and its screenshot together (`pcsx2/GS/Renderers/Common/GSRenderer.cpp`, lines 712-792
  at v2.8.2), so one key press is enough. If you did press it, press Shift + F8 again.
- The savestate is taken a moment later than the dump. That does not matter: it is used to
  look at the console memory, not to be compared with the image.
- The savestate always uses **slot 1**. Do not press F2 (it changes the slot).
- In this version a GS dump is a Zstandard-compressed file, so it ends in **`.gs.zst`**.
  (Older PCSX2 versions used `.gs.xz`; this one does not.) The scripts expect `.gs.zst`.
- The GS dump is written a moment after the key press. Wait about 3 seconds before step 3.

Game controls (PCSX2 defaults for the keyboard): arrows = D-pad, **K** = Cross, **L** = Circle,
**J** = Square, **I** = Triangle, **Enter** = Start, **Backspace** = Select, **Q/E** = L1/R1,
**1/3** = L2/R2. Space pauses and Tab fast-forwards (useful for long loads).

## Steps

1. Start the emulator with the game:

   ```bash
   bash tools/ref/run_pcsx2.sh
   ```

   (It uses the disc image from `local.env`. A different one can be given as an argument:
   `bash tools/ref/run_pcsx2.sh "/path/to/disc.iso"`.)
   The first time it prepares its configuration; this is automatic. It uses the Vulkan
   renderer at native resolution (1x), no widescreen patches, no cheats. Do not change these
   settings: otherwise the captures will not match what the port produces.
   Its own files live in `third_party/pcsx2-ref/`; nothing is written to your home folder.

2. Play until the game reaches the first moment of the table below. At that moment press
   **Shift + F8** and then **F1**. Wait about 3 seconds. In a match you can pause first with
   Space if you prefer, release the pause, and press the keys: the exact instant is not
   important.

3. In another terminal (from the repository root) run the command for that moment. It renames
   the three new files:

   ```bash
   bash tools/ref/name_capture.sh 01-title
   ```

   If it says a file is missing, press the missing key again (the game must be running) and
   repeat the command. Nothing is moved in that case.

4. Repeat steps 2 and 3 for every moment:

   | Name | Moment | After the keys, run |
   |---|---|---|
   | `01-title` | Title screen | `bash tools/ref/name_capture.sh 01-title` |
   | `02-menu` | Main menu | `bash tools/ref/name_capture.sh 02-menu` |
   | `03-charselect` | Character select | `bash tools/ref/name_capture.sh 03-charselect` |
   | `04-match-1v1` | A few seconds into a 1v1 match | `bash tools/ref/name_capture.sh 04-match-1v1` |
   | `05-match-4p` | A few seconds into a 4-player match | `bash tools/ref/name_capture.sh 05-match-4p` |
   | `06-results` | Results screen | `bash tools/ref/name_capture.sh 06-results` |

   Running a name again replaces the earlier capture with that name.

5. Write `work/ref/pcsx2/NOTES.md` (any text editor): anything that looks wrong in the
   emulator, which buttons you pressed to get from the title to a match, and roughly how many
   seconds each load takes.

## Check that everything is there

```bash
bash tools/ref/check_captures.sh; echo "exit=$?"
```

It lists the 18 expected files (6 moments x `.png`, `.p2s`, `.gs.zst`) as `present` or
`missing`. When it prints `18 of 18 present` and `exit=0`, the job is done.

## If something goes wrong

- *"DON_PS2_BIOS_DIR is not set"* or *"no disc image given"*: `local.env` is missing or has
  not been filled in; do step 2 of "Before you start".
- *The window does not open or shows a graphics error*: the log is
  `third_party/pcsx2-ref/PCSX2/logs/emulog.txt`; send its last lines.
- *You want to start again from scratch*: delete `third_party/pcsx2-ref/PCSX2/`; the next run
  recreates the configuration.
- Leftover files in `work/ref/pcsx2/raw/` (extra screenshots, `.backup` savestates) are
  harmless; only the newest file of each kind is used.
