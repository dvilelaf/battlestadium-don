"""tools/textures/import_dolphin_pack.py on synthetic images (no game file): a PS2 dump is matched to the pack image of
the same picture and original size, its alpha is brought to the PS2's 0..128 range, and a picture with no counterpart,
an ambiguous one and one of another size are left out."""
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "textures"))
import import_dolphin_pack as tool  # noqa: E402


def picture(seed: int, size=(32, 32)) -> np.ndarray:
    rng = np.random.default_rng(seed)
    small = rng.integers(0, 256, (4, 4, 3), dtype=np.uint8)  # big blocks, so a thumbnail keeps them
    rgb = np.asarray(Image.fromarray(small, "RGB").resize(size, Image.NEAREST))
    return np.dstack([rgb, np.full(size[::-1], 255, np.uint8)])


def save(path: Path, rgba: np.ndarray, scale: int = 1, ps2_alpha: bool = False) -> None:
    a = rgba.copy()
    if ps2_alpha:
        a[..., 3] = a[..., 3] // 2 + (a[..., 3] > 0)  # 255 -> 128
    image = Image.fromarray(a, "RGBA")
    if scale != 1:
        image = image.resize((image.width * scale, image.height * scale), Image.NEAREST)
    image.save(path)


def test_import(tmp_path: Path) -> None:
    dumps, pack, out = tmp_path / "dumps", tmp_path / "pack", tmp_path / "out"
    dumps.mkdir()
    pack.mkdir()
    # The pack: two pictures at 3x, named with their original 32x32 size, and one 64x64 picture.
    save(pack / "tex1_32x32_aaaa_9.png", picture(1), scale=3)
    save(pack / "tex1_32x32_bbbb_9.png", picture(2), scale=3)
    save(pack / "tex1_64x64_cccc_9.png", picture(3, (64, 64)), scale=3)
    # The PS2 dumps: picture 1 (alpha 0..128), a picture the pack does not have, and picture 3 at another size.
    save(dumps / "1111-2222-00001994.png", picture(1), ps2_alpha=True)
    save(dumps / "3333-4444-00001994.png", picture(9))
    save(dumps / "5555-6666-00001994.png", picture(3, (32, 64)))

    counts = tool.import_pack(dumps, pack, out)

    assert counts["dumped"] == 3
    assert counts["imported"] == 1
    assert counts["no_size"] == 1
    assert counts["no_match"] + counts["ambiguous"] == 1
    with Image.open(out / "1111-2222-00001994.png") as imported:
        assert imported.size == (96, 96)  # the pack's image, at its own resolution
        assert np.asarray(imported)[..., 3].max() == 128  # alpha brought to the PS2's range
    assert counts["rejected_content"] == 0
    assert len(counts["report"][0].split("\t")) == 9
    assert not (out / "3333-4444-00001994.png").exists()
    assert not (out / "5555-6666-00001994.png").exists()


def import_one(tmp_path: Path, target: np.ndarray, candidate: np.ndarray) -> dict:
    dumps, pack, out = tmp_path / "dumps", tmp_path / "pack", tmp_path / "out"
    dumps.mkdir()
    pack.mkdir()
    save(dumps / "1111-2222-00001994.png", target, ps2_alpha=True)
    width, height = target.shape[1], target.shape[0]
    save(pack / f"tex1_{width}x{height}_aaaa_9.png", candidate, scale=3)
    return tool.import_pack(dumps, pack, out)


def test_sparse_wrong_shape_is_rejected_despite_identical_thumbnail(tmp_path: Path) -> None:
    target = np.zeros((64, 64, 4), dtype=np.uint8)
    candidate = target.copy()
    target[4, 4] = [255, 255, 255, 255]
    candidate[7, 7] = [255, 255, 255, 255]
    # Both white dots average into the same thumbnail cell. The old nearest-thumbnail check accepted this.
    assert np.array_equal(tool.thumbnail(Image.fromarray(target)), tool.thumbnail(Image.fromarray(candidate)))
    counts = import_one(tmp_path, target, candidate)
    assert counts["imported"] == 0
    assert counts["rejected_content"] == 1
    assert counts["no_match"] == counts["ambiguous"] == 0
    assert float(counts["rejected_report"][0].split("\t")[4]) == 0.0
    assert not list((tmp_path / "out").glob("*.png"))


def test_exact_flat_colour_uses_narrow_fallback(tmp_path: Path) -> None:
    flat = np.full((32, 32, 4), [100, 120, 140, 255], dtype=np.uint8)
    counts = import_one(tmp_path, flat, flat)
    assert counts["imported"] == 1
    assert counts["rejected_content"] == 0
    assert counts["report"][0].split("\t")[-1] == "1"
    with Image.open(tmp_path / "out" / "1111-2222-00001994.png") as image:
        assert image.size == (96, 96)
        assert np.asarray(image)[..., 3].max() == 128


def test_similar_flat_colour_is_not_mistaken_for_exact(tmp_path: Path) -> None:
    target = np.full((32, 32, 4), [100, 120, 140, 255], dtype=np.uint8)
    candidate = np.full((32, 32, 4), [104, 124, 144, 255], dtype=np.uint8)
    counts = import_one(tmp_path, target, candidate)
    assert counts["imported"] == 0
    assert counts["rejected_content"] == 1
    assert counts["no_match"] == counts["ambiguous"] == 0
    assert counts["rejected_report"][0].split("\t")[-1] == "0"


def test_one_pixel_and_thin_textures_have_finite_metrics(tmp_path: Path) -> None:
    for width, height in [(1, 1), (1, 8), (8, 1)]:
        case = tmp_path / f"{width}x{height}"
        case.mkdir()
        rgba = picture(7, (width, height))
        counts = import_one(case, rgba, rgba)
        assert counts["imported"] == 1
        assert counts["rejected_content"] == 0
        metrics = [float(v) for v in counts["report"][0].split("\t")[4:8]]
        assert np.isfinite(metrics).all()


def test_flat_fallback_does_not_ignore_alpha_mismatch(tmp_path: Path) -> None:
    target = np.full((8, 8, 4), [100, 120, 140, 255], dtype=np.uint8)
    candidate = target.copy()
    candidate[..., 3] = 240
    counts = import_one(tmp_path, target, candidate)
    assert counts["imported"] == 0
    # This difference is small enough for candidate selection, but not the flat fallback.
    assert counts["rejected_content"] == 1


def test_existing_png_output_is_refused_without_changing_it(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    sentinel = out / "existing.PNG"
    before = b"existing custom replacement must be preserved"
    sentinel.write_bytes(before)
    target = picture(1)
    with pytest.raises(ValueError, match="output folder already contains PNG"):
        import_one(tmp_path, target, target)
    assert sentinel.read_bytes() == before
    assert sorted(path.name for path in out.iterdir()) == [sentinel.name]


def test_existing_non_png_output_files_are_preserved(tmp_path: Path) -> None:
    out = tmp_path / "out"
    out.mkdir()
    notes = out / "README.txt"
    before = "A user's pack notes.\n"
    notes.write_text(before)
    target = picture(1)
    counts = import_one(tmp_path, target, target)
    assert counts["imported"] == 1
    assert notes.read_text() == before
