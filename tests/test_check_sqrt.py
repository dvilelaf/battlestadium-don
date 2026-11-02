import struct
import sys
from pathlib import Path

# python -I does not add the script folder to sys.path; the tool lives in tools/verify/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import check_sqrt  # noqa: E402

BASE = 0x00100000
PH_OFF = 0x34
DATA_OFF = 0x54


def cop1(fmt, ft, fs, fd, funct):
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


def sqrt_s(fd, ft, fs=0):
    return cop1(0x10, ft, fs, fd, 4)


def make_elf(path, words):
    """Write a synthetic ELF32 little endian file with one PT_LOAD at BASE holding the words."""
    code = struct.pack(f"<{len(words)}I", *words)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    ehdr = struct.pack("<16sHHIIIIIHHHHHH", ident, 2, 8, 1, BASE, PH_OFF, 0, 0, 0x34, 0x20, 1, 0, 0, 0)
    phdr = struct.pack("<8I", 1, DATA_OFF, BASE, BASE, len(code), len(code), 5, 4)
    path.write_bytes(ehdr + phdr + code)
    return path


def make_layout(path, end):
    path.write_text(f"[text]\nstart = 0x{BASE:08X}\nend = 0x{end:08X}\n")
    return path


def make_map(path, rows):
    """rows: [(name, start, end)]"""
    lines = ["Name,Start,End,Size"]
    lines += [f"{n},0x{s:08X},0x{e:08X},{e - s}" for n, s, e in rows]
    path.write_text("\n".join(lines) + "\n")
    return path


def make_generated(folder, files, stubs=""):
    folder.mkdir()
    for name, text in files.items():
        (folder / name).write_text(text)
    (folder / "ps2_recompiled_stubs.h").write_text(stubs)
    return folder


# Words of the synthetic ELF: sqrt.s f7 = sqrt(f5) (ft != fs), a non-sqrt add.s in between, sqrt.s f13 = sqrt(f0).
WORDS = [sqrt_s(fd=13, ft=0), cop1(0x10, 0, 0, 3, 0), sqrt_s(fd=7, ft=5)]
ADDR_A = BASE
ADDR_B = BASE + 8

GOOD = f"""// generated
void FUN_00100000(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {{
    // 0x100000: 0x46000344  c1          0x344
    ctx->pc = 0x{ADDR_A:08X}u;
    // 0x100000: 0x46000344  c1          0x344
    ctx->pc = 0x{ADDR_A:08X}u;
    ctx->f[13] = FPU_SQRT_S(
        fabsf(ctx->f[0]));
    // 0x100008: 0x46000001  c1          0x1
    ctx->pc = 0x{ADDR_B:08X}u;
    ctx->f[7] = FPU_SQRT_S(fabsf(ctx->f[5]));
}}
"""

BAD = f"""// generated
void entry_00100004(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {{
    ctx->pc = 0x{ADDR_B:08X}u;
    ctx->f[7] = FPU_SQRT_S(fabsf(ctx->f[0]));
}}
"""


def test_find_sqrt_words(tmp_path):
    elf = make_elf(tmp_path / "t.elf", WORDS)
    _, base, blob, _, _ = check_sqrt.layout.load(elf)
    found = check_sqrt.find_sqrt(blob, base, BASE, BASE + 12)
    assert found == [(ADDR_A, 13, 0, 0), (ADDR_B, 7, 5, 0)]


def test_collect_sqrt_reads_wrapped_statement_and_every_file(tmp_path):
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD, "bad.cpp": BAD})
    got = check_sqrt.collect_sqrt(gen)
    assert got[ADDR_A] == [("good.cpp", 13, 0)]
    assert sorted(got[ADDR_B]) == [("bad.cpp", 7, 0), ("good.cpp", 7, 5)]


JUMP = f"""void FUN_00100000(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {{
    if (branch_taken) {{
        ctx->pc = 0x{ADDR_B:08X}u;
        goto label_b;
    }}
    ctx->pc = 0x{ADDR_B:08X}u;
label_b:
    // 0x100008: 0x46000001  c1          0x1
    ctx->pc = 0x{ADDR_B:08X}u;
    ctx->f[7] = FPU_SQRT_S(fabsf(ctx->f[5]));
}}
"""


def test_collect_sqrt_takes_the_instruction_after_the_label_not_the_jump_sites(tmp_path):
    gen = make_generated(tmp_path / "gen", {"jump.cpp": JUMP})
    assert check_sqrt.collect_sqrt(gen) == {ADDR_B: [("jump.cpp", 7, 5)]}


def test_compare_reports_the_wrong_occurrence(tmp_path):
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD, "bad.cpp": BAD})
    elf = make_elf(tmp_path / "t.elf", WORDS)
    _, base, blob, _, _ = check_sqrt.layout.load(elf)
    words = check_sqrt.find_sqrt(blob, base, BASE, BASE + 12)
    lines, checked, errors, skipped, ft_ne_fs = check_sqrt.compare(
        words, check_sqrt.collect_sqrt(gen), [])
    assert (checked, errors, skipped, ft_ne_fs) == (2, 1, 0, 1)
    assert lines == [f"error 0x{ADDR_B:08X}: expected fd=7 ft=5, found bad.cpp: dest=f7 src=f0"]


def test_main_end_to_end_fails_on_wrong_file(tmp_path, capsys):
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD, "bad.cpp": BAD})
    elf = make_elf(tmp_path / "t.elf", WORDS)
    layout = make_layout(tmp_path / "layout.toml", BASE + 12)
    rows = [("FUN_00100000", BASE, BASE + 8)]
    code = check_sqrt.main(["--elf", str(elf), "--layout", str(layout), "--generated", str(gen),
                            "--map", str(make_map(tmp_path / "map.csv", rows))])
    out = capsys.readouterr().out.splitlines()
    assert code == 1
    assert out[-2:] == ["sqrt.s: 2 checked, 1 errors", "of which 1 have ft != fs"]


def test_main_passes_when_all_occurrences_are_correct(tmp_path, capsys):
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD})
    elf = make_elf(tmp_path / "t.elf", WORDS)
    layout = make_layout(tmp_path / "layout.toml", BASE + 12)
    code = check_sqrt.main(["--elf", str(elf), "--layout", str(layout), "--generated", str(gen),
                            "--map", str(make_map(tmp_path / "map.csv", []))])
    out = capsys.readouterr().out.splitlines()
    assert code == 0
    assert out == ["sqrt.s: 2 checked, 0 errors", "of which 1 have ft != fs"]


def test_missing_address_inside_replaced_function_is_skipped(tmp_path, capsys):
    # Only ADDR_A is generated; ADDR_B lies inside the replaced function that starts at BASE.
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD.split("// 0x100008")[0] + "}\n"},
                         stubs=f"void FUN_00100000_0x{BASE:x}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);\n")
    elf = make_elf(tmp_path / "t.elf", WORDS)
    layout = make_layout(tmp_path / "layout.toml", BASE + 12)
    rows = [("FUN_00100000", BASE, BASE + 12)]
    code = check_sqrt.main(["--elf", str(elf), "--layout", str(layout), "--generated", str(gen),
                            "--map", str(make_map(tmp_path / "map.csv", rows))])
    out = capsys.readouterr().out.splitlines()
    assert code == 0
    assert out == ["sqrt.s: 1 checked, 0 errors, 1 skipped", "of which 0 have ft != fs"]


def test_missing_address_outside_replaced_function_is_error(tmp_path, capsys):
    gen = make_generated(tmp_path / "gen", {"good.cpp": GOOD.split("// 0x100008")[0] + "}\n"},
                         stubs=f"void FUN_00100000_0x{BASE:x}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);\n")
    elf = make_elf(tmp_path / "t.elf", WORDS)
    layout = make_layout(tmp_path / "layout.toml", BASE + 12)
    # The replaced function ends before ADDR_B, so ADDR_B is not inside it.
    rows = [("FUN_00100000", BASE, BASE + 4)]
    code = check_sqrt.main(["--elf", str(elf), "--layout", str(layout), "--generated", str(gen),
                            "--map", str(make_map(tmp_path / "map.csv", rows))])
    out = capsys.readouterr().out.splitlines()
    assert code == 1
    assert out == [f"error 0x{ADDR_B:08X}: expected fd=7 ft=5, found missing",
                   "sqrt.s: 2 checked, 1 errors", "of which 1 have ft != fs"]
