"""Build, recompile, compile and run tiny guest programs; compare with the reference model.

A Program is a list of guest functions (name, words). The harness
  1. lays the functions out in a tiny MIPS ELF (no game data),
  2. runs the port's own recompiler on it (DON_RECOMP, or the binary tools/pipeline/generate.sh uses),
  3. compiles the generated C++ together with driver.cpp against the runtime's headers, with the project's
     floating-point rules (-ffp-contract=off, -march=x86-64-v3, no fast-math), in build/conformance/<name>/,
  4. runs the driver on the cases and returns the registers after every case.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import struct
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASE = 0x100000
SPACING = 0x80
RT = ROOT / "third_party/PS2Recomp"
RT_RUNTIME = Path(os.environ.get("DON_RT_DIR") or RT)       # DON_RT_DIR: another tree whose runtime headers are used (to try a patch)
DEFAULT_RECOMP = RT / "out/build-tools/ps2xRecomp/ps2_recomp"
NREG = 132                      # r0..r31, HI LO HI1 LO1 SA, F0..F31 ACC FCR31, then the vector unit (driver.cpp)
CXXFLAGS = ["-std=gnu++20", "-O2", "-DNDEBUG", "-march=x86-64-v3", "-ffp-contract=off"]


class ToolsMissing(Exception):
    pass


@dataclass
class Func:
    name: str
    words: list[int]
    addr: int = 0


@dataclass
class Program:
    name: str
    funcs: list[Func] = field(default_factory=list)

    def add(self, name: str, words: list[int]) -> int:
        assert len(words) * 4 <= SPACING
        self.funcs.append(Func(name, list(words), BASE + SPACING * len(self.funcs)))
        return len(self.funcs) - 1


def recompiler() -> Path:
    path = Path(os.environ.get("DON_RECOMP") or DEFAULT_RECOMP)
    if not path.is_file():
        raise ToolsMissing(f"no recompiler binary at {path} (set DON_RECOMP)")
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


def _symbol(f: Func) -> str:
    return f"FUN_{f.addr:08x}"           # the recompiler's C++ name is FUN_<addr>_0x<addr>


def build(prog: Program, workdir: Path) -> Path:
    """Recompile and compile the program; return the driver executable. Cached by content."""
    recomp = recompiler()
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        raise ToolsMissing("no C++ compiler")
    code = bytearray(SPACING * len(prog.funcs))
    for f in prog.funcs:
        off = f.addr - BASE
        code[off:off + 4 * len(f.words)] = b"".join(struct.pack("<I", w) for w in f.words)
    key = hashlib.sha256(bytes(code) + recomp.read_bytes() + (HERE / "driver.cpp").read_bytes()
                         + (RT_RUNTIME / "ps2xRuntime/include/ps2_runtime_macros.h").read_bytes()
                         + " ".join(CXXFLAGS).encode()).hexdigest()[:16]
    exe = workdir / "driver"
    stamp = workdir / "key"
    if exe.is_file() and stamp.is_file() and stamp.read_text() == key:
        return exe
    shutil.rmtree(workdir, ignore_errors=True)
    workdir.mkdir(parents=True)
    (workdir / "t.elf").write_bytes(_elf(bytes(code)))
    rows = "".join(f"{_symbol(f)},0x{f.addr:08x},0x{f.addr + 4 * len(f.words):08x},{4 * len(f.words)}\n"
                   for f in prog.funcs)
    (workdir / "f.csv").write_text("Name,Start,End,Size\n" + rows)
    gen = workdir / "gen"
    (workdir / "t.toml").write_text(
        f'[general]\ninput = "{workdir / "t.elf"}"\nghidra_output = "{workdir / "f.csv"}"\n'
        f'output = "{gen}/"\nsingle_file_output = false\npatch_syscalls = false\n'
        'patch_cop0 = true\npatch_cache = true\nstubs = []\n\n[patches]\ninstructions = []\n')
    r = subprocess.run([str(recomp), str(workdir / "t.toml")], cwd=workdir, capture_output=True, text=True,
                       timeout=300)
    (workdir / "recomp.log").write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError(f"recompiler failed ({r.returncode}), see {workdir / 'recomp.log'}")
    (workdir / "table.inc").write_text(
        "".join(f'    {{"{_symbol(f)}", {_symbol(f)}_0x{f.addr:x}}},\n' for f in prog.funcs))
    # one translation unit: the runtime headers are parsed once (the generated functions have unique names)
    unity = workdir / "unity.cpp"
    unity.write_text("".join(f'#include "{p}"\n' for p in sorted(gen.glob("FUN_*.cpp"))))
    srcs = [unity]
    inc = [f"-I{RT_RUNTIME}/ps2xRuntime/include", f"-I{RT_RUNTIME}/ps2xRuntime/src/lib/Kernel", f"-I{RT}/ps2xIOP/include",
           f"-I{gen}", f"-I{workdir}"]
    cmd = [cxx, *CXXFLAGS, *inc, str(HERE / "driver.cpp"), *map(str, srcs), "-o", str(exe)]
    c = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    (workdir / "compile.log").write_text(" ".join(cmd) + "\n" + c.stdout + c.stderr)
    if c.returncode != 0:
        raise RuntimeError(f"compile failed, see {workdir / 'compile.log'}:\n{c.stderr[-1500:]}")
    stamp.write_text(key)
    return exe


def run_cases(exe: Path, cases: list[tuple[int, list[tuple[int, int]]]], workdir: Path):
    """cases: (function index, NREG (lo, hi) registers: r0..r31, HI, LO, HI1, LO1, SA, F0..F31, ACC, FCR31). Returns (the 71 registers, {block offset: byte} of the bytes that differ from the pattern) after each case."""
    lines = []
    for idx, regs in cases:
        lines.append(f"{idx} " + " ".join(f"{lo:x} {hi:x}" for lo, hi in regs))
    inp = workdir / "cases.txt"
    inp.write_text("\n".join(lines) + "\n")
    r = subprocess.run([str(exe), str(inp)], capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError(f"driver exited with {r.returncode}: {r.stderr[-500:]}")
    out = []
    for line in r.stdout.strip().splitlines():
        if line.startswith("THROW"):             # the generated code throws: the instruction is not translated
            out.append((line, {}))
            continue
        n = [int(x, 16) for x in line.split()]
        regs = [(n[2 * i], n[2 * i + 1]) for i in range(NREG)]
        count = n[2 * NREG]
        mem = {n[2 * NREG + 1 + 2 * i]: n[2 * NREG + 2 + 2 * i] for i in range(count)}      # offset in the block -> byte, only the changed ones
        out.append((regs, mem))
    if len(out) != len(cases):
        raise RuntimeError(f"driver printed {len(out)} lines for {len(cases)} cases")
    return out
