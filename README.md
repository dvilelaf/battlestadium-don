# Battle Stadium D.O.N for PC

A project to bring *Battle Stadium D.O.N* (PlayStation 2, 2006, `SLPS-25675`) to PC, on
Windows and Linux, by static recompilation: the game code is translated to C++ once and
compiled as a native program.

**This repository does not contain the game.** Not the disc image, not the executable, not its
data, not the translated code. Each person generates it on their own machine from their own disc.

## Status

**Version 1.0: playable on Linux and Windows.** Downloads (the program only, no game material) are on the
[Releases](https://github.com/dvilelaf/battlestadium-don/releases) page: unpack, start the program and point it at your
own disc image; the first run checks the image and prepares the game on your PC (about 5-15 minutes). Menus, fights,
videos, sound, memory card and gamepads work. Online play for up to 4 players exists but has not been tried between two
PCs yet. The port needs a 64-bit processor with **AVX2** (Intel 2013 or later, AMD Ryzen); on Windows, a graphics card with a Vulkan driver.
The detail, task by task, is in [docs/STATUS.md](docs/STATUS.md). To build it from source:
[docs/BUILDING.md](docs/BUILDING.md), then [docs/RUNNING.md](docs/RUNNING.md).

## Scope of the first stage

- Windows and Linux (x86-64), with gamepad.
- 60 fps, sound, videos, memory card, 4 players.
- Japanese or English: the program is the same; the language is decided by the disc image that
  is used (the original Japanese one or the fan translation by Lord Izen).

Out of this stage: macOS, browser, more than 4 players, mods.

## Where to start

| If you want to... | Read |
|---|---|
| Know what has been found out about the game | [docs/knowledge/](docs/knowledge/README.md) |
| Understand why it is done this way | [docs/DECISIONS.md](docs/DECISIONS.md) |
| See the plan, the milestones and the risks | [docs/PLAN.md](docs/PLAN.md) |
| Pick up a task | [docs/TASKS.md](docs/TASKS.md) and first [AGENTS.md](AGENTS.md) |

## How it is laid out

```
config/     function map and recompiler configuration
patches/    patches on PS2Recomp and PCSX2 (cloned separately, at a pinned commit)
src/        original code: window and gamepads, game settings, bridges to PCSX2
tools/      scripts for analysis, setup, build and verification
tests/      tests that do not need the game
docs/       knowledge, decisions, plan, tasks, journal
```

## Credits

This project builds on [PS2Recomp](https://github.com/ran-j/PS2Recomp) and
[PCSX2](https://github.com/PCSX2/pcsx2), and learns from
[Fukami](https://github.com/danisandoval/fukami) and
[KillzoneRecomp](https://github.com/drakolordx7/KillzoneRecomp). The English translation is
the work of Lord Izen.

## License and legal notice

Code under GPL-3.0 ([LICENSE](LICENSE)). Every third-party piece and its licence: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). *Battle Stadium D.O.N* and its characters belong to their owners; this
project is not affiliated with them and does not distribute their material. You need your own
copy of the game.

More: [LEGAL.md](LEGAL.md) · how to make the disc image the program needs:
[docs/HOW_TO_MAKE_A_DISC_IMAGE.md](docs/HOW_TO_MAKE_A_DISC_IMAGE.md).
