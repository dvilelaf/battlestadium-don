import sys
from pathlib import Path

# python -I does not add the script folder to sys.path; this test imports tools/disc/verify_disc.py.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "disc"))

from verify_disc import CONFIG_PATH, EXIT_ERROR, EXIT_MISMATCH, EXIT_OK, classify, load_config, main  # noqa: E402

CONFIG = load_config(CONFIG_PATH)
ELF_SHA256 = CONFIG["elf"]["sha256"]
VARIANT_SHA1 = {variant["id"]: variant["data_afs_sha1"] for variant in CONFIG["variant"]}
WRONG_ELF_SHA256 = "0" * 64
UNKNOWN_DATA_SHA1 = "0" * 40


def test_config_has_the_three_variants():
    assert list(VARIANT_SHA1) == ["jp", "en-jpvoice", "en-envoice"]


def test_each_variant_is_recognised():
    for variant_id, data_sha1 in VARIANT_SHA1.items():
        assert classify(ELF_SHA256, data_sha1, CONFIG) == (EXIT_OK, variant_id)


def test_wrong_elf_is_rejected():
    assert classify(WRONG_ELF_SHA256, VARIANT_SHA1["jp"], CONFIG) == (EXIT_MISMATCH, None)


def test_unknown_data_afs_is_rejected():
    assert classify(ELF_SHA256, UNKNOWN_DATA_SHA1, CONFIG) == (EXIT_MISMATCH, "unknown")


def test_hashes_are_compared_case_insensitively():
    assert classify(ELF_SHA256.upper(), VARIANT_SHA1["en-envoice"].upper(), CONFIG) == (
        EXIT_OK,
        "en-envoice",
    )


def test_missing_iso_exits_with_error(tmp_path):
    assert main([str(tmp_path / "absent.iso")]) == EXIT_ERROR
