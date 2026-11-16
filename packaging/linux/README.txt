Battle Stadium D.O.N for PC (Linux)
===================================

This download contains the program only. It contains nothing of the game, and it downloads nothing.
You need your own disc image of the PS2 game (SLPS-25675), made from a disc you own.

1. Extract this archive anywhere you can write to:   tar xzf don-linux-<version>.tar.gz
2. Run ./don from the extracted folder.
3. Point it at your own disc image when it asks (or run  ./don --prepare /path/to/your-image.iso ).
   The first preparation translates and compiles the game's code on your PC from your disc; it takes a while
   and the result stays on your PC. Nothing is uploaded.

Licence: GPL-3.0-or-later (LICENSE). Third-party licences: THIRD_PARTY_NOTICES.md and licenses/.
Source code: SOURCE.txt. Legal notes: LEGAL.md.

Options (after ./don; most are also in the start screen's Settings):
   --upscale 2            draw at twice the resolution (1 to 8)
   --textures on|off      load a texture pack (PCSX2 format; Vulkan only)
   --textures-dir <dir>   the folder of the texture packs (default: textures/SLPS-25675/replacements in the user folder)
   --dump-textures        write the game's own textures out, the starting point of a pack
   --widescreen on|off    16:9 fights: more of the stage at the sides (menus stay 4:3)
The disc image can be an .iso or a .chd file.
