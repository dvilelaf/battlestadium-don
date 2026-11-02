Battle Stadium D.O.N - PC port (Windows, x64)
=============================================

This folder holds the program and its libraries. It does NOT hold the game: you need your own copy of the
game's files (from your own disc). Nothing here is from the game.

1. Requirements
---------------
Nothing to install for the program: the Microsoft Visual C++ runtime DLLs it needs are in this folder, beside
the program (msvcp140.dll, vcruntime140.dll and the others; see THIRD-PARTY.txt). A graphics card with a Vulkan
driver is needed (any recent driver for NVIDIA, AMD or Intel).

2. Unzip and put your game's files in it
---------------------------------------
Unzip this folder anywhere you like, for example C:\Games\DON.

Windows will very likely show a blue box the first time you start the program: "Windows protected your
PC", about an unrecognised app. That is only because this program is not signed with a paid certificate;
it is normal for free software. Click "More info" and then "Run anyway".

Your own game files go in a folder named `game` inside this folder, the one that holds "Battle Stadium
DON.exe". That is the folder with the game's data files (the contents of your disc, extracted to a folder
on your disk), the one that holds the file SLPS_256.75:

    Battle Stadium DON.exe      <- double-click this
    game\SLPS_256.75            <- your game's files, from your own disc
    gs\  vu\  spu2\  *.dll      <- the program's own files, do not move them

Your saves, settings and the log are not in this folder: they are in %APPDATA%\don (see 5 and 6), so a new
version unzipped anywhere keeps them.

3. Start
--------
Double-click "Battle Stadium DON.exe". It asks nothing. A window opens, the game starts.

To change something, open a command prompt in this folder and run the program with an option, for example:

   "Battle Stadium DON.exe" --sound off

Its messages always go to %APPDATA%\don\don.log; started from a command prompt they also appear in the
window. The options are:
   --sound off            no sound
   --renderer software    use the software renderer (default: vulkan)
   --upscale 2            draw at twice the resolution (1 to 8)
   --players 4            three or four players (a multitap is presented to the game)
   --seconds 60           end after 60 seconds (for tests)
No library option is needed: the program finds gs\, vu\ and spu2\ next to itself.

4. Keys and gamepad
-------------------
Enter = Start, L = Circle (confirm), K = Cross (cancel/back), J = Square, I = Triangle, arrow keys =
D-pad, W A S D = left stick, Q / E = L1 / R1, 1 / 3 = L2 / R2, Backspace = Select, C / V = L3 / R3.
Start with --players 4 for four players: gamepads fill players 2 to 4 (player 1 is the keyboard).
Esc, or closing the window, quits. A gamepad works by position: right face button = Circle (confirm),
bottom = Cross (cancel), left = Square, top = Triangle.

5. Saves
--------
Everything the program keeps for you is in one folder, %APPDATA%\don (type it in the Explorer's address
bar to open it):
    memcards\     your memory cards (mc0 and mc1)
    gs-cache\     the graphics cache (deleting it only brings back the first-time stutters)
    net.ini       the start screen's settings (name, address, host port...)
    don.log       what the program did (see 6)
Save from the game's main menu: OPTIONS, MEMORY CARD, SAVE. Copy memcards\ to keep your saves. A version
older than this one kept the saves in a `saves` folder beside the program: this one copies them to
memcards\ the first time, if memcards\ has none (unzip the new version over the old folder, or copy the old
`saves` folder beside the new program, before the first start).

6. If something goes wrong
--------------------------
The program writes what it does to %APPDATA%\don\don.log. Every start is added at the end of it, under a
line with the date and time, so the start that went wrong is still there after you start the game again
(past 10 MB the file becomes `don.old.log`). The log also says which graphics card and driver were used,
the frame rate every 10 seconds, the controllers it saw, and whether the previous start ended badly. Send
that file with a report of a problem. `BUILT_FROM.txt` says which version of the port this folder is.

7. What does not work yet
-------------------------
(State of 2026-10-14.)
- Videos are not played; they are skipped (a few seconds of black before the title).
- The music plays, but the port has only been tried on a few menus and fights; some sounds may be missing.
- In a window there can be small stutters at moments.
- Two players by default (a second gamepad); the keyboard is player 1 only. For three or four players start
  with --players 4 (not tried with real gamepads). No button remapping from the port (the game's own four
  layouts work) and no vibration.
- Picking up and throwing items, and throwing an opponent, have not been seen working.
- This folder has only been run under Wine on Linux, headless: not on a real Windows PC yet, and no window
  has been tried.

8. Licence and source
---------------------
The port is free software under the GNU General Public License v3.0 (file LICENSE). Its source code,
including the patches to PS2Recomp and PCSX2 that it contains, is at:

   SOURCE: <address to be filled in by whoever distributes this folder>

The libraries in this folder and their licences are in THIRD-PARTY.txt.
