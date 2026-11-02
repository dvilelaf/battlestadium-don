#!/usr/bin/env python3
"""Compare two images with SSIM and mean absolute difference.

Usage:
    uv run --no-project --with pillow --with numpy --with scikit-image python -I \
        tools/verify/image_parity.py a.png b.png [--min-ssim 0.90] \
        [--crop x,y,width,height] [--diff output.png]

Prints `SSIM=<4 decimals> MAD=<2 decimals> OK|FAIL` and exits with 0 when
SSIM >= --min-ssim, 1 otherwise.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity


def load_pair(path_a, path_b, crop=None):
    """Load both images as RGB, resize b to the size of a and apply the crop."""
    img_a = Image.open(path_a).convert("RGB")
    img_b = Image.open(path_b).convert("RGB")
    if img_b.size != img_a.size:
        img_b = img_b.resize(img_a.size, Image.BILINEAR)
    if crop is not None:
        x, y, width, height = crop
        box = (x, y, x + width, y + height)
        img_a = img_a.crop(box)
        img_b = img_b.crop(box)
    return np.asarray(img_a), np.asarray(img_b)


def compare(path_a, path_b, crop=None):
    """Return (ssim, mad) for two image files.

    ssim is the structural similarity over all channels (1.0 for identical images).
    mad is the mean absolute difference per channel value, on the 0-255 scale.
    """
    arr_a, arr_b = load_pair(path_a, path_b, crop)
    ssim = structural_similarity(arr_a, arr_b, channel_axis=2, data_range=255)
    mad = np.abs(arr_a.astype(np.float64) - arr_b.astype(np.float64)).mean()
    return float(ssim), float(mad)


def save_diff(path_a, path_b, out_path, crop=None):
    """Save the absolute difference amplified x4 as an image."""
    arr_a, arr_b = load_pair(path_a, path_b, crop)
    absdiff = np.abs(arr_a.astype(np.int16) - arr_b.astype(np.int16)) * 4
    Image.fromarray(np.clip(absdiff, 0, 255).astype(np.uint8)).save(out_path)


def parse_crop(text):
    """Parse 'x,y,width,height' into a tuple of four ints."""
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--crop expects x,y,width,height")
    return tuple(int(part) for part in parts)


def build_parser():
    parser = argparse.ArgumentParser(description="Compare two images with SSIM and MAD.")
    parser.add_argument("a", type=Path, help="reference image")
    parser.add_argument("b", type=Path, help="image to compare (resized to a if sizes differ)")
    parser.add_argument("--min-ssim", type=float, default=0.90, help="pass threshold (default 0.90)")
    parser.add_argument("--crop", type=parse_crop, default=None, help="region x,y,width,height")
    parser.add_argument("--diff", type=Path, default=None, help="save the amplified difference image")
    return parser


def main(argv):
    args = build_parser().parse_args(argv)
    ssim, mad = compare(args.a, args.b, args.crop)
    if args.diff is not None:
        save_diff(args.a, args.b, args.diff, args.crop)
    status = "OK" if ssim >= args.min_ssim else "FAIL"
    print(f"SSIM={ssim:.4f} MAD={mad:.2f} {status}")
    return 0 if status == "OK" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
