"""tools/textures/import_dolphin_pack.py on synthetic images (no game file): a PS2 dump is matched to the pack image of
the same picture and original size, its alpha is brought to the PS2's 0..128 range, and a picture with no counterpart,
an ambiguous one and one of another size are left out."""
import sys
from pathlib import Path

import numpy as np
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
    imported = Image.open(out / "1111-2222-00001994.png")
    assert imported.size == (96, 96)  # the pack's image, at its own resolution
    assert np.asarray(imported)[..., 3].max() == 128  # alpha brought to the PS2's range
    assert not (out / "3333-4444-00001994.png").exists()
    assert not (out / "5555-6666-00001994.png").exists()
