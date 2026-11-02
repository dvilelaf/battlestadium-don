import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

# python -I does not add the script folder to sys.path; the tool lives in tools/verify/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import image_parity  # noqa: E402


def make_image(path, size=(64, 48), color=(120, 60, 200)):
    """Write a synthetic image with a gradient so SSIM has structure to compare."""
    width, height = size
    xs = np.linspace(0, 255, width, dtype=np.uint8)
    arr = np.zeros((height, width, 3), dtype=np.uint8)
    arr[:, :, 0] = xs[None, :]
    arr[:, :, 1] = color[1]
    arr[:, :, 2] = color[2]
    Image.fromarray(arr).save(path)
    return path


def make_changed_rectangle(source, target, box=(10, 10, 40, 30)):
    """Copy source and paint a black rectangle over box."""
    arr = np.asarray(Image.open(source).convert("RGB")).copy()
    x0, y0, x1, y1 = box
    arr[y0:y1, x0:x1] = 0
    Image.fromarray(arr).save(target)
    return target


def test_identical_images(tmp_path):
    a = make_image(tmp_path / "a.png")
    b = make_image(tmp_path / "b.png")
    ssim, mad = image_parity.compare(a, b)
    assert ssim == pytest.approx(1.0)
    assert mad == 0.0


def test_changed_rectangle_lowers_ssim(tmp_path):
    a = make_image(tmp_path / "a.png")
    b = make_changed_rectangle(a, tmp_path / "b.png")
    ssim, mad = image_parity.compare(a, b)
    assert ssim < 1.0
    assert mad > 0.0


def test_different_sizes_are_resized_to_first_image(tmp_path):
    a = make_image(tmp_path / "a.png", size=(64, 48))
    b = make_image(tmp_path / "b.png", size=(128, 96))
    ssim, mad = image_parity.compare(a, b)
    # b is the same gradient at twice the size, so after the bilinear resize it is close to a.
    assert ssim > 0.9
    assert mad < 10.0


def test_crop_ignores_changes_outside_region(tmp_path):
    a = make_image(tmp_path / "a.png")
    b = make_changed_rectangle(a, tmp_path / "b.png", box=(10, 10, 40, 30))
    # The crop (50,35)-(64,48) does not touch the changed rectangle (10,10)-(40,30).
    ssim_full, _ = image_parity.compare(a, b)
    ssim_crop, mad_crop = image_parity.compare(a, b, crop=(50, 35, 14, 13))
    assert ssim_full < 1.0
    assert ssim_crop == pytest.approx(1.0)
    assert mad_crop == 0.0


def test_main_prints_ok_and_returns_zero(tmp_path, capsys):
    a = make_image(tmp_path / "a.png")
    b = make_image(tmp_path / "b.png")
    code = image_parity.main([str(a), str(b)])
    out = capsys.readouterr().out.strip()
    assert code == 0
    assert out == "SSIM=1.0000 MAD=0.00 OK"


def test_main_min_ssim_fails_on_different_images(tmp_path, capsys):
    a = make_image(tmp_path / "a.png")
    b = make_changed_rectangle(a, tmp_path / "b.png", box=(0, 0, 64, 48))
    code = image_parity.main([str(a), str(b), "--min-ssim", "0.90"])
    out = capsys.readouterr().out.strip()
    assert code == 1
    assert out.endswith("FAIL")


def test_main_crop_argument(tmp_path, capsys):
    a = make_image(tmp_path / "a.png")
    b = make_changed_rectangle(a, tmp_path / "b.png", box=(10, 10, 40, 30))
    code = image_parity.main([str(a), str(b), "--crop", "50,35,14,13"])
    out = capsys.readouterr().out.strip()
    assert code == 0
    assert out == "SSIM=1.0000 MAD=0.00 OK"
