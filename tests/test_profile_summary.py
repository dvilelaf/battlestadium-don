"""Tests of tools/verify/profile_summary.py (T-203) with synthetic profiles and a fake symbol table.

The profiles are small files written here in the format of src/host/profile.h; the symbols come
from DictSymbolSource, so nothing here needs `nm`, the game or a real binary. The command line is
run as a subprocess too, on a profile whose module is not on this machine (that is the case of a
profile taken somewhere else), to check its exit codes and that it still prints the whole table.
"""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools" / "verify"))

import profile_summary as ps  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tools" / "verify" / "profile_summary.py"

DON = "/opt/don/don"
GS = "/opt/don/libdon_gs.so.1"
CXX = "/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
LIBC = "/usr/lib/x86_64-linux-gnu/libc.so.6"

MODULES = (
    ps.Module(0x100000, 0x200000, 0, DON),
    ps.Module(0x200000, 0x300000, 0, GS),
    ps.Module(0x300000, 0x400000, 0, CXX),
    ps.Module(0x400000, 0x500000, 0, LIBC),
    ps.Module(0x500000, 0x600000, 0, "/opt/don/modules/don_game.dll"),
)


def address(module_index, offset):
    """An address of a synthetic module (offset 0 in every module of this file); None = no module."""
    if module_index is None:
        return 0x9000000 + offset
    module = MODULES[module_index]
    return module.start + offset


# One row per bucket of BUCKET_RULES: the module (its index in MODULES, or None for an address in no
# module) and the symbol name of an address that must land there. A name in the "gfx" row that looks
# like host code shows that the module decides first.
BUCKET_CASES = {
    "gfx": (1, "HostGs::on_thread_begin()"),
    "gamedll": (4, "whatever()"),
    "recomp": (0, "entry_00100100_0x100100(unsigned char*, R5900Context*, PS2Runtime*)"),
    "iop": (0, "ps2x::iop::R3000::step()"),
    "sched": (0, "EeScheduler::run()"),
    "vu": (0, "VU1Interpreter::interpret(unsigned int)"),
    "gif": (0, "PS2Memory::GifWrite(unsigned int, unsigned int)"),
    "handlers": (0, "ps2_syscalls::scePadRead(void*, unsigned int)"),
    "libcxx": (2, "std::__throw_bad_alloc()"),
    "host": (0, "HostAudio::write(float const*, unsigned int)"),
    "runtime": (0, "PS2Runtime::run()"),
    "noload": (None, ""),
    "unknown": (0, "some_c_function(int)"),
}


def make_profile(path, entries, samples_lost=0, rate=1000, thread="GameThread", duration_us=2000000,
                 cpu_us=2000000, samples_taken=None):
    """Writes a profile file: `entries` is a list of (address, count) or (address, count, module, name)."""
    lines = ["# don profile 1", f"rate_hz {rate}", f"thread {thread}", f"duration_us {duration_us}",
             f"cpu_us {cpu_us}",
             f"samples_taken {samples_taken if samples_taken is not None else sum(count for _, count, *_ in entries)}",
             f"samples_lost {samples_lost}", f"addresses {len(entries)}", f"modules {len(MODULES)}"]
    for module in MODULES:
        lines.append(f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}")
    for entry in entries:
        lines.append(f"{entry[0]:#018x} {entry[1]}")
    path.write_text("\n".join(lines) + "\n")
    return path


def source_for(entries):
    """The fake symbol table of `entries` (address, count, module index, symbol name)."""
    tables = {}
    for address_, _, module_index, name in entries:
        if module_index is None:
            continue
        module = MODULES[module_index]
        tables.setdefault(module.path, []).append((address_ - module.start + module.offset, 0x20, name))
    return ps.DictSymbolSource(tables)


@pytest.mark.parametrize("bucket", list(BUCKET_CASES))
def test_each_bucket_is_reached(tmp_path, bucket):
    module_index, name = BUCKET_CASES[bucket]
    entries = [(address(module_index, 0x1000), 1, module_index, name)]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, source_for(entries))

    assert summary.buckets[bucket] == 1
    assert sum(summary.buckets.values()) == 1
    assert summary.symbols[0].bucket == bucket
    assert summary.symbols[0].name == name


def test_the_rules_are_ordered_and_the_graphics_library_comes_first(tmp_path):
    # A name that the host rule would take, inside libdon_gs: the module decides.
    entries = [(address(1, 0x1000), 5, 1, "HostGs::on_thread_begin()")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, source_for(entries))
    assert summary.buckets["gfx"] == 5
    assert summary.buckets["host"] == 0


def test_address_in_no_module_is_outside_the_module_map(tmp_path):
    # 0x9000000 is outside every module: the kernel's vDSO lands here in a real profile.
    entries = [(0x9000000, 7, None, "")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, source_for(entries))
    assert summary.buckets["noload"] == 7
    assert summary.buckets["unknown"] == 0
    assert summary.in_module == 0
    assert summary.with_symbol == 0
    assert "outside the module map" in ps.format_summary(summary, top=5)


def test_address_between_two_sized_symbols_has_no_symbol(tmp_path):
    base = address(0, 0x1000)
    entries = [(base, 3, 0, "EeScheduler::run()"), (base + 0x100, 4, 0, "PS2Runtime::run()")]
    # The fake table gives every symbol size 0x20, so `base + 0x50` is in the gap between them.
    tables = {DON: [(0x1000, 0x20, "EeScheduler::run()"), (0x1100, 0x20, "PS2Runtime::run()")]}
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries + [(base + 0x50, 9, 0, "")]))
    summary = ps.analyze(profile, ps.DictSymbolSource(tables))

    assert summary.buckets["unknown"] == 9
    assert summary.buckets["sched"] == 3
    assert summary.buckets["runtime"] == 4
    assert summary.in_module == 9 + 3 + 4  # the gap is inside a module, it just has no symbol
    assert summary.with_symbol == 3 + 4


def test_shares_add_up_to_one_hundred(tmp_path):
    entries = [(address(0, 0x1000 + index * 0x100), index + 1, 0, name)
               for index, name in enumerate(("PS2Runtime::run()", "EeScheduler::run()", "some_c()"))]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, source_for(entries))

    total = sum(summary.buckets.values())
    assert total == summary.samples == sum(count for _, count, *_ in entries)
    shares = [100.0 * value / total for value in summary.buckets.values()]
    assert sum(shares) == pytest.approx(100.0)
    for key in ps.BUCKET_ORDER:
        assert ps.BUCKET_LABELS[key]


def test_the_module_file_offset_is_used(tmp_path):
    # A module mapped with a file offset: the symbol table is in virtual addresses, equal to the file
    # offsets here because the segment delta is zero.
    module = ps.Module(0x700000, 0x800000, 0x9000, "/opt/don/don")
    entries = [(0x701234, 11, 0, "ps2x::iop::R3000::step()")]
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1",
             "samples_taken 11", "samples_lost 0", "addresses 1", "modules 1",
             f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}",
             f"{0x701234:#018x} 11"]
    path = tmp_path / "p.txt"
    path.write_text("\n".join(lines) + "\n")
    profile = ps.read_profile(path)
    # 0x701234 - 0x700000 + 0x9000 = 0xA234, which is the symbol's file address here.
    source = ps.DictSymbolSource({module.path: [(0xA234, 0x40, "ps2x::iop::R3000::step()")]})
    summary = ps.analyze(profile, source)
    assert summary.buckets["iop"] == 11
    assert summary.with_symbol == 11


def test_game_functions_are_grouped_by_guest_address(tmp_path):
    # The same function sampled at three instructions, plus a second function.
    symbols = {
        DON: [
            (0x1000, 0x40, "entry_00200020_0x200020(unsigned char*, R5900Context*, PS2Runtime*)"),
            (0x1040, 0x40, "FUN_00136770_0x136770"),
        ]
    }
    entries = [(address(0, 0x1000), 100, 0, ""), (address(0, 0x1010), 50, 0, ""),
               (address(0, 0x1020), 25, 0, ""), (address(0, 0x1040), 10, 0, "")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, ps.DictSymbolSource(symbols), {0x200020: "do_something"})

    assert [entry.guest_address for entry in summary.game] == [0x200020, 0x136770]
    assert [entry.samples for entry in summary.game] == [175, 10]
    assert summary.game[0].name == "do_something"  # the name from the functions CSV
    assert summary.game[1].name == "FUN_00136770_0x136770"
    assert summary.buckets["recomp"] == 185

    text = ps.format_summary(summary, top=1)
    assert "do_something" in text
    assert "0x00200020" in text
    assert "0x00136770" not in text  # only the top one
    assert "recompiled game code" in text


def test_format_summary_shows_every_bucket_and_the_totals(tmp_path):
    entries = [(address(0, 0x1000), 3, 0, "PS2Runtime::run()"), (address(2, 0x2000), 1, 2, "std::abort()")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, source_for(entries))
    text = ps.format_summary(summary, top=30)

    for key in ps.BUCKET_ORDER:
        assert ps.BUCKET_LABELS[key] in text
    assert "total" in text
    assert "C and C++ libraries" in text
    assert "75.0%" in text  # three of four samples in PS2Runtime::run()
    assert "100.0%" in text


def test_a_lost_sample_is_reported_but_not_in_the_counts(tmp_path):
    entries = [(address(0, 0x1000), 2, 0, "PS2Runtime::run()")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries, samples_lost=17))
    summary = ps.analyze(profile, source_for(entries))
    assert summary.samples == 2
    assert "17 lost" in ps.format_summary(summary, top=5)


def test_read_profile_rejects_a_bad_file(tmp_path):
    good = make_profile(tmp_path / "good.txt", [(address(0, 0x1000), 1, 0, "some_c()")])
    assert ps.read_profile(good).rate_hz == 1000

    path = tmp_path / "junk.txt"
    path.write_text("hello world\n")
    with pytest.raises(ValueError):
        ps.read_profile(path)

    path.write_text("# don profile 1\nrate_hz 1000\nmodule 0x1\n")
    with pytest.raises(ValueError):
        ps.read_profile(path)

    path.write_text("# don profile 1\nrate_hz 1000\n0xabc not-a-count\n")
    with pytest.raises(ValueError):
        ps.read_profile(path)


def test_parse_nm_reads_sizes_types_and_spaces():
    text = "\n".join([
        "0000000000001000 0000000000000020 T busy_a(double, unsigned long)",
        "0000000000001020 T _init",
        "0000000000002000 0000000000000004 r __abi_tag",
        "",
    ])
    symbols = ps.parse_nm(text)
    assert [symbol.name for symbol in symbols] == ["busy_a(double, unsigned long)", "_init", "__abi_tag"]
    assert symbols[0] == ps.Symbol(0x1000, 0x20, "busy_a(double, unsigned long)", "T")
    assert symbols[1].size == 0 and symbols[1].is_text
    assert not symbols[2].is_text  # read-only data

    # A lookup never lands in data: the last text symbol at or before the address decides.
    assert ps.find_symbol(symbols, 0x1010).name == "busy_a(double, unsigned long)"
    assert ps.find_symbol(symbols, 0x1020).name == "_init"
    assert ps.find_symbol(symbols, 0x2000) is None
    assert ps.find_symbol([], 0x1000) is None


def test_nm_source_says_what_it_could_not_read(tmp_path):
    module = ps.Module(0x100000, 0x200000, 0, str(tmp_path / "not-here"))
    source = ps.NmSymbolSource()
    assert source.symbols_for(module) == []
    assert any("not on this machine" in note for note in source.notes())

    entries = [(0x100100, 4, 0, "")]
    profile = ps.read_profile(make_profile(tmp_path / "p.txt", entries))
    summary = ps.analyze(profile, ps.NmSymbolSource(program="/nonexistent/nm"))
    assert summary.buckets["unknown"] == 4
    assert any("nm" in note for note in summary.notes)


def test_command_line_prints_the_summary_and_its_exit_codes(tmp_path):
    profile_path = make_profile(tmp_path / "p.txt", [(address(0, 0x1000), 4, 0, "PS2Runtime::run()")])

    result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(profile_path), "--top", "3"],
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stderr
    assert "subsystem" in result.stdout
    assert "thread GameThread" in result.stdout
    assert "2000 expected from the thread's processor time" in result.stdout

    missing = subprocess.run([sys.executable, "-I", str(SCRIPT), str(tmp_path / "no.txt")],
                             capture_output=True, text=True, timeout=120)
    assert missing.returncode == 2
    assert "no such file" in missing.stderr

    bad_csv = subprocess.run([sys.executable, "-I", str(SCRIPT), str(profile_path), "--csv",
                              str(tmp_path / "no.csv")], capture_output=True, text=True, timeout=120)
    assert bad_csv.returncode == 2


def test_load_functions_reads_the_csv(tmp_path):
    csv_path = tmp_path / "functions.csv"
    csv_path.write_text("Name,Start,End,Size\n"
                        "FUN_00136770,0x00136770,0x00136a00,656\n"
                        "JUNK,not-a-number,0x10,0\n")
    functions = ps.load_functions(csv_path)
    assert functions == {0x136770: "FUN_00136770"}


def test_game_address_regex_accepts_the_names_the_recompiler_writes():
    for name, expected in [
        ("FUN_00136770_0x136770", 0x136770),
        ("entry_002e2030_0x2e2030(unsigned char*, R5900Context*, PS2Runtime*)", 0x2E2030),
        ("sceGsSyncV_0x1011d8", 0x1011D8),
        ("entry_00375640_0x375640(...)::$_8::operator()", 0x375640),
        ("caseD_1_0x1bb210(unsigned char*, R5900Context*, PS2Runtime*)", 0x1BB210),
    ]:
        match = ps.GAME_ADDRESS_RE.search(name)
        assert match is not None, name
        assert int(match.group(1), 16) == expected
    # Not game code: the generated file's static initializer, and names without an address.
    assert ps.GAME_ADDRESS_RE.search("_GLOBAL__sub_I_FUN_0010e088_0x10e088.cpp") is None
    assert ps.GAME_ADDRESS_RE.search("PS2Runtime::run()") is None


# ---- the file offset -> virtual address translation (the lld trap) ---------------------------------
# A sampled address becomes a file offset (address - module start + the mapping's file offset); nm's
# symbol values are virtual addresses. The two differ by p_vaddr - p_offset of the segment that holds
# the offset: zero with GNU ld (the probe), 0x1000 in the port's code segment with lld. Without the
# translation every symbol in `don` was named by whatever lay 4 KiB before it.


def test_a_sample_is_named_by_its_virtual_address_not_its_file_offset(tmp_path):
    # The module's code segment has p_offset 0x1000, p_vaddr 0x2000: a file offset F is the virtual
    # address F + 0x1000. The decoy sits exactly at F.
    module = ps.Module(0x100000, 0x200000, 0x1000, "/opt/don/don")
    sampled = 0x101000                                   # file offset 0x2000, virtual address 0x3000
    file_offset = sampled - module.start + module.offset
    tables = {module.path: [(file_offset, 0x40, "decoy_at_the_file_offset()"),
                            (file_offset + 0x1000, 0x40, "the_function_at_the_virtual_address()")]}
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1", "samples_taken 5",
             "samples_lost 0", "addresses 1", "modules 1",
             f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}",
             f"{sampled:#018x} 5"]
    path = tmp_path / "p.txt"
    path.write_text("\n".join(lines) + "\n")
    profile = ps.read_profile(path)
    segments = ps.DictSegmentSource({module.path: [(0x1000, 0x2000, 0x100000)]})

    summary = ps.analyze(profile, ps.DictSymbolSource(tables), segments=segments)
    assert summary.symbols[0].name == "the_function_at_the_virtual_address()"
    assert summary.symbols[0].samples == 5

    # Without the translation (the old behaviour) the decoy is named: that is the bug this test pins.
    wrong = ps.analyze(profile, ps.DictSymbolSource(tables))
    assert wrong.symbols[0].name == "decoy_at_the_file_offset()"


def test_two_segments_with_different_deltas(tmp_path):
    module = ps.Module(0x200000, 0x300000, 0x4000, "/opt/don/don")
    # One segment with delta 0x1000 before 0x20000, one with delta 0x2000 after.
    segments = ps.DictSegmentSource({module.path: [(0x4000, 0x5000, 0x1C000), (0x20000, 0x22000, 0x1000)]})
    first = 0x200000 + 0x8000                            # file offset 0xC000 -> vaddr 0xD000
    second = 0x200000 + 0x1C500                          # file offset 0x20500 -> vaddr 0x22500
    tables = {module.path: [(0xD000, 0x40, "in_the_first_segment()"), (0x22500, 0x40, "in_the_second_segment()")]}
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1", "samples_taken 2",
             "samples_lost 0", "addresses 2", "modules 1",
             f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}",
             f"{first:#018x} 3", f"{second:#018x} 4"]
    path = tmp_path / "p.txt"
    path.write_text("\n".join(lines) + "\n")
    summary = ps.analyze(ps.read_profile(path), ps.DictSymbolSource(tables), segments=segments)
    names = {entry.name: entry.samples for entry in summary.symbols}
    assert names == {"in_the_first_segment()": 3, "in_the_second_segment()": 4}


def test_equal_deltas_need_no_translation(tmp_path):
    # GNU ld: p_vaddr == p_offset, so the file offset is the virtual address and nothing changes.
    module = ps.Module(0x300000, 0x400000, 0x1000, "/opt/don/don")
    sampled = 0x303000                                   # file offset 0x4000 == virtual address 0x4000
    tables = {module.path: [(0x4000, 0x40, "same_for_both()")]}
    segments = ps.DictSegmentSource({module.path: [(0x1000, 0x1000, 0x100000)]})
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1", "samples_taken 2",
             "samples_lost 0", "addresses 1", "modules 1",
             f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}",
             f"{sampled:#018x} 2"]
    path = tmp_path / "p.txt"
    path.write_text("\n".join(lines) + "\n")
    summary = ps.analyze(ps.read_profile(path), ps.DictSymbolSource(tables), segments=segments)
    assert summary.symbols[0].name == "same_for_both()"
    assert summary.notes == []


def test_a_file_offset_outside_every_segment_is_reported(tmp_path):
    module = ps.Module(0x400000, 0x500000, 0x1000, "/opt/don/don")
    sampled = 0x490000                                   # file offset 0x91000: past the only segment
    tables = {module.path: [(0x91000, 0x40, "looked_up_at_the_file_offset()")]}
    segments = ps.DictSegmentSource({module.path: [(0x1000, 0x2000, 0x1000)]})
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1", "samples_taken 1",
             "samples_lost 0", "addresses 1", "modules 1",
             f"module {module.start:#018x} {module.end:#018x} {module.offset:#018x} {module.path}",
             f"{sampled:#018x} 1"]
    path = tmp_path / "p.txt"
    path.write_text("\n".join(lines) + "\n")
    summary = ps.analyze(ps.read_profile(path), ps.DictSymbolSource(tables), segments=segments)
    assert summary.symbols[0].name == "looked_up_at_the_file_offset()"  # best effort, as before
    assert any("outside every known segment" in note for note in summary.notes)


def test_the_elf_segment_source_reads_a_real_elf64(tmp_path):
    # A tiny but genuine ELF64 little endian file: one PT_LOAD (p_offset 0x1000, p_vaddr 0x2000, filesz 0x10).
    header = bytearray(64)
    header[0:4] = b"\x7fELF"
    header[4] = 2   # ELFCLASS64
    header[5] = 1   # ELFDATA2LSB
    header[6] = 1   # EV_CURRENT
    import struct as _struct
    _struct.pack_into("<Q", header, 0x20, 64)   # e_phoff
    _struct.pack_into("<H", header, 0x36, 56)   # e_phentsize
    _struct.pack_into("<H", header, 0x38, 1)    # e_phnum
    phdr = bytearray(56)
    _struct.pack_into("<I", phdr, 0, ps.PT_LOAD)
    _struct.pack_into("<QQQQQQ", phdr, 8, 0x1000, 0x2000, 0x2000, 0x10, 0x10, 0x1000)
    path = tmp_path / "fake.elf"
    path.write_bytes(bytes(header) + bytes(phdr) + bytes(0x40))
    module = ps.Module(0x100000, 0x200000, 0, str(path))
    segments = ps.ElfSegmentSource().segments_for(module)
    assert segments == [ps.Segment(0x1000, 0x2000, 0x10)]
    assert ps.virtual_address_of(module, 0x100000 + 0x1008, segments) == 0x2008
    # A file that is not an ELF gives no segments at all (the caller falls back to the file offset).
    junk = tmp_path / "junk.so"
    junk.write_bytes(b"not an elf at all, but long enough to pass the length check ....................")
    assert ps.ElfSegmentSource().segments_for(ps.Module(0, 1, 0, str(junk))) == []
    assert ps.ElfSegmentSource().segments_for(ps.Module(0, 1, 0, str(tmp_path / "missing.so"))) == []


def test_a_module_newer_than_the_profile_is_flagged(tmp_path):
    binary = tmp_path / "don"
    binary.write_bytes(b"x" * 16)
    profile_path = tmp_path / "p.txt"
    lines = ["# don profile 1", "rate_hz 1000", "thread t", "duration_us 1", "cpu_us 1", "samples_taken 1",
             "samples_lost 0", "addresses 1", "modules 1",
             f"module {0x100000:#018x} {0x200000:#018x} {0:#018x} {binary}",
             f"{0x101000:#018x} 1"]
    profile_path.write_text("\n".join(lines) + "\n")
    import os as _os
    profile = ps.read_profile(profile_path)
    _os.utime(profile_path, (1_000_000, 1_000_000))      # the profile is old...
    _os.utime(binary, (2_000_000, 2_000_000))            # ...and the binary was rebuilt after it
    summary = ps.analyze(profile, ps.DictSymbolSource({str(binary): [(0x1000, 0x40, "x()")]}))
    assert any("newer than the profile" in note for note in summary.notes)
    assert "symbols may not match" in ps.format_summary(summary, top=5)
    # And nothing is flagged when the file is older (the normal case).
    _os.utime(binary, (500_000, 500_000))
    assert not any("newer than the profile" in note
                   for note in ps.analyze(profile, ps.DictSymbolSource({str(binary): [(0x1000, 0x40, "x()")]})).notes)


# ---------------------------------------------------------------------------------------------
# T-759: a profile written by src/host/profile_win32.cpp. Its module lines are whole images
# (start = image base, offset 0, so address - start is the RVA) with Windows paths, possibly with
# spaces; the game DLL has no symbols, so its samples are grouped by module + offset.
# ---------------------------------------------------------------------------------------------
WIN_EXE = r"C:\Games\DON\Battle Stadium DON.exe"
WIN_DLL = r"C:\Games\DON\modules\don_game.dll"


def make_windows_profile(path):
    lines = ["# don profile 1", "rate_hz 1000", "thread GameThread", "duration_us 3000000", "cpu_us 2900000",
             "samples_taken 10", "samples_lost 0", "addresses 4", "modules 2",
             f"module {0x140000000:#018x} {0x140800000:#018x} {0:#018x} {WIN_EXE}",
             f"module {0x7ff800000000:#018x} {0x7ff800400000:#018x} {0:#018x} {WIN_DLL}",
             f"{0x7ff800012340:#018x} 4", f"{0x7ff800012350:#018x} 3",
             f"{0x7ff800045610:#018x} 2", f"{0x140001000:#018x} 1"]
    path.write_text("\n".join(lines) + "\n")
    return path


def test_windows_profile_groups_module_samples_by_module_and_offset(tmp_path):
    profile = ps.read_profile(make_windows_profile(tmp_path / "w.txt"))
    assert profile.modules[0].path == WIN_EXE  # the spaces of the path survive
    summary = ps.analyze(profile, ps.DictSymbolSource({}))

    by_name = {entry.name: entry.samples for entry in summary.symbols}
    # 0x12340 and 0x12350 share one 0x100-byte group; 0x45610 is another; the exe is its own module.
    assert by_name["don_game.dll+0x12300"] == 7
    assert by_name["don_game.dll+0x45600"] == 2
    assert by_name["Battle Stadium DON.exe+0x1000"] == 1
    assert summary.buckets["gamedll"] == 9
    assert summary.in_module == 10
    assert summary.with_symbol == 10
    assert not any("ELF" in note or "not on this machine" in note for note in summary.notes)


def test_windows_profile_prints_the_module_groups(tmp_path):
    profile = ps.read_profile(make_windows_profile(tmp_path / "w.txt"))
    text = ps.format_summary(ps.analyze(profile, ps.DictSymbolSource({})), 5)
    assert "don_game.dll+0x12300" in text
    assert "game module DLL" in text


# ---------------------------------------------------------------------------------------------
# T-765: the optional `callers <n>` section (DON_PROFILE_CALLERS=1 on Windows): lines
# `<pc> <caller> <count>`, the return address read from the top of the stopped thread's stack.
# ---------------------------------------------------------------------------------------------
def make_callers_profile(path):
    make_windows_profile(path)
    with path.open("a") as handle:
        handle.write("callers 3\n")
        handle.write(f"{0x7ff800012340:#018x} {0x140001000:#018x} 3\n")
        handle.write(f"{0x7ff800012340:#018x} {0x7ff800045610:#018x} 1\n")
        handle.write(f"{0x140001000:#018x} {0x7ff800045610:#018x} 1\n")
    return path


def test_the_callers_section_is_read_and_is_optional(tmp_path):
    profile = ps.read_profile(make_callers_profile(tmp_path / "c.txt"))
    assert profile.callers[(0x7ff800012340, 0x140001000)] == 3
    assert len(profile.callers) == 3
    assert len(profile.counts) == 4  # the caller lines are not sample lines
    assert ps.read_profile(make_windows_profile(tmp_path / "w.txt")).callers == {}


def test_the_summary_prints_the_top_callers_of_the_top_pcs(tmp_path):
    profile = ps.read_profile(make_callers_profile(tmp_path / "c.txt"))
    text = ps.format_summary(ps.analyze(profile, ps.DictSymbolSource({})), 5)
    assert "callers of the top" in text
    assert "don_game.dll+0x12300" in text
    assert "Battle Stadium DON.exe+0x1000" in text
    assert "(4 of 4 with a caller)" in text
    # Without the section nothing is printed.
    plain = ps.format_summary(ps.analyze(ps.read_profile(make_windows_profile(tmp_path / "w.txt")),
                                         ps.DictSymbolSource({})), 5)
    assert "callers of the top" not in plain
