# How to make a disc image of your own game

The program needs a disc image of *Battle Stadium D.O.N* (PlayStation 2, `SLPS-25675`) made from **a disc you own**.
This page explains what a valid image is and how to make one. It does not link to any place to download images, and
nobody in this project will provide one.

## What the program accepts

- A single file of the whole disc: **`.iso`**, or the same file compressed as **`.gz`** or inside a **`.zip`**.
- The disc is a DVD, so the image is about 1.7 GB (1.77 GB).
- Either the original Japanese disc, or that disc with the English fan translation by Lord Izen applied by you.
  The language of the game is the language of the image.
- Not accepted: images in other formats (`.bin`/`.cue`, `.chd`, `.cso`, `.nrg`, `.mdf`). Convert them to `.iso`
  first. An incomplete or still-downloading file is detected and refused.

The setup screen checks the image and tells you if it is the wrong disc, damaged or incomplete.

## Making the image

You need a PC (or a PS2) with a DVD drive and the disc itself.

**On Windows**
1. Insert the disc. Use any program that can copy a DVD to a plain `.iso` file (the optical-drive tools you
   already use for backups). Choose "ISO" / "raw sector copy" as the output.
2. Check the result is about 1.7 GB.

**On Linux**
1. Insert the disc and find the drive (for example `/dev/sr0`; `lsblk` lists it). Do not mount it.
2. Copy it:
   ```bash
   dd if=/dev/sr0 of=battlestadium-don.iso bs=2048 status=progress
   ```
   `bs=2048` is the sector size of a DVD. If the drive reports read errors, clean the disc and retry.

**On a PS2 or from the disc's file system only**
Copying just the files of the disc is not enough: the program needs the complete image, including its boot files.

## Checking it

- The size is close to 1.77 GB.
- The setup screen of the program says the disc was recognised (the game's id `SLPS-25675` is read from the image).
- If you have a hash database of your own dumps, compare there. The program does not need that.

## Where to put it

Pick the file in the setup screen when the program asks. It is read, never changed, and the program works from its
own copy of what it needs. Keep your image: the program needs it again only when you ask it to rebuild.

See also [LEGAL.md](../LEGAL.md): the image and everything generated from it are for your own use.
