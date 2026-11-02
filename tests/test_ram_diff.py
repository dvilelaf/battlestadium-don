"""Tests of tools/verify/ram_diff.py (T-016 iteration 8).

Two synthetic images are compared: a differing word that holds a code address on one side only, a
differing word with values outside the code (kept with --kind all, dropped with --kind code) and an
identical word. The function map names the code address.
"""
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "verify" / "ram_diff.py"
CSV = ROOT / "config" / "functions.ghidra.csv"

SIZE = 0x20000
CODE_ADDRESS = 0x00123458          # inside a row of the function map (FUN_001233e8)
OTHER_CODE_ADDRESS = 0x00200000    # a different row of the function map (inside the code range)
DATA_ADDRESS = 0x004C9E80          # static data, not code


def write_image(path, values):
    data = bytearray(SIZE)
    for address, value in values.items():
        struct.pack_into("<I", data, address, value)
    path.write_bytes(bytes(data))


def run(ours, reference, *args):
    result = subprocess.run([sys.executable, "-I", str(TOOL), str(ours), str(reference), *args],
                            capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    return result.stdout


def test_ram_diff(tmp_path):
    ours = tmp_path / "ours.bin"
    reference = tmp_path / "ref.bin"
    write_image(ours, {0x100: CODE_ADDRESS, 0x200: 0x11, 0x300: 0xAB})
    write_image(reference, {0x100: OTHER_CODE_ADDRESS, 0x200: 0x22, 0x300: 0xAB})

    text = run(ours, reference, "--range", "0x0:0x%x" % SIZE, "--csv", str(CSV))
    # Only 0x100 holds a code address on one side; 0x200 is 0x11 against 0x22 (both data) and 0x300 is
    # identical, so kind code prints one word.
    assert "1 differing word(s) of 32768" in text, text
    assert "0x00000100" in text and "FUN_001233e8" in text, text
    assert "0x00000200" not in text, text
    assert "0x00000300" not in text, text  # identical
    assert "0x00000100:0x00000104" in text, text  # the group summary

    text = run(ours, reference, "--range", "0x0:0x%x" % SIZE, "--kind", "all")
    assert "2 differing word(s) of 32768" in text, text
    assert "0x00000100" in text and "0x00000200" in text, text

    # A word that differs only in data (both sides outside the code) is dropped by kind code.
    write_image(reference, {0x400: DATA_ADDRESS})
    text = run(ours, reference, "--range", "0x0:0x%x" % SIZE, "--kind", "code")
    assert "0x00000400" not in text, text
    text = run(ours, reference, "--range", "0x0:0x%x" % SIZE, "--kind", "all")
    assert "0x00000400" in text, text
