# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/verify/profile_diff.py on two tiny fake profiles and maps."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))
import profile_diff  # noqa: E402

MAP = """ don

 Timestamp is 6ae2456b (Wed Oct 28 18:56:43 2026)

 Preferred load address is 0000000140000000

 Address         Publics by Value              Rva+Base               Lib:Object

 0001:00000000       alpha                      0000000140001000     a.obj
 0001:00000100       beta                       0000000140001100     b.obj
"""


def profile(exe_base, samples, cpu_us, taken):
    lines = [
        "# don profile 1", "rate_hz 1000", "thread GameThread",
        "duration_us 1000000", f"cpu_us {cpu_us}", f"samples_taken {taken}",
        "samples_lost 0", f"addresses {len(samples)}", "modules 2",
        f"module 0x{exe_base:016x} 0x{exe_base + 0x10000:016x} 0x0 C:\\x\\don.exe",
        "module 0x00007ff900000000 0x00007ff900100000 0x0 C:\\Windows\\SYSTEM32\\ntdll.dll",
    ]
    lines += [f"0x{a:016x} {c}" for a, c in samples]
    return "\n".join(lines) + "\n"


def write(tmp_path, name, text):
    p = tmp_path / name
    p.write_text(text)
    return p


def test_diff_symbolizes_and_sorts_by_delta(tmp_path):
    base_a, base_b = 0x7ff700000000, 0x7ff710000000
    # RVA 0x1010 is in alpha, 0x1120 in beta; the images load at different bases.
    pa = write(tmp_path, "a.profile", profile(base_a, [(base_a + 0x1010, 100), (base_a + 0x1120, 50), (0x7ff900000500, 7)], 1_000_000, 157))
    pb = write(tmp_path, "b.profile", profile(base_b, [(base_b + 0x1010, 100), (base_b + 0x1120, 150), (0x7ff900000500, 7)], 2_000_000, 257))
    ma, mb = write(tmp_path, "a.map", MAP), write(tmp_path, "b.map", MAP)
    rows, info = profile_diff.diff(pa, ma, pb, mb, blanks=1000)
    by = {r.symbol: r for r in rows}
    # 1 ms per sample in A (1e6 us / 157 samples is not 1000, so scale = cpu_us/samples).
    assert by["beta"].samples_a == 50 and by["beta"].samples_b == 150
    assert abs(by["beta"].ms_a - 50 * (1_000_000 / 157) / 1000 / 1000) < 1e-9
    assert abs(by["beta"].ms_b - 150 * (2_000_000 / 257) / 1000 / 1000) < 1e-9
    assert rows[0].symbol == "beta"  # biggest delta first
    assert by["alpha"].module == "don.exe"
    assert by["alpha"].delta == by["alpha"].ms_b - by["alpha"].ms_a
    assert info["blanks"] == 1000


def test_unmapped_module_without_exports_groups_by_module(tmp_path):
    base = 0x7ff700000000
    p = write(tmp_path, "a.profile", profile(base, [(0x7ff900000500, 5)], 1000, 5))
    m = write(tmp_path, "a.map", MAP)
    rows, _ = profile_diff.diff(p, m, p, m, blanks=10)
    assert rows[0].module == "ntdll.dll" and rows[0].symbol.startswith("ntdll.dll+")


def test_default_blanks_from_duration(tmp_path):
    base = 0x7ff700000000
    p = write(tmp_path, "a.profile", profile(base, [(base + 0x1010, 5)], 1000, 5))
    m = write(tmp_path, "a.map", MAP)
    _, info = profile_diff.diff(p, m, p, m)
    assert info["blanks"] == round(1.0 * profile_diff.BLANK_HZ)


def test_dollar_labels_belong_to_the_previous_function(tmp_path):
    base = 0x7ff700000000
    m = write(tmp_path, "a.map", MAP + " 0001:00000150       $ehgcr_1_2                 0000000140001150     b.obj\n")
    p = write(tmp_path, "a.profile", profile(base, [(base + 0x1160, 3)], 1000, 3))
    rows, _ = profile_diff.diff(p, m, p, m, blanks=1)
    assert rows[0].symbol == "beta"
