# Input scripts

A script is a text file that presses buttons for the game at given frames, takes screenshots and ends the run.
It lets the port be driven without a person: to reach a screen, to compare it, to run a smoke test. The parser
and the player are `src/host/input_script.*`, the host program connects them with its `--input-script FILE`
option (T-112, wired into `src/host/host.cpp`), and `tools/run/scripts/` holds the routes that are known to
work (`to_main_menu.txt`, `answer_memcard.txt`).

## Format

One command per line. `#` starts a comment (to the end of the line); blank lines are ignored.

```
# frame  [P2:]command   arguments
  600    tap            START 6          # press START for 6 frames
  900    press          DOWN
  906    release        DOWN
  1200   shot           menu             # saves <shots folder>/menu.png
  1500   quit
```

A line is: **frame**, an optional **player prefix**, the **command**, its **arguments**. Commands and button
names are case-insensitive (the canonical spelling is lower case for commands, upper case for buttons).

### Frame

- `600`: the absolute frame.
- `+30`: 30 frames after the frame of the **previous command line** (in the order of the file; `+0` is the
  same frame). The first line counts from frame 0. A relative frame makes a long route easy to edit: change
  one wait and everything after it moves. Absolute and relative frames can be mixed.

Lines do not have to be in frame order; the script is sorted by frame (commands of the same frame keep the
order of the file) before it is played.

### Commands

| Command | Meaning |
|---|---|
| `tap BUTTON FRAMES` | Hold the button for `FRAMES` frames (at least 1), starting at this frame: held at frames `F` to `F+FRAMES-1`. Two overlapping taps of the same button give one continuous press until the later one ends. |
| `press BUTTON` | Hold the button from this frame until a `release`. |
| `release BUTTON` | End a `press`. Harmless if the button was not held. It does not cut a `tap` short. If a `press` and a `release` of the button are on the same frame, the one later in the file wins. |
| `stick LEFT\|RIGHT X Y` | Set a stick, `X` and `Y` between -1 and 1 (decimals allowed: `0.5`). `X` positive is right, `Y` positive is **down** (as on the PS2 pad and in SDL): `stick LEFT 0 -1` is up. The position stays until the next `stick` command for that stick; `stick LEFT 0 0` returns it to the centre. No dead zone is applied: -1 is byte 0x00, 0 is 0x80, +1 is 0xFF. |
| `shot NAME` | Take a screenshot named `NAME` (letters, digits, `_`, `-`, `.`; it cannot start with `.`). The host saves it as `NAME.png` in its screenshots folder (`--shots`). Not per player. |
| `quit` | End the run (exit code 0). Not per player. |

### Buttons

`UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L1 L2 R1 R2 START SELECT L3 R3` (PS2 buttons, not the
position on a modern gamepad). In this game CIRCLE confirms and CROSS cancels.

### Players

An optional prefix `P1:` to `P4:` after the frame says which player the command is for; without it the
command is for player 1. The prefix may be attached (`P2:tap UP 4`) or separate (`P2: tap UP 4`). It is not
accepted on `shot` and `quit`. Players 2 to 4 only have a pad if the game was set up for them; a script that
mentions only player 1 does not plug in the others (the pad of a player is "connected" only if the script has at
least one button or stick command for that player).

## How frames are counted

A frame is a **presented frame**: one vertical blank of the game (60 per second on the real console), never
wall-clock time. The host calls the player once per presented frame with the number of that frame (1, 2, 3...),
before the game reads the pads for it. So `tap START 6` at frame 600 means the game sees START held in the
frames 600 to 605, whether the host runs at full speed, slower or without a window (`--headless`). A script
that works at one speed works at any other; what changes with the speed is only how long it takes.

A screenshot at frame `N` is the picture presented by frame `N`.

## Scripts and a person's devices

A script only **holds buttons**. The host merges the script with the devices of the player (keyboard and
gamepad): a button is pressed if it is pressed on either; for a stick the one farther from the centre wins. A
person can therefore take over during a script, and a script can run with no devices at all.

## Who steps the script, and why it is exact

The host program connects the player in one place, and the choice of thread is what makes "frame N" mean the
same thing in every run (T-112):

- the host GS calls `HostGs::set_on_vblank` **on the game thread** at the end of every vertical blank, with the
  number of that blank. The script is stepped *there*: the pad state the game reads during frame N is the state
  the script asked for at frame N, whatever the speed of the host (60 frames a second on a fast machine, 14 in
  the current port, and the same buttons in the same frames in both);
- the pad table (`src/host/pad_table.h`) allows one writer. The writer of the table the runtime reads is the
  **game thread**, in that hook: the devices (keyboard and gamepads) are gathered on the main thread and
  published into a *second* table, which the game thread reads and merges with the script's state
  (`input::merge_pad_states`). Two tables, one writer each, so the vblank hook never races with the main
  thread;
- the `shot` screenshots are written in the same hook (the picture of that frame) and `quit` asks the runtime
  to stop, which ends the run with exit code 0;
- with `--headless` there is no SDL event loop, so the host opens no keyboard or gamepad and player 1 is
  published as connected and neutral: the script is the only input of such a run.

## Errors

The whole file is read before anything runs. Every wrong line is reported with its line number and what is
wrong, for example `line 12: unknown button 'FOO' (UP DOWN LEFT ...)`, and **nothing runs** if there is any error.

## Example

`tools/run/scripts/to_main_menu.txt`: from power-on of the game to the main menu, translated from the first part
of `tools/ref/route.txt` (the route of the reference emulator). Its frame numbers are first estimates (60 per
second, with margins) that have to be corrected when the recompiled game reaches those screens.

```
600   shot  memcard_prompt
+30   tap   LEFT    6
+30   tap   CIRCLE  6
+6400 shot  title
+60   tap   START   6
+600  shot  main_menu
+30   quit
```

## Use

```bash
build/linux-release/don --input-script tools/run/scripts/to_main_menu.txt --shots work/shots/menu ...
```

(The option is wired in a later step.) The parser can also be exercised without the game by the tests:
`tests/input/` (`test_input_script`).
