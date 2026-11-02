"""Tests of tools/funcmap/layout.py with synthetic rows and bytes (no game files)."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "funcmap"))
import layout  # noqa: E402

pytest.importorskip("rabbitizer")

BASE = 0x100000
NOP = 0
JR_RA = 0x03E00008
LUI_V0_8007 = 0x3C028007
LUI_V0_0012 = 0x3C020012
ADDIU_V0_V0_5 = 0x24420005
BAD = 0x00000055  # not an R5900 instruction (0xFFFFFFFF would decode as an MMI word)


def blob_of(words_by_addr, size=0x30000):
    """Zero-filled blob at BASE with the given {address: word}."""
    words = [0] * (size // 4)
    for a, w in words_by_addr.items():
        words[(a - BASE) // 4] = w
    return struct.pack(f"<{len(words)}I", *words)


def rows(*spec):
    return [(n, s, e) for n, s, e in spec]


def test_text_end_stops_at_big_gap_but_not_at_small_one():
    r = rows(("FUN_a", 0x100000, 0x100100), ("FUN_b", 0x100800, 0x100900), ("FUN_c", 0x120000, 0x120040))
    end, rest = layout.split_text(r)
    assert end == 0x100900  # the 0x700 hole is inside the text (below TEXT_GAP)
    assert rest == [("FUN_c", 0x120000, 0x120040)]


def test_text_end_uses_the_largest_end_of_nested_rows():
    r = rows(("FUN_a", 0x100000, 0x101000), ("entry_x", 0x100010, 0x100020))
    assert layout.split_text(r) == (0x101000, [])


def test_text_must_start_at_the_load_address():
    with pytest.raises(ValueError):
        layout.split_text(rows(("FUN_a", 0x200000, 0x200010)))


def test_islands_merge_below_the_gap():
    rest = rows(("FUN_a", 0x3000, 0x3100), ("entry_b", 0x3300, 0x3310), ("FUN_c", 0x4000, 0x4010))
    isl = layout.group_islands(rest)
    assert [(i["start"], i["end"], i["functions"], i["labels"]) for i in isl] == [
        (0x3000, 0x3310, 1, 1),
        (0x4000, 0x4010, 1, 0),
    ]


def test_island_gap_of_exactly_the_limit_splits():
    rest = rows(("FUN_a", 0x3000, 0x3100), ("FUN_b", 0x3100 + layout.MERGE_GAP, 0x3600))
    assert len(layout.group_islands(rest)) == 2


def test_oversized_label_does_not_swallow_the_following_islands():
    rest = rows(("entry_a", 0x3000, 0x3000 + 0x100000), ("entry_b", 0x20000, 0x20010))
    rest.sort(key=lambda r: r[1])
    isl = layout.group_islands(rest)
    assert [(i["start"], i["end"]) for i in isl] == [(0x3000, 0x3004), (0x20000, 0x20010)]


def test_labels_are_recognised_by_prefix():
    assert layout.is_label("entry_00123456") and layout.is_label("caseD_4") and layout.is_label("default")
    assert not layout.is_label("FUN_00123456") and not layout.is_label("entry")


def test_classify():
    assert layout.classify(True, {0x8007}, False) == "kernel_patch_blob"
    assert layout.classify(True, {0x8000}, False) == "kernel_patch_blob"
    assert layout.classify(True, {0x0012, 0x8008}, False) == "code"
    assert layout.classify(True, set(), False) == "code"
    assert layout.classify(False, {0x8007}, False) == "false_positive"
    assert layout.classify(True, {0x8007}, True) == "false_positive"


def analyse(blob, text_rows, island_rows, vu=()):
    return layout.analyse(sorted(text_rows + island_rows, key=lambda r: (r[1], r[2])), blob, BASE, list(vu))


def test_island_with_kernel_lui_is_a_kernel_patch_blob():
    blob = blob_of({0x120000: LUI_V0_8007, 0x120004: ADDIU_V0_V0_5, 0x120008: JR_RA})
    end, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)), rows(("FUN_k", 0x120000, 0x120010)))
    assert end == 0x100010
    assert len(isl) == 1 and isl[0]["kind"] == "kernel_patch_blob"


def test_island_with_ordinary_code_is_code():
    blob = blob_of({0x120000: LUI_V0_0012, 0x120004: ADDIU_V0_V0_5, 0x120008: JR_RA})
    _, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)), rows(("FUN_k", 0x120000, 0x120010)))
    assert isl[0]["kind"] == "code"


def test_island_with_undecodable_words_is_false_positive():
    blob = blob_of({0x120000: LUI_V0_8007, 0x120004: BAD, 0x120008: JR_RA})
    _, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)), rows(("FUN_k", 0x120000, 0x120010)))
    assert isl[0]["kind"] == "false_positive"


def test_island_inside_a_vu_microprogram_is_false_positive():
    blob = blob_of({0x120000: LUI_V0_8007, 0x120004: JR_RA})
    _, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)), rows(("FUN_k", 0x120000, 0x120010)),
                     vu=[(0x11FF00, 0x120100)])
    assert isl[0]["kind"] == "false_positive"


def test_island_with_functions_is_judged_on_its_function_rows_only():
    # a label-only hole with garbage between two good functions does not spoil the island
    blob = blob_of({0x120000: LUI_V0_8007, 0x120008: JR_RA, 0x120080: BAD, 0x120100: JR_RA})
    _, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)),
                     rows(("FUN_a", 0x120000, 0x120010), ("entry_g", 0x120040, 0x120044), ("FUN_b", 0x120100, 0x120110)))
    assert isl[0]["kind"] == "kernel_patch_blob"
    assert isl[0]["functions"] == 2 and isl[0]["labels"] == 1
    assert "do not decode" in isl[0]["note"]


def test_island_without_functions_is_judged_on_all_its_words():
    blob = blob_of({0x120000: BAD})
    _, isl = analyse(blob, rows(("FUN_t", 0x100000, 0x100010)), rows(("entry_a", 0x120000, 0x120010)))
    assert isl[0]["kind"] == "false_positive"


def test_find_address_refs_follows_lui_addiu_pairs():
    blob = blob_of({
        0x100000: 0x3C02003C,  # lui v0, 0x3C
        0x100004: 0x24420428,  # addiu v0, v0, 0x428  -> 0x3C0428
        0x100008: 0x3C03003C,  # lui v1, 0x3C
        0x10000C: 0x2463FFF0,  # addiu v1, v1, -0x10  -> 0x3BFFF0 (outside the range)
    })
    assert layout.find_address_refs(blob, BASE, 0x100100, 0x3C0400, 0x3C0500) == [(0x100000, 0x3C0428)]


def test_find_address_refs_ignores_a_pair_on_a_different_register():
    blob = blob_of({0x100000: 0x3C02003C, 0x100004: 0x24630428})  # lui v0 ... addiu v1, v1
    assert layout.find_address_refs(blob, BASE, 0x100100, 0x3C0400, 0x3C0500) == []


def test_find_data_refs_skips_unaligned_values_and_the_range_itself():
    blob = blob_of({0x102000: 0x103010, 0x102004: 0x103011, 0x103020: 0x103010}, size=0x4000)
    assert layout.find_data_refs(blob, BASE, 0x101000, 0x103000, 0x103100) == [(0x102000, 0x103010)]


def test_function_at_prefers_the_smallest_enclosing_function_and_ignores_labels():
    r = rows(("FUN_big", 0x1000, 0x9000), ("entry_x", 0x2000, 0x2100), ("FUN_small", 0x2000, 0x2200))
    assert layout.function_at(r, 0x2010) == "FUN_small"
    assert layout.function_at(r, 0x8000) == "FUN_big"
    assert layout.function_at(r, 0x9000) == "-"


def test_render_toml_round_trips():
    import tomllib
    isl = [{"start": 0x3BC428, "end": 0x3BD8C8, "kind": "kernel_patch_blob", "functions": 9, "labels": 55,
            "note": "n"}]
    d = tomllib.loads(layout.render_toml("ab" * 32, 0x100000, 0x4C9D80, 0x5A6800, 0x100008, 0x3902D4, isl))
    assert d["text"] == {"start": 0x100000, "end": 0x3902D4}
    assert d["elf"]["bss_end"] == 0x5A6800
    assert d["island"][0]["kind"] == "kernel_patch_blob" and d["island"][0]["end"] == 0x3BD8C8
