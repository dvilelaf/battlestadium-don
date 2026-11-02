"""T-202: the recompiler must test the whole 64-bit register in bltz/bgez/blez/bgtz.

The emitter used to test only the low 32 bits (GPR_S32), so after a dsubu whose 64-bit result is positive
but has bit 31 set, bltz was taken. The game's soft-float double subtract (FUN_00130598) does exactly that,
which turned the camera look-at into its vertical branch (the 3D view rolled by 90 degrees).

The test builds a tiny MIPS ELF (no game data), runs the recompiler on it and reads the emitted conditions.
The recompiler is DON_RECOMP, or third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp; the test is
skipped when neither exists.
"""
import os
import re
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BASE = 0x100000
# 0x00 bltz a0 / 0x08 bgez a0 / 0x10 blez a0 / 0x18 bgtz a0, all to the jr ra at 0x20
WORDS = [0x04800007, 0, 0x04810005, 0, 0x18800003, 0, 0x1C800001, 0, 0x03E00008, 0]


def _recompiler() -> Path:
    env = os.environ.get("DON_RECOMP")
    path = Path(env) if env else ROOT / "third_party/PS2Recomp/out/build-tools/ps2xRecomp/ps2_recomp"
    if not path.is_file():
        pytest.skip("no recompiler binary (set DON_RECOMP)")
    return path


def _elf(code: bytes) -> bytes:
    shstr = b"\0.text\0.shstrtab\0"
    off_code = 0x80
    off_str = off_code + len(code)
    off_sh = (off_str + len(shstr) + 3) & ~3
    ehdr = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + b"\0" * 9, 2, 8, 1, BASE, 52, off_sh,
                       0x20924001, 52, 32, 1, 40, 3, 2)
    phdr = struct.pack("<8I", 1, off_code, BASE, BASE, len(code), len(code), 5, 4)

    def sh(name, typ, flags, addr, off, size):
        return struct.pack("<10I", name, typ, flags, addr, off, size, 0, 0, 4, 0)

    shs = sh(0, 0, 0, 0, 0, 0) + sh(1, 1, 6, BASE, off_code, len(code)) + sh(7, 3, 0, 0, off_str, len(shstr))
    blob = bytearray(ehdr + phdr)
    blob += b"\0" * (off_code - len(blob)) + code + shstr
    blob += b"\0" * (off_sh - len(blob)) + shs
    return bytes(blob)


def test_sign_branches_test_all_64_bits(tmp_path):
    recomp = _recompiler()
    code = b"".join(struct.pack("<I", w) for w in WORDS)
    (tmp_path / "t.elf").write_bytes(_elf(code))
    (tmp_path / "f.csv").write_text(f"Name,Start,End,Size\nFUN_{BASE:08x},0x{BASE:08x},0x{BASE + len(code):08x},{len(code)}\n")
    (tmp_path / "t.toml").write_text(
        f'[general]\ninput = "{tmp_path / "t.elf"}"\nghidra_output = "{tmp_path / "f.csv"}"\n'
        f'output = "{tmp_path / "gen"}/"\nsingle_file_output = false\npatch_syscalls = false\n'
        'patch_cop0 = true\npatch_cache = true\nstubs = []\n\n[patches]\ninstructions = []\n')
    subprocess.run([str(recomp), str(tmp_path / "t.toml")], cwd=tmp_path, check=True,
                   capture_output=True, timeout=120)
    text = next((tmp_path / "gen").glob("FUN_*.cpp")).read_text()
    conds = dict(re.findall(r"branch_taken_0x([0-9a-f]+) = \((.*)\);", text))
    assert set(conds) == {"100000", "100008", "100010", "100018"}, conds
    assert conds["100000"] == "GPR_S64(ctx, 4) < 0"
    assert conds["100008"] == "GPR_S64(ctx, 4) >= 0"
    assert conds["100010"] == "GPR_S64(ctx, 4) <= 0"
    assert conds["100018"] == "GPR_S64(ctx, 4) > 0"
