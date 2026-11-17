#!/usr/bin/env python3
"""Turns a Dolphin (GameCube) texture pack of Battle Stadium D.O.N into a texture pack for this port (issue #5).

The port loads PCSX2-style packs (docs/RUNNING.md, "Resolution and texture packs"): one PNG per PS2 texture, named
after PCSX2's hash of the PS2 texture. A Dolphin pack for the GameCube version (game ID G8SJ..) names its images
`tex1_<W>x<H>_<hash>..._<format>.png` after Dolphin's hash of the GameCube texture, and usually ships them scaled up.
The two hashes have nothing in common, but most textures are the same pictures in both versions. So this tool:

1. takes the PS2 textures dumped by the port itself (`--dump-textures`, the player's own game, nothing from here);
2. for each one, looks at the pack images whose original size (the `<W>x<H>` of the name) is the same, scales each
   down to that size and compares the two on a 16x16 thumbnail (colour weighted by opacity, and opacity);
3. takes the closest one when it is close and clearly closer than the second best, checks its native-size
   opacity, spatial colour and edge agreement, and writes the pack's image
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
    if ps2 and rgba[..., 3].max() <= 128.0 / 255.0 + 1e-7:
        rgba = rgba.copy()
        rgba[..., 3] = np.clip(rgba[..., 3] * (255.0 / 128.0), 0.0, 1.0)  # the PS2's 0..128 alpha
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


def correlation(a: np.ndarray, b: np.ndarray) -> float:
    """Zero for an empty or constant signal; no NaN for one-pixel textures."""
    if not a.size or not b.size:
        return 0.0
    a, b = a - a.mean(), b - b.mean()
    denominator = float(np.sqrt(np.sum(a * a) * np.sum(b * b)))
    return float(np.sum(a * b) / denominator) if denominator > 1e-12 else 0.0


def validate_pair(dump: Path, pack: Path, min_alpha: float = 0.75,
                  min_rgb: float = 0.85, min_edge: float = 0.50) -> tuple[bool, dict]:
    """Conservative native-size content check after thumbnail candidate selection.

    The alpha intersection/union and centred colour/gradient correlations catch small sparse images whose
    wrong shapes look close after averaging. Constant images have no usable correlation: only a genuinely
    constant pair may instead pass nearly exact premultiplied RGBA agreement, including its alpha.
    """
    with Image.open(dump) as image:
        size = image.size
        a = np.asarray(image.convert("RGBA"), dtype=np.float64) / 255.0
    if a[..., 3].max() <= 128.0 / 255.0 + 1e-8:
        a[..., 3] = np.minimum(a[..., 3] * (255.0 / 128.0), 1.0)
    with Image.open(pack) as image:
        b = np.asarray(image.convert("RGBA").resize(size, Image.Resampling.BOX), dtype=np.float64) / 255.0
    union = np.maximum(a[..., 3], b[..., 3])
    union_sum = float(union.sum())
    alpha = float(np.minimum(a[..., 3], b[..., 3]).sum() / union_sum) if union_sum else 1.0
    x, y = a[..., :3] * a[..., 3:], b[..., :3] * b[..., 3:]
    centred_x, centred_y = x - x.mean(axis=(0, 1)), y - y.mean(axis=(0, 1))
    rgb = correlation(centred_x, centred_y)
    dx = np.concatenate([np.diff(x, axis=0).ravel(), np.diff(x, axis=1).ravel()])
    dy = np.concatenate([np.diff(y, axis=0).ravel(), np.diff(y, axis=1).ravel()])
    edge = correlation(dx, dy)
    difference = float(np.abs(x - y).sum() + np.abs(a[..., 3] - b[..., 3]).sum())
    mae = difference / (4.0 * union_sum) if union_sum else 0.0
    features_a = np.concatenate([x, a[..., 3:]], axis=2)
    features_b = np.concatenate([y, b[..., 3:]], axis=2)
    flat = (np.max(np.abs(features_a - features_a.mean(axis=(0, 1)))) <= 1e-12
            and np.max(np.abs(features_b - features_b.mean(axis=(0, 1)))) <= 1e-12)
    exact_flat = flat and alpha >= 0.98 and mae <= 1.0 / 255.0
    accepted = (alpha >= min_alpha and rgb >= min_rgb and edge >= min_edge) or exact_flat
    return bool(accepted), {"alpha_overlap": alpha, "rgb_correlation": rgb,
                            "edge_correlation": edge, "content_mae": mae, "flat_fallback": bool(exact_flat)}


def import_pack(dumps: Path, pack: Path, out: Path, max_error: float = 0.012) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    # A rejected candidate must never leave an older replacement active. Do not remove or
    # overwrite a user's existing pack: require a fresh output folder for each conversion.
    if any(path.is_file() and path.suffix.lower() == ".png" for path in out.iterdir()):
        raise ValueError("output folder already contains PNG files; use a new empty output folder")
    index = pack_index(pack)
    cache: dict = {}
    counts = {"dumped": 0, "imported": 0, "no_size": 0, "ambiguous": 0, "no_match": 0, "rejected_content": 0}
    report, rejected_report = [], []
    for dump in sorted(dumps.glob("*.png")):
        counts["dumped"] += 1
        with Image.open(dump) as image:
            size = image.size
            ps2_alpha = np.asarray(image.convert("RGBA"))[..., 3].max() <= 128
            candidates = index.get(size, [])
            if not candidates:
                counts["no_size"] += 1
                continue
            target = thumbnail(image, ps2=True)
        scores = []
        for path in candidates:
            if path not in cache:
                with Image.open(path) as image:
                    cache[path] = thumbnail(image.resize(size, Image.Resampling.BOX))
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
        accepted, metrics = validate_pair(dump, best_path)
        # The first four columns are retained for existing report consumers; the remaining columns are
        # alpha overlap, colour correlation, gradient correlation, RGBA content error, flat fallback.
        row = (f"{dump.name}\t{best_path.name}\t{best:.4f}\t{second:.4f}\t"
               f"{metrics['alpha_overlap']:.4f}\t{metrics['rgb_correlation']:.4f}\t"
               f"{metrics['edge_correlation']:.4f}\t{metrics['content_mae']:.6f}\t{int(metrics['flat_fallback'])}")
        if not accepted:
            counts["rejected_content"] += 1
            rejected_report.append(row)
            continue
        destination = out / dump.name
        if ps2_alpha:
            with Image.open(best_path) as image:
                hd = np.asarray(image.convert("RGBA")).copy()
            hd[..., 3] = (hd[..., 3].astype(np.uint32) * 128 + 127) // 255
            Image.fromarray(hd, "RGBA").save(destination)
        else:
            shutil.copyfile(best_path, destination)
        counts["imported"] += 1
        report.append(row)
    counts["report"] = report
    counts["rejected_report"] = rejected_report
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dumps", type=Path, required=True, help="the port's dumps folder (--dump-textures)")
    parser.add_argument("--pack", type=Path, required=True, help="the Dolphin pack folder with the tex1_*.png files")
    parser.add_argument("--out", type=Path, required=True, help="the replacements folder to write")
    parser.add_argument("--max-error", type=float, default=0.012, help="accepted thumbnail error (default 0.012)")
    parser.add_argument("--report", type=Path, help="TSV with accepted pairs: dump, pack image, error, second best, alpha/RGB/edge/MAE metrics, flat flag")
    parser.add_argument("--rejected-report", type=Path, help="TSV of candidates rejected by native-size content checks")
    args = parser.parse_args()
    for folder in (args.dumps, args.pack):
        if not folder.is_dir():
            print(f"error: {folder} is not a folder", file=sys.stderr)
            return 2
    try:
        counts = import_pack(args.dumps, args.pack, args.out, args.max_error)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    if args.report:
        args.report.write_text("\n".join(counts["report"]) + "\n", encoding="utf-8")
    if args.rejected_report:
        args.rejected_report.write_text("\n".join(counts["rejected_report"]) + "\n", encoding="utf-8")
    print(f"dumped {counts['dumped']}: imported {counts['imported']}, no pack image of that size {counts['no_size']}, "
          f"ambiguous {counts['ambiguous']}, no match {counts['no_match']}, rejected content {counts['rejected_content']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
