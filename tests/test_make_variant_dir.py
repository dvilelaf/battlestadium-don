import hashlib
import sys
from pathlib import Path

import pytest

# python -I does not add the script folder to sys.path; this test imports tools/disc/make_variant_dir.py.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "disc"))

import make_variant_dir as mvd  # noqa: E402

# Synthetic stand-ins: no game file is needed. The "executable" and the three "DATA.AFS" are short byte strings
# whose hashes the synthetic config lists.
ELF = b"fake executable"
DATA = {"jp": b"data jp", "en-jpvoice": b"data en jpvoice", "en-envoice": b"data en envoice"}
CONFIG = {
    "elf": {"sha256": hashlib.sha256(ELF).hexdigest()},
    "variant": [
        {"id": key, "description": f"variant {key}", "data_afs_sha1": hashlib.sha1(value).hexdigest()}
        for key, value in DATA.items()
    ],
}


def write_jp(folder, data=DATA["jp"]):
    folder.mkdir()
    (folder / "SLPS_256.75").write_bytes(ELF)
    (folder / "SYSTEM.CNF").write_text("BOOT2 = cdrom0:\\SLPS_256.75;1")
    (folder / "DIR.DIR").write_text("data.afs")
    (folder / "MOVIE.AFS").write_bytes(b"movie")
    (folder / "DATA.AFS").write_bytes(data)
    (folder / "BGM.AFS").write_bytes(b"bgm jp")
    (folder / "SE.AFS").write_bytes(b"se jp")
    (folder / "MODULES").mkdir()
    (folder / "MODULES" / "0000.FPK").write_bytes(b"module")
    return folder


def write_english(folder, voices="en-jpvoice", with_bgm=False):
    folder.mkdir()
    (folder / "DATA.AFS").write_bytes(DATA[voices])
    if with_bgm:
        (folder / "BGM.AFS").write_bytes(b"bgm en")
    return folder


@pytest.fixture
def jp(tmp_path):
    return write_jp(tmp_path / "jp")


def test_japanese_folder_is_recognised_and_linked(tmp_path, jp):
    dest = tmp_path / "out"
    variant_id, made = mvd.make(jp, jp, dest, config=CONFIG)
    assert variant_id == "jp"
    assert set(made) == set(mvd.JP_FILES) | set(mvd.DATA_FILES)
    assert (dest / "DATA.AFS").is_symlink() and (dest / "DATA.AFS").read_bytes() == DATA["jp"]
    assert (dest / "MODULES" / "0000.FPK").read_bytes() == b"module"
    assert (dest / mvd.MARKER).is_file()


def test_english_data_files_come_from_the_variant_and_the_rest_from_japanese(tmp_path, jp):
    en = write_english(tmp_path / "en", with_bgm=True)
    dest = tmp_path / "out"
    variant_id, _ = mvd.make(jp, en, dest, config=CONFIG)
    assert variant_id == "en-jpvoice"
    assert (dest / "DATA.AFS").resolve() == (en / "DATA.AFS").resolve()
    assert (dest / "BGM.AFS").resolve() == (en / "BGM.AFS").resolve()
    assert (dest / "SE.AFS").resolve() == (jp / "SE.AFS").resolve()  # the variant lacks it
    assert (dest / "SLPS_256.75").resolve() == (jp / "SLPS_256.75").resolve()


def test_english_voices_are_told_apart(tmp_path, jp):
    en = write_english(tmp_path / "en", voices="en-envoice")
    assert mvd.make(jp, en, tmp_path / "out", config=CONFIG)[0] == "en-envoice"


def test_copy_makes_regular_files(tmp_path, jp):
    dest = tmp_path / "out"
    _, made = mvd.make(jp, write_english(tmp_path / "en"), dest, copy=True, config=CONFIG)
    assert set(made.values()) == {"copy"}
    assert not any(path.is_symlink() for path in dest.iterdir())
    assert (dest / "DATA.AFS").read_bytes() == DATA["en-jpvoice"]
    assert (dest / "MODULES" / "0000.FPK").read_bytes() == b"module"


def test_copy_is_used_when_links_are_not_allowed(tmp_path, jp, monkeypatch):
    def refuse(self, *args, **kwargs):
        raise OSError("no symlinks here")

    monkeypatch.setattr(Path, "symlink_to", refuse)
    _, made = mvd.make(jp, jp, tmp_path / "out", config=CONFIG)
    assert set(made.values()) == {"copy"}
    assert (tmp_path / "out" / "DATA.AFS").read_bytes() == DATA["jp"]


def test_refuses_a_folder_it_did_not_make(tmp_path, jp):
    dest = tmp_path / "out"
    dest.mkdir()
    (dest / "mine.txt").write_text("keep me")
    with pytest.raises(mvd.MakeError, match="not empty"):
        mvd.make(jp, jp, dest, config=CONFIG)
    assert [path.name for path in dest.iterdir()] == ["mine.txt"]


def test_refuses_a_file_as_destination(tmp_path, jp):
    dest = tmp_path / "out"
    dest.write_text("a file")
    with pytest.raises(mvd.MakeError, match="not a folder"):
        mvd.make(jp, jp, dest, config=CONFIG)


def test_an_empty_destination_is_accepted(tmp_path, jp):
    dest = tmp_path / "out"
    dest.mkdir()
    assert mvd.make(jp, jp, dest, config=CONFIG)[0] == "jp"


def test_rebuilding_own_folder_changes_variant_and_keeps_the_ports_files(tmp_path, jp):
    dest = tmp_path / "out"
    mvd.make(jp, jp, dest, config=CONFIG)
    (dest / "mc0").mkdir()
    (dest / "mc0" / "save").write_text("card")
    variant_id, _ = mvd.make(jp, write_english(tmp_path / "en"), dest, config=CONFIG)
    assert variant_id == "en-jpvoice"
    assert (dest / "DATA.AFS").read_bytes() == DATA["en-jpvoice"]
    assert (dest / "mc0" / "save").read_text() == "card"


def test_unknown_data_afs_makes_nothing(tmp_path, jp):
    other = tmp_path / "other"
    other.mkdir()
    (other / "DATA.AFS").write_bytes(b"not a known variant")
    dest = tmp_path / "out"
    assert mvd.make(jp, other, dest, config=CONFIG) == (None, {})
    assert not dest.exists()


def test_a_wrong_executable_is_refused(tmp_path, jp):
    (jp / "SLPS_256.75").write_bytes(b"something else")
    with pytest.raises(mvd.MakeError, match="executable"):
        mvd.make(jp, jp, tmp_path / "out", config=CONFIG)
    assert not (tmp_path / "out").exists()


def test_missing_japanese_files_are_listed(tmp_path, jp):
    (jp / "MOVIE.AFS").unlink()
    (jp / "DIR.DIR").unlink()
    with pytest.raises(mvd.MakeError, match="DIR.DIR.*MOVIE.AFS|MOVIE.AFS.*DIR.DIR"):
        mvd.make(jp, jp, tmp_path / "out", config=CONFIG)


def test_a_missing_variant_is_an_error(tmp_path, jp):
    with pytest.raises(mvd.MakeError, match="does not exist"):
        mvd.make(jp, tmp_path / "nothing.iso", tmp_path / "out", config=CONFIG)


def test_an_image_is_extracted_into_the_destination(tmp_path, jp, monkeypatch):
    calls = []

    def fake_extract(image, folder):
        calls.append(Path(image).name)
        (Path(folder) / "DATA.AFS").write_bytes(DATA["en-envoice"])
        (Path(folder) / "SE.AFS").write_bytes(b"se en")

    monkeypatch.setattr(mvd, "extract_image", fake_extract)
    image = tmp_path / "disc.iso"
    image.write_bytes(b"image")
    dest = tmp_path / "out"
    variant_id, _ = mvd.make(jp, image, dest, config=CONFIG)
    assert calls == ["disc.iso"]
    assert variant_id == "en-envoice"
    assert (dest / "SE.AFS").read_bytes() == b"se en"
    assert (dest / "BGM.AFS").resolve() == (jp / "BGM.AFS").resolve()
    # a rebuild removes the extracted files with the rest of what the tool made
    mvd.make(jp, jp, dest, config=CONFIG)
    assert not (dest / mvd.EXTRACTED).exists()


def test_an_image_of_an_unknown_variant_leaves_nothing(tmp_path, jp, monkeypatch):
    monkeypatch.setattr(mvd, "extract_image", lambda image, folder: (Path(folder) / "DATA.AFS").write_bytes(b"x"))
    image = tmp_path / "disc.iso"
    image.write_bytes(b"image")
    dest = tmp_path / "out"
    assert mvd.make(jp, image, dest, config=CONFIG) == (None, {})
    assert not (dest / mvd.EXTRACTED).exists()


def test_main_exit_codes(tmp_path, jp, capsys):
    config = tmp_path / "discs.toml"
    config.write_text(
        f'[elf]\nsha256 = "{CONFIG["elf"]["sha256"]}"\n'
        + "".join(
            f'[[variant]]\nid = "{v["id"]}"\ndescription = "{v["description"]}"\ndata_afs_sha1 = "{v["data_afs_sha1"]}"\n'
            for v in CONFIG["variant"]
        )
    )
    dest = tmp_path / "out"
    assert mvd.main([str(jp), str(jp), str(dest), "--config", str(config)]) == mvd.EXIT_OK
    assert "variant: jp" in capsys.readouterr().out
    assert mvd.main([str(jp), str(tmp_path / "missing"), str(tmp_path / "o2"), "--config", str(config)]) == mvd.EXIT_ERROR
    unknown = tmp_path / "unknown"
    unknown.mkdir()
    (unknown / "DATA.AFS").write_bytes(b"?")
    assert mvd.main([str(jp), str(unknown), str(tmp_path / "o3"), "--config", str(config)]) == mvd.EXIT_UNKNOWN
