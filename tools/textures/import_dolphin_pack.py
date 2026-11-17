#!/usr/bin/env python3
"""Turns a Dolphin (GameCube) texture pack of Battle Stadium D.O.N into a texture pack for this port (issue #5).

The port loads PCSX2-style packs (docs/RUNNING.md, "Resolution and texture packs"): one PNG per PS2 texture, named
after PCSX2's hash of the PS2 texture. A Dolphin pack for the GameCube version (game ID G8SJ..) names its images
`tex1_<W>x<H>_<hash>..._<format>.png` after Dolphin's hash of the GameCube texture, and usually ships them scaled up.
The two hashes have nothing in common, but most textures are the same pictures in both versions. So this tool:

1. takes the PS2 textures dumped by the port itself (`--dump-textures`, the player's own game, nothing from here);
2. for each one, looks at the pack images whose original size (the `<W>x<H>` of the name) is the same, scales each
   down to that size and compares the two on a 16x16 thumbnail (colour weighted by opacity, and opacity);
3. takes the closest one when it is close and clearly closer than the second best, and writes the pack's image
   under the PS2 dump's name. When the PS2 texture keeps its alpha in the console's 0..128 range (0x80 = opaque),
   the image's 0..255 alpha is brought to that range, because PCSX2 uses a replacement's alpha as it is.

Textures that differ between the two versions (button icons, for example), that have another size, or that the dump
never saw are left out; the summary counts each case and the report (TSV) lists every match with its error.

    uv run --no-project --with pillow --with numpy python -I tools/textures/import_dolphin_pack.py \\
        --dumps "<user folder>/textures/SLPS-25675/dumps" --pack <folder with the tex1_*.png files> \\
        --out "<user folder>/textures/SLPS-25675/replacements"

Play with `--dump-textures` through every screen, stage and character you want covered first: a texture is only
imported if it was dumped.
"""
import argparse
import re
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

THUMB = 16
NAME = re.compile(r"tex1_(\d+)x(\d+)_")


def thumbnail(image: Image.Image, ps2: bool = False) -> np.ndarray:
    """16x16 RGBA features: colour premultiplied by opacity, and opacity, all 0..1."""
    rgba = np.asarray(image.convert("RGBA"), dtype=np.float32) / 255.0
    if ps2 and rgba[..., 3].max() <= 0.51:
        rgba = rgba.copy()
        rgba[..., 3] = np.clip(rgba[..., 3] * 2.0, 0.0, 1.0)  # the PS2's 0..128 alpha
    small = Image.fromarray((rgba * 255.0 + 0.5).astype(np.uint8), "RGBA").resize((THUMB, THUMB), Image.BOX)
    a = np.asarray(small, dtype=np.float32) / 255.0
    return np.concatenate([a[..., :3] * a[..., 3:4], a[..., 3:4]], axis=2)


def pack_index(pack: Path) -> dict:
    """{(width, height): [paths]} by the original size in each Dolphin file name."""
    index: dict = {}
    for path in sorted(pack.glob("*.png")):
        match = NAME.match(path.name)
        if match:
            index.setdefault((int(match[1]), int(match[2])), []).append(path)
    return index


def import_pack(dumps: Path, pack: Path, out: Path, max_error: float = 0.012) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    index = pack_index(pack)
    cache: dict = {}
    counts = {"dumped": 0, "imported": 0, "no_size": 0, "ambiguous": 0, "no_match": 0}
    report = []
    for dump in sorted(dumps.glob("*.png")):
        counts["dumped"] += 1
        image = Image.open(dump)
        candidates = index.get(image.size, [])
        if not candidates:
            counts["no_size"] += 1
            continue
        target = thumbnail(image, ps2=True)
        scores = []
        for path in candidates:
            if path not in cache:
                cache[path] = thumbnail(Image.open(path).resize(image.size, Image.BOX))
            scores.append((float(np.mean(np.abs(cache[path] - target))), path))
        scores.sort(key=lambda s: s[0])
        best, best_path = scores[0]
        second = scores[1][0] if len(scores) > 1 else 1.0
        if best > max_error * 4:
            counts["no_match"] += 1
            continue
        if best > max_error and second < best * 1.5:
            counts["ambiguous"] += 1
            continue
        destination = out / dump.name
        if np.asarray(image.convert("RGBA"))[..., 3].max() <= 128:
            hd = np.asarray(Image.open(best_path).convert("RGBA")).copy()
            hd[..., 3] = (hd[..., 3].astype(np.uint32) * 128 + 127) // 255
            Image.fromarray(hd, "RGBA").save(destination)
        else:
            shutil.copyfile(best_path, destination)
        counts["imported"] += 1
        report.append(f"{dump.name}\t{best_path.name}\t{best:.4f}\t{second:.4f}")
    counts["report"] = report
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dumps", type=Path, required=True, help="the port's dumps folder (--dump-textures)")
    parser.add_argument("--pack", type=Path, required=True, help="the Dolphin pack folder with the tex1_*.png files")
    parser.add_argument("--out", type=Path, required=True, help="the replacements folder to write")
    parser.add_argument("--max-error", type=float, default=0.012, help="accepted thumbnail error (default 0.012)")
    parser.add_argument("--report", type=Path, help="TSV with every import: dump, pack image, error, second best")
    args = parser.parse_args()
    for folder in (args.dumps, args.pack):
        if not folder.is_dir():
            print(f"error: {folder} is not a folder", file=sys.stderr)
            return 2
    counts = import_pack(args.dumps, args.pack, args.out, args.max_error)
    if args.report:
        args.report.write_text("\n".join(counts["report"]) + "\n", encoding="utf-8")
    print(f"dumped {counts['dumped']}: imported {counts['imported']}, no pack image of that size {counts['no_size']}, "
          f"ambiguous {counts['ambiguous']}, no match {counts['no_match']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
