"""tools/verify/dump_regs.py on a tiny GS dump written here (the real dumps are game data)."""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))
import dump_regs  # noqa: E402

PMODE = 0x8067
SMODE1 = 0x740814504
SMODE2 = 0x1
DISPFB = (140 << 0) | (10 << 9)  # FBP 140, FBW 10, PSM 0
DISPLAY = 636 | (50 << 12) | (3 << 23) | (2559 << 32) | (447 << 44)


def block(values):
    data = bytearray(dump_regs.PRIV_BYTES)
    offsets = dict(dump_regs.REGISTERS)
    for name, value in values.items():
        struct.pack_into("<Q", data, offsets[name], value)
    return bytes(data)


def make_dump(vsyncs=2, serial=b"SLPS-00000", state=b"\x11" * 16, extra_packets=b""):
    screenshot = b"\xAA" * (2 * 2 * 4)
    header = struct.pack("<9I", 9, len(state), 36, len(serial), 0x1234, 2, 2, 36 + len(serial), len(screenshot))
    out = bytearray(struct.pack("<II", 0xFFFFFFFF, len(header) + len(serial) + len(screenshot)))
    out += header + serial + screenshot + state
    out += block({"PMODE": PMODE, "SMODE1": SMODE1, "SMODE2": SMODE2})
    out += bytes([0, 2]) + struct.pack("<I", 5) + b"hello"  # a transfer that must be skipped
    out += extra_packets
    for i in range(vsyncs):
        regs = {"PMODE": PMODE, "SMODE1": SMODE1, "SMODE2": SMODE2, "DISPFB1": DISPFB + i, "DISPLAY1": DISPLAY,
                "CSR": 0x551B400C + i, "SIGLBLID": 7}
        out += bytes([3]) + block(regs) + bytes([1, i & 1])
    return bytes(out)


def test_parse_registers_at_each_vsync():
    serial, moments = dump_regs.parse_registers(make_dump(vsyncs=3))
    assert serial == "SLPS-00000"
    assert [m[0] for m in moments] == ["initial", "vsync 1", "vsync 2", "vsync 3"]
    assert [m[1] for m in moments] == [None, 0, 1, 0]
    initial = moments[0][2]
    assert initial["PMODE"] == PMODE and initial["SMODE1"] == SMODE1 and initial["DISPFB1"] == 0
    last = moments[3][2]
    assert last["DISPFB1"] == DISPFB + 2 and last["DISPLAY1"] == DISPLAY
    assert last["CSR"] == 0x551B400C + 2 and last["SIGLBLID"] == 7
    assert len(last) == 19


def test_decode_names_the_fields():
    assert dump_regs.decode("PMODE", PMODE) == "EN1=1 EN2=1 CRTMD=1 MMOD=1 AMOD=1 SLBG=0 ALP=128"
    assert "CMOD=2" in dump_regs.decode("SMODE1", SMODE1) and "PRST=1" in dump_regs.decode("SMODE1", SMODE1)
    assert dump_regs.decode("SMODE2", 3) == "INT=1 FFMD=1 DPMS=0"
    assert dump_regs.decode("DISPFB1", DISPFB) == "FBP=140 FBW=10 PSM=0 DBX=0 DBY=0"
    assert dump_regs.decode("DISPLAY2", DISPLAY) == "DX=636 DY=50 MAGH=3 MAGV=0 DW=2559 DH=447"
    assert dump_regs.decode("CSR", 5) == ""


def test_command_line(tmp_path, capsys):
    path = tmp_path / "t.gs"
    path.write_bytes(make_dump(vsyncs=1))
    assert dump_regs.main([str(path), "--decode"]) == 0
    text = capsys.readouterr().out
    assert "moments=2" in text and "== vsync 1 (field 0)" in text
    assert "DISPLAY1  0x" in text and "DW=2559" in text and "FBP=140" in text
    assert dump_regs.main([str(path)]) == 0
    assert "DW=2559" not in capsys.readouterr().out


def test_zstd_file(tmp_path, capsys):
    zstd = shutil.which("zstd")
    if not zstd:
        pytest.skip("no zstd tool")
    raw = tmp_path / "t.gs"
    raw.write_bytes(make_dump(vsyncs=2))
    subprocess.run([zstd, "-q", str(raw), "-o", str(tmp_path / "t.gs.zst")], check=True)
    assert dump_regs.main([str(tmp_path / "t.gs.zst")]) == 0
    assert "moments=3" in capsys.readouterr().out


def test_errors(tmp_path, capsys):
    bad = tmp_path / "bad.gs"
    bad.write_bytes(b"\x00" * 64)
    assert dump_regs.main([str(bad)]) == 1
    assert "not a GS dump" in capsys.readouterr().err
    cut = tmp_path / "cut.gs"
    cut.write_bytes(make_dump()[:100])
    assert dump_regs.main([str(cut)]) == 1
    assert dump_regs.main([str(tmp_path / "missing.gs")]) == 1
