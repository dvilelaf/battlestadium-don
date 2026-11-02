"""Tests of tools/pipeline/make_config.py with synthetic files (no game files, no real config)."""
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "pipeline"))
import make_config as mc  # noqa: E402

TEXT = (0x100000, 0x200000)
ISLANDS = [
    (0x300000, 0x300100, "kernel_patch_blob"),
    (0x310000, 0x310100, "false_positive"),
    (0x320000, 0x320100, "code"),
]


def test_filter_function_map_islands():
    rows = [
        ("a", 0x100000, 0x100010, "16"),
        ("b", 0x1FFFF0, 0x200000, "16"),
        ("c", 0x1FFFF0, 0x200010, "32"),      # crosses the end of the text
        ("blob", 0x300000, 0x300010, "16"),
        ("blob2", 0x300020, 0x300030, "16"),
        ("fp", 0x310000, 0x310010, "16"),
        ("code", 0x320000, 0x320010, "16"),   # island of kind code: kept
        ("label", 0x310010, 0x4BB9A0, "x"),   # starts in an island, absurd end
        ("far", 0x500000, 0x500010, "16"),
    ]
    kept, dropped = mc.filter_function_map(rows, TEXT, ISLANDS)
    assert [r[0] for r in kept] == ["a", "b", "code"]
    assert dropped["island 0x00300000-0x00300100 (kernel_patch_blob)"] == 2
    assert dropped["island 0x00310000-0x00310100 (false_positive)"] == 2
    assert dropped["outside text and islands"] == 2


def test_filter_stubs_by_policy():
    stubs = ["x@0x00100000", "y@0x00100010", "z@0x00100020", "w@0x00100030", "u@0x00100040"]
    policy = {"x": "stub", "y": "recompile", "z": "review", "w": "stub"}
    kept, dropped, counts = mc.filter_stubs(stubs, "recompile", policy)
    assert kept == ["x@0x00100000", "w@0x00100030"]
    assert dropped == ["y@0x00100010", "z@0x00100020", "u@0x00100040"]
    assert counts == {"stub": 2, "recompile": 3}


def test_stubs_outside_text_dropped():
    inside, outside = mc.filter_by_text(["a@0x00100000", "b@0x00300000", "c@0x00200000"], TEXT)
    assert inside == ["a@0x00100000"]
    assert outside == ["b@0x00300000", "c@0x00200000"]


def test_stub_name_clash():
    rows = [("AddIntcHandler", 0x100500, 0x100510, "16"), ("other", 0x100600, 0x100610, "16")]
    clashes = mc.stubs_matching_other_rows(
        ["AddIntcHandler@0x00100100", "AddIntcHandler@0x00100500", "other@0x00100600"], rows)
    assert clashes == [("AddIntcHandler@0x00100100", [0x100500])]


def test_merge_entry_points_removes_duplicates():
    merged, stats = mc.merge_entry_points(
        ["f@0x00100020", "f@0x00100020", "g@0x00100010"], [0x100010, 0x100030])
    assert merged == ["g@0x00100010", "f@0x00100020", "entry_00100030@0x00100030"]
    assert stats == {"analyzer": 3, "analyzer_unique": 2, "extra": 2, "manual": 0, "merged": 3}


def test_read_extra_entry_points(tmp_path):
    p = tmp_path / "extra.txt"
    p.write_text("# comment\n0x00100000  # new_code\n\n0x00100010\n")
    assert mc.read_extra_entry_points(p) == [0x100000, 0x100010]


def test_filter_jump_tables():
    ok = {"address": "0x4bc430", "entries": [{"index": 0, "target": "0x100000"},
                                            {"index": 1, "target": "0x1fffff"}]}
    bad = {"address": "0x3c0000", "entries": [{"index": 0, "target": "0x100000"},
                                             {"index": 1, "target": "0x3bfff8"}]}
    empty = {"address": "0x1", "entries": []}
    kept, dropped = mc.filter_jump_tables([ok, bad, empty], TEXT)
    assert kept == [ok]
    assert dropped == [bad, empty]


def test_filter_mmio_sorted():
    kept, dropped = mc.filter_mmio(
        {"0x100010": "0x1000f000", "0x300000": "0x10002020", "0x100000": "0x1000b410"}, TEXT)
    assert list(kept) == ["0x100000", "0x100010"]
    assert dropped == {"0x300000": "0x10002020"}


def test_write_toml_roundtrip():
    general = {
        "input": "/r/disc/SLPS_256.75", "ghidra_output": "/r/work/f.csv", "output": "/r/generated/",
        "single_file_output": False, "patch_syscalls": False, "patch_cop0": True,
        "patch_cache": True, "stubs": ["a@0x00100000"], "entry_points": ["b@0x00100010"],
        "skip": [],
    }
    tables = [{"address": "0x4bc430", "base_register": 2,
               "entries": [{"index": 0, "target": "0x100000"}]}]
    text = mc.write_toml(general, [(0x11A31C, "0x0")], {"0x100000": "0x1000f000"}, tables)
    d = tomllib.loads(text)
    assert d["general"]["stubs"] == ["a@0x00100000"]
    assert d["general"]["patch_cop0"] is True and d["general"]["single_file_output"] is False
    assert d["general"]["skip"] == []
    assert d["patches"]["instructions"] == [{"address": "0x11a31c", "value": "0x0"}]
    assert d["mmio"] == {"0x100000": "0x1000f000"}
    assert d["jump_tables"]["table"] == tables
    assert mc.write_toml(general, [(0x11A31C, "0x0")], {"0x100000": "0x1000f000"}, tables) == text


def test_main_end_to_end(tmp_path):
    (tmp_path / "f.csv").write_text(
        "Name,Start,End,Size\nmain,0x00100008,0x00100018,16\nblob,0x00300000,0x00300010,16\n")
    (tmp_path / "layout.toml").write_text(
        "[text]\nstart = 0x00100000\nend = 0x00200000\n"
        '[[island]]\nstart = 0x00300000\nend = 0x00300100\nkind = "kernel_patch_blob"\n')
    (tmp_path / "policy.toml").write_text('default = "recompile"\n[policy]\nsceA = "stub"\nsceB = "review"\n')
    (tmp_path / "extra.txt").write_text("0x00100008  # x\n0x00100100\n")
    (tmp_path / "analyzer.toml").write_text(
        '[general]\nstubs = ["sceA@0x00100008", "sceB@0x00100100", "sceA@0x00300000"]\n'
        'entry_points = ["main@0x00100008", "far@0x00400000"]\n'
        '[mmio]\n"0x100000" = "0x1000f000"\n"0x300000" = "0x1000f000"\n'
        '[jump_tables]\n[[jump_tables.table]]\naddress = "0x4bc430"\nentries = [\n'
        '  { index = 0, target = "0x100000" },\n]\n'
        '[[jump_tables.table]]\naddress = "0x3c0000"\nentries = [\n'
        '  { index = 0, target = "0x3bfff8" },\n]\n')
    out = tmp_path / "out" / "don.toml"
    rc = mc.main([
        "--csv", str(tmp_path / "f.csv"), "--layout", str(tmp_path / "layout.toml"),
        "--policy", str(tmp_path / "policy.toml"), "--extra", str(tmp_path / "extra.txt"),
        "--patches", str(tmp_path / "missing.toml"),
        "--analyzer-config", str(tmp_path / "analyzer.toml"), "--elf", str(tmp_path / "elf"),
        "--out", str(out), "--filtered-csv", str(tmp_path / "out" / "f.csv"),
        "--generated-dir", str(tmp_path / "gen"),
    ])
    assert rc == 0
    d = tomllib.loads(out.read_text())
    assert d["general"]["stubs"] == ["sceA@0x00100008"]
    assert d["general"]["entry_points"] == ["main@0x00100008", "entry_00100100@0x00100100"]
    assert d["general"]["ghidra_output"] == str((tmp_path / "out" / "f.csv").resolve())
    assert d["general"]["output"].endswith("gen/")
    assert d["patches"]["instructions"] == []
    assert list(d["mmio"]) == ["0x100000"]
    assert len(d["jump_tables"]["table"]) == 1
    assert (tmp_path / "out" / "f.csv").read_text() == (
        "Name,Start,End,Size\nmain,0x00100008,0x00100018,16\n")
    assert not (tmp_path / "gen").exists()
    first = out.read_text()
    mc.main(["--csv", str(tmp_path / "f.csv"), "--layout", str(tmp_path / "layout.toml"),
             "--policy", str(tmp_path / "policy.toml"), "--extra", str(tmp_path / "extra.txt"),
             "--patches", str(tmp_path / "missing.toml"),
             "--analyzer-config", str(tmp_path / "analyzer.toml"), "--elf", str(tmp_path / "elf"),
             "--out", str(out), "--filtered-csv", str(tmp_path / "out" / "f.csv"),
             "--generated-dir", str(tmp_path / "gen")])
    assert out.read_text() == first


ROWS = [
    ("outer", 0x100000, 0x100100, "256"),
    ("inner", 0x100040, 0x100080, "64"),
    ("entry_00100090", 0x100090, 0x1000a0, "16"),
    ("caseD_5", 0x1000b0, 0x1000c0, "16"),
]


def test_classify_address():
    assert mc.classify_address(0x100040, ROWS) == ("start", "inner")
    assert mc.classify_address(0x100090, ROWS) == ("start", "entry_00100090")
    assert mc.classify_address(0x100044, ROWS) == ("inside", "inner")   # innermost
    assert mc.classify_address(0x100010, ROWS) == ("inside", "outer")
    assert mc.classify_address(0x100094, ROWS) == ("inside", "outer")   # label is not a function
    assert mc.classify_address(0x180000, ROWS) == ("none", None)


def test_merge_manual_entry_points():
    merged, stats = mc.merge_entry_points(
        ["f@0x00100020"], [0x100030], [0x100020, 0x100030, 0x100040])
    assert merged == ["f@0x00100020", "entry_00100030@0x00100030", "entry_00100040@0x00100040"]
    assert stats["manual"] == 3 and stats["extra"] == 1 and stats["merged"] == 3


def _run(tmp_path, manual, capsys):
    (tmp_path / "f.csv").write_text(
        "Name,Start,End,Size\nouter,0x00100008,0x00100108,256\nlab,0x00100200,0x00100210,16\n")
    (tmp_path / "layout.toml").write_text("[text]\nstart = 0x00100000\nend = 0x00200000\n")
    (tmp_path / "policy.toml").write_text('default = "recompile"\n')
    (tmp_path / "extra.txt").write_text("")
    (tmp_path / "analyzer.toml").write_text('[general]\nstubs = []\nentry_points = ["a@0x00100050"]\n')
    out = tmp_path / "don.toml"
    rc = mc.main([
        "--csv", str(tmp_path / "f.csv"), "--layout", str(tmp_path / "layout.toml"),
        "--policy", str(tmp_path / "policy.toml"), "--extra", str(tmp_path / "extra.txt"),
        "--manual-extra", str(manual), "--patches", str(tmp_path / "none.toml"),
        "--analyzer-config", str(tmp_path / "analyzer.toml"), "--elf", str(tmp_path / "elf"),
        "--out", str(out), "--filtered-csv", str(tmp_path / "ff.csv"),
        "--generated-dir", str(tmp_path / "gen")])
    assert rc == 0
    return tomllib.loads(out.read_text())["general"]["entry_points"], capsys.readouterr().out


def test_manual_missing_file_is_not_an_error(tmp_path, capsys):
    eps, out = _run(tmp_path, tmp_path / "missing.txt", capsys)
    assert eps == ["a@0x00100050"]
    assert "manual file 0 read" in out and "WARNING" not in out


def test_manual_classification_and_outside(tmp_path, capsys):
    manual = tmp_path / "manual.txt"
    manual.write_text("# header\n0x00100200  # start of a row\n0x00100050  # duplicate of analyzer"
                      "\n0x00100020 # inside outer\n0x00180000  # no row\n0x00500000  # outside text\n")
    eps, out = _run(tmp_path, manual, capsys)
    assert eps == ["entry_00100020@0x00100020", "a@0x00100050", "entry_00100200@0x00100200",
                   "entry_00180000@0x00180000"]
    assert "manual file 5 read (1 outside text dropped)" in out
    assert "outside text (manual): 0x00500000" in out
    assert "manual 0x00100200: start of row lab" in out
    assert "manual 0x00100020: inside function outer" in out
    assert "manual 0x00100050: inside function outer" in out
    assert "WARNING: manual entry point 0x00180000" in out
    assert out.count("WARNING:") == 1
