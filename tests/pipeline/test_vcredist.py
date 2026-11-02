"""Tests of the app-local VC++ runtime (T-770): the pin, the PE import reader and the package check. No download."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "pipeline"))
import fetch_vcredist as fv  # noqa: E402
import package_checks as pc  # noqa: E402


def fake_pe(imports: list[str] = (), delay: list[str] = (), machine: int = 0x8664) -> bytes:
    """A minimal PE32+ with one section (RVA 0x1000 = file offset 0x400) holding import and delay-import tables."""
    base_rva, base_off = 0x1000, 0x400

    def table(names: list[str], entry: int, name_field: int, at: int) -> tuple[bytes, bytes]:
        """Descriptors (entry bytes each, name RVA at name_field, last one zero) followed by the names."""
        names_at = at + entry * (len(names) + 1)
        descriptors, strings = b"", b""
        for name in names:
            rva = base_rva + names_at + len(strings)
            desc = bytearray(entry)
            struct.pack_into("<I", desc, name_field, rva)
            struct.pack_into("<I", desc, 0, 1)   # a non-zero first field marks a used entry
            descriptors += bytes(desc)
            strings += name.encode() + b"\0"
        return descriptors + bytes(entry) + strings, b""

    body = bytearray()
    imp, _ = table(list(imports), 20, 12, 0)
    imp_at = 0
    body += imp
    delay_at = len(body)
    dly, _ = table(list(delay), 32, 4, delay_at)
    body += dly
    pe = 0x80
    opt = 240
    headers = bytearray(base_off)
    headers[:2] = b"MZ"
    struct.pack_into("<I", headers, 0x3C, pe)
    headers[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", headers, pe + 4, machine, 1, 0, 0, 0, opt, 0x22)
    o = pe + 24
    struct.pack_into("<H", headers, o, 0x20B)
    struct.pack_into("<I", headers, o + 108, 16)   # NumberOfRvaAndSizes
    dirs = o + 112
    if imports:
        struct.pack_into("<II", headers, dirs + 8, base_rva + imp_at, 20 * (len(imports) + 1))
    if delay:
        struct.pack_into("<II", headers, dirs + 13 * 8, base_rva + delay_at, 32 * (len(delay) + 1))
    sec = o + opt
    headers[sec:sec + 8] = b".idata\0\0"
    struct.pack_into("<IIII", headers, sec + 8, len(body), base_rva, len(body), base_off)
    return bytes(headers) + bytes(body)


def test_pin_file_has_a_sha256_and_a_version():
    pin = fv.read_pin()
    assert len(pin["sha256"]) == 64 and int(pin["sha256"], 16)
    assert pin["version"].count(".") == 3
    assert pin["sha256"].upper() in pin["url"]   # Microsoft's content-addressed URL carries the hash


def test_pe_imports_reads_normal_and_delay_imports():
    data = fake_pe(["KERNEL32.dll", "MSVCP140.dll"], delay=["vcruntime140_1.dll"])
    assert pc.pe_imports(data) == ["kernel32.dll", "msvcp140.dll", "vcruntime140_1.dll"]


def test_pe_imports_of_a_file_without_imports_and_of_a_non_pe():
    assert pc.pe_imports(fake_pe()) == []
    assert pc.pe_imports(b"not a pe") == []


def test_vc_runtime_names():
    for name in ("msvcp140.dll", "MSVCP140_ATOMIC_WAIT.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
                 "vcomp140.dll", "mfc140u.dll"):
        assert pc.is_vc_runtime(name), name
    # the UCRT is part of Windows 10+, the rest are system DLLs
    for name in ("ucrtbase.dll", "api-ms-win-crt-runtime-l1-1-0.dll", "kernel32.dll", "msvcrt.dll", "SDL3.dll"):
        assert not pc.is_vc_runtime(name), name


def stage(tmp_path: Path, exe_imports: list[str], shipped: list[str], recomp_imports: list[str] = ()) -> Path:
    root = tmp_path / "pkg"
    (root / "bin").mkdir(parents=True)
    (root / "toolchain" / "bin").mkdir(parents=True)
    (root / "Battle Stadium DON.exe").write_bytes(fake_pe(exe_imports))
    (root / "bin" / "ps2_recomp.exe").write_bytes(fake_pe(recomp_imports))
    (root / "toolchain" / "bin" / "clang++.exe").write_bytes(fake_pe(["msvcp140.dll"]))   # the toolchain's own: ignored
    for name in shipped:
        (root / name).write_bytes(fake_pe(["kernel32.dll"]))
    return root


def test_check_passes_when_every_runtime_import_ships(tmp_path):
    root = stage(tmp_path, ["kernel32.dll", "msvcp140.dll", "vcruntime140.dll"], ["msvcp140.dll", "VCRUNTIME140.dll"])
    assert pc.check_vc_runtime(root) == []


def test_check_names_a_missing_runtime_dll_and_who_imports_it(tmp_path):
    root = stage(tmp_path, ["msvcp140.dll", "msvcp140_atomic_wait.dll"], ["msvcp140.dll"])
    problems = pc.check_vc_runtime(root)
    assert len(problems) == 1
    assert "msvcp140_atomic_wait.dll" in problems[0] and "Battle Stadium DON.exe" in problems[0]


def test_check_follows_the_imports_of_the_shipped_runtime_dlls(tmp_path):
    root = stage(tmp_path, ["msvcp140.dll"], [])
    (root / "msvcp140.dll").write_bytes(fake_pe(["vcruntime140.dll"]))   # the shipped DLL needs another one
    problems = pc.check_vc_runtime(root)
    assert len(problems) == 1 and "vcruntime140.dll" in problems[0] and "msvcp140.dll" in problems[0]


def test_check_wants_the_dlls_beside_bin_ps2_recomp(tmp_path):
    root = stage(tmp_path, [], [], recomp_imports=["vcruntime140.dll"])
    (root / "vcruntime140.dll").write_bytes(fake_pe())
    problems = pc.check_vc_runtime(root)
    assert len(problems) == 1 and "bin" in problems[0].replace("\\", "/")
    (root / "bin" / "vcruntime140.dll").write_bytes(fake_pe())
    assert pc.check_vc_runtime(root) == []
