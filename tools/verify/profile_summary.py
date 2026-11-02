#!/usr/bin/env python3
"""Summarises a profile written by the profiler of the port (T-203).

The port writes a profile when it is started with DON_PROFILE=<file>:

    DON_PROFILE=work/profile-tests/run.txt ./build/linux-dev/don --seconds 60

The file holds raw host instruction pointers (the port does no symbol lookup; see
src/host/profile.h for the format). This script turns them into symbols and subsystems:

    uv run --no-project python -I tools/verify/profile_summary.py work/profile-tests/run.txt --top 30

It prints (1) the share of samples per subsystem, (2) the top symbols, and (3) the top recompiled
game functions. To do that it maps every sampled address to its module (the module lines of the
profile: start, end, file offset, path) and then to a symbol: the address inside the module becomes
a file address (address - module start + module file offset) and is looked up in the symbol table of
the module's file, which is read with `nm`:

    nm -C --defined-only -S <file>            the full symbol table
    nm -D -C --defined-only -S <file>         only when the first one is empty (a stripped library)

The program to run is `nm`, or $DON_NM (for example llvm-nm) when it is set. A module whose file is
not on this machine, or that nm cannot read, contributes no symbols: its samples are still counted,
with an empty symbol name. Nothing here is game data: the output names symbols and addresses only.

Windows profiles (T-759, src/host/profile_win32.cpp): the module lines have Windows paths
(`C:\\...`, spaces allowed) and cover the whole image with file offset 0, so address - start is
the RVA. nm and the ELF headers are not used for them (the files are PE, usually on another
machine); the samples of such a module are grouped by module + offset, in groups of WINDOWS_GROUP
(0x100) bytes, and named `<file>+0x<offset>` (for example `don_game.dll+0x12300`). The game DLL
(don_game*.dll) has its own row, "game module DLL (Windows, no symbols)", in the subsystem table.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import os
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# =============================================================================================
# THE BUCKET RULES. One row per subsystem, in decision order: the first row whose test matches
# decides, so the order matters (the graphics library and the game code come first). `name` is the
# demangled symbol name (an empty string when the address is inside a module but in no symbol) and
# `module` is the Module it belongs to (None when the address is in no module: the profile lists
# only executable mappings that come from a file, so the kernel's vDSO - where a glibc
# clock_gettime runs, the scheduler's clock read on every dispatch - lands here).
#
# This table, and nothing else, decides the subsystem of a sample. The helpers it uses are below.
# =============================================================================================
BUCKET_RULES = [
    ("gfx", "graphics library (libdon_gs)",
     lambda name, module: module is not None and "libdon_gs" in module.basename),
    ("gamedll", "game module DLL (Windows, no symbols)",
     lambda name, module: module is not None and is_game_module_dll(module)),
    ("recomp", "recompiled game code",
     lambda name, module: GAME_ADDRESS_RE.search(name) is not None),
    ("iop", "I/O processor emulator",
     lambda name, module: "ps2x::iop::" in name),
    ("sched", "scheduler",
     lambda name, module: "EeScheduler::" in name),
    ("vu", "vector unit interpreter",
     lambda name, module: "VU1Interpreter::" in name or "VU0" in name),
    ("gif", "GIF/GS front of the runtime",
     lambda name, module: rule_gif_gs(name)),
    ("handlers", "runtime handlers",
     lambda name, module: "ps2_syscalls::" in name or "ps2_stubs::" in name),
    ("libcxx", "C and C++ libraries",
     lambda name, module: rule_c_library(name, module)),
    ("host", "host program",
     lambda name, module: rule_host(name)),
    ("runtime", "other runtime code",
     lambda name, module: rule_runtime(name)),
    ("noload", "outside the module map",
     lambda name, module: module is None),
    ("unknown", "unknown",
     lambda name, module: True),
]

# Two constants the table uses. With `nm -C` the parameter list or a nested name follows the guest
# address, so a recompiled name is not "ending in" `_0x<hex>`: it continues with `(`, `:` or ends.
# Windows profiles: the size of the module + offset groups that stand in for symbols.
WINDOWS_GROUP = 0x100
GAME_ADDRESS_RE = re.compile(r"_0x([0-9a-fA-F]+)(?=$|[:(])")
LIBRARY_NAME_RE = re.compile(
    r"^(libc[-.]|libc\+\+|libc\+\+abi|libstdc\+\+|libm[-.]|libgcc_s|libpthread|libdl|libatomic|"
    r"libubsan|libasan|libtsan|ld-linux|ld-musl|librt|libutil)"
)

# The first component of a qualified name decides host against runtime, for the two rows that need
# more than one name. Keep these lists short: what is not in either lands in "unknown", where the
# summary still shows the symbol (an anonymous-namespace helper of the runtime and one of the host
# program cannot be told apart from the symbol name alone).
HOST_COMPONENTS = {
    "HostGs", "HostAudio", "HostWindow", "AudioRing", "WavWriter", "ScreenshotWriter", "PadTable",
    "input",
}
# The host's functions without a namespace (name prefixes).
HOST_NAMES = ("don_", "main", "translatePs2Path")
RUNTIME_COMPONENTS = {
    "PS2Runtime", "PS2Memory", "PS2GS", "PS2Gs", "PS2HostGs", "PS2Vfs", "PS2RomDevice",
    "PS2RomProfile", "PS2AudioBackend", "PS2Iop", "PS2IopHostAdapter", "PS2PadBackend", "PS2Pad",
    "PS2Kr", "PS2P", "EeScheduler", "Ee", "EeKernel", "EeKernelSnapshot", "EeTimer",
    "EeDispatcherTransfer", "VU0Interpreter", "VU1Interpreter", "VU0", "VU1", "GifArbiter",
    "GifHostScanner", "GS", "GSMem", "GSCpuBackend", "SifModuleRecord", "Mips", "R5900", "Kernel",
    "Memory", "Syscalls", "ps2_syscalls", "ps2_stubs", "ps2_log", "ps2_runtime_calls",
    "ps2_game_overrides", "ps2x", "ps2xRuntime", "ps2_vag", "iop", "RunQueueManager",
}


def is_game_module_dll(module: "Module") -> bool:
    """The recompiled game as a separate DLL of the Windows build (don_game.dll): no symbols, see below."""
    base = module.basename.lower()
    return base.startswith("don_game") and base.endswith(".dll")


def name_head(name: str) -> str:
    """The name up to its argument list (`nm -C` puts the return type first for templates)."""
    return name.split("(", 1)[0]


def has_component(head: str, components: set) -> bool:
    """Whether one of the namespace or class names appears qualified in the name."""
    return any(component + "::" in head for component in components)


def rule_gif_gs(name: str) -> bool:
    """The GIF/GS front of the runtime: the arbiter, the GS classes and the memory helpers."""
    head = name_head(name)
    return ("GifArbiter::" in head or "GS::" in head
            or ("PS2Memory::" in head and ("Gif" in head or "Vif" in head)))


def rule_c_library(name: str, module: "Module | None") -> bool:
    """The C and C++ libraries: their own modules, and the compiler and standard-library machinery.

    Template instantiations are named after their return type (`void std::__merge_adaptive<...>`),
    so `std::` is looked for before the argument list, not only at the start of the name; that also
    keeps a host function whose parameters mention std:: (`don_gs_load(std::string const&, ...)`)
    out of this bucket.
    """
    if module is not None and LIBRARY_NAME_RE.match(module.basename):
        return True
    head = name_head(name)
    return ("std::" in head or "__gnu_cxx::" in head or "__cxxabiv1::" in head
            or head.startswith("__") or head.startswith("_GLOBAL__sub_I_")
            or head in ("_start", "_init", "_fini"))


def rule_host(name: str) -> bool:
    """The host program (src/host): its own types, and the few functions without a namespace."""
    head = name_head(name)
    return has_component(head, HOST_COMPONENTS) or head.startswith(HOST_NAMES)


def rule_runtime(name: str) -> bool:
    """The rest of the runtime (PS2Recomp), which lives in C++ namespaces and classes."""
    head = name_head(name)
    return has_component(head, RUNTIME_COMPONENTS) or head.startswith(("ps2", "PS2"))


# =============================================================================================
# The profile file
# =============================================================================================

_COUNT_KEYS = ("rate_hz", "duration_us", "cpu_us", "samples_taken", "samples_lost")
_IGNORED_KEYS = ("addresses", "modules", "callers")  # the following lines are the data


@dataclass(frozen=True)
class Module:
    """One executable mapping of a loaded file (a line of the profile's module map)."""

    start: int
    end: int
    offset: int
    path: str

    @property
    def basename(self) -> str:
        return self.path.replace("\\", "/").rsplit("/", 1)[-1]

    @property
    def is_windows(self) -> bool:
        """Whether the path is a Windows one (written by src/host/profile_win32.cpp): `C:\\...` or `\\\\...`."""
        return re.match(r"^([A-Za-z]:[\\/]|\\\\)", self.path) is not None

    def contains(self, address: int) -> bool:
        return self.start <= address < self.end

    def file_address(self, address: int) -> int:
        """`address` as an address inside the file (the program lands elsewhere in every run)."""
        return address - self.start + self.offset


@dataclass
class Profile:
    path: Path
    rate_hz: int = 0
    thread: str = ""
    duration_us: int = 0
    cpu_us: int = 0
    samples_taken: int = 0
    samples_lost: int = 0
    modules: list[Module] = field(default_factory=list)
    counts: list[tuple[int, int]] = field(default_factory=list)  # (address, samples)
    # Optional `callers` section (T-765): {(sampled address, return address at the top of the stack): samples}.
    callers: dict[tuple[int, int], int] = field(default_factory=dict)

    @property
    def samples(self) -> int:
        return sum(count for _, count in self.counts)

    def module_for(self, address: int) -> Module | None:
        for module in self.modules:
            if module.contains(address):
                return module
        return None


def read_profile(path: Path) -> Profile:
    """Reads the plain-text profile written by src/host/profile.cpp. Raises ValueError on junk."""
    profile = Profile(path=path)
    text = path.read_text(encoding="utf-8", errors="replace")
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        head, _, tail = line.partition(" ")
        tail = tail.strip()
        if head == "module":
            fields = line.split(" ", 4)
            if len(fields) != 5:
                raise ValueError(f"{path}:{number}: malformed module line: {raw!r}")
            try:
                profile.modules.append(Module(int(fields[1], 16), int(fields[2], 16), int(fields[3], 16), fields[4]))
            except ValueError as error:
                raise ValueError(f"{path}:{number}: malformed module line: {raw!r}") from error
        elif head.startswith("0x") and len(tail.split()) == 2:
            # `<pc> <caller> <count>`: a line of the optional `callers` section (the sample lines have two fields).
            fields = line.split()
            try:
                key = (int(fields[0], 16), int(fields[1], 16))
                profile.callers[key] = profile.callers.get(key, 0) + int(fields[2])
            except ValueError as error:
                raise ValueError(f"{path}:{number}: malformed caller line: {raw!r}") from error
        elif head.startswith("0x"):
            try:
                profile.counts.append((int(head, 16), int(tail)))
            except ValueError as error:
                raise ValueError(f"{path}:{number}: malformed sample line: {raw!r}") from error
        elif head in _COUNT_KEYS:
            try:
                setattr(profile, head, int(tail))
            except ValueError as error:
                raise ValueError(f"{path}:{number}: bad value in {raw!r}") from error
        elif head == "thread":
            profile.thread = tail
        elif head in _IGNORED_KEYS:
            pass
        else:
            raise ValueError(f"{path}:{number}: unknown line: {raw!r}")
    if not profile.rate_hz:
        raise ValueError(f"{path}: no rate_hz line: not a profile file?")
    return profile


# =============================================================================================
# Symbols
# =============================================================================================


@dataclass(frozen=True)
class Symbol:
    address: int  # address inside the module's file
    size: int
    name: str
    kind: str = "T"  # nm's letter: t/T/w/W/i/I are code, the rest is data

    @property
    def is_text(self) -> bool:
        return self.kind in "tTwWiI"


class SymbolSource:
    """Where the symbols of a module come from. The tests inject their own."""

    def symbols_for(self, module: Module) -> list[Symbol]:
        raise NotImplementedError

    def notes(self) -> list[str]:
        """What could not be read, for the summary to print."""
        return []


class NmSymbolSource(SymbolSource):
    """`nm -C --defined-only -S`, with `nm -D` for a library without a full symbol table."""

    def __init__(self, program: str | None = None):
        self.program = program or os.environ.get("DON_NM") or "nm"
        self._cache: dict[str, list[Symbol]] = {}
        self._notes: list[str] = []

    def symbols_for(self, module: Module) -> list[Symbol]:
        if module.path not in self._cache:
            self._cache[module.path] = self._read(module)
        return self._cache[module.path]

    def notes(self) -> list[str]:
        return list(self._notes)

    def _read(self, module: Module) -> list[Symbol]:
        if shutil.which(self.program) is None:
            self._note_once(f"'{self.program}' is not available: no symbols were resolved")
            return []
        if not Path(module.path).is_file():
            self._note_once(f"not on this machine: {module.path}")
            return []
        symbols = self._run(["-C", "--defined-only", "-S", module.path])
        if not symbols:
            symbols = self._run(["-D", "-C", "--defined-only", "-S", module.path])
        if not symbols:
            self._note_once(f"nm found no symbol in {module.path}")
        return symbols

    def _run(self, arguments: list[str]) -> list[Symbol]:
        try:
            result = subprocess.run([self.program, *arguments], capture_output=True, text=True, timeout=300)
        except (OSError, subprocess.SubprocessError) as error:
            self._note_once(f"nm failed: {error}")
            return []
        return parse_nm(result.stdout)

    def _note_once(self, note: str) -> None:
        if note not in self._notes:
            self._notes.append(note)


def parse_nm(text: str) -> list[Symbol]:
    """Parses `nm`'s default format: `address [size] kind name` (the name may hold spaces)."""
    symbols: list[Symbol] = []
    for line in text.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 3:
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        if len(fields) == 3:
            size, kind, name = 0, fields[1], fields[2]
        else:
            try:
                size = int(fields[1], 16)
            except ValueError:
                continue
            kind, name = fields[2], fields[3]
        if len(kind) != 1:
            continue
        symbols.append(Symbol(address, size, name.strip(), kind))
    symbols.sort(key=lambda symbol: symbol.address)
    return symbols


class DictSymbolSource(SymbolSource):
    """A symbol table given directly (the tests): {module path or basename: [(address, size, name)]}."""

    def __init__(self, tables: dict[str, list[tuple[int, int, str]]], kind: str = "T"):
        self._tables = {
            key: sorted((Symbol(address, size, name, kind) for address, size, name in entries),
                        key=lambda symbol: symbol.address)
            for key, entries in tables.items()
        }

    def symbols_for(self, module: Module) -> list[Symbol]:
        return self._tables.get(module.path, self._tables.get(module.basename, []))


# ---------------------------------------------------------------------------------------------
# From the sampled address to a virtual address: the module's program headers
# ---------------------------------------------------------------------------------------------
# Module.file_address() turns a sampled address into a FILE OFFSET (address - mapping start + the
# mapping's file offset). nm's symbol values are VIRTUAL ADDRESSES, and the two are the same only when
# the ELF segment's p_vaddr - p_offset is zero, which is what GNU ld does. lld links the port and its
# code segment has p_vaddr - p_offset = 0x1000, so looking a file offset up directly names whatever
# lies 4 KiB before the instruction that was sampled (that is what made loadImage, publishSnapshot and
# the log's deque look hot in the first reports). The correct translation finds the PT_LOAD segment
# whose file range contains the offset: vaddr = file_offset - p_offset + p_vaddr.

PT_LOAD = 1
ELF_MAGIC = b"\x7fELF"


@dataclass(frozen=True)
class Segment:
    offset: int  # p_offset
    vaddr: int   # p_vaddr
    size: int    # p_filesz

    def contains(self, file_offset: int) -> bool:
        return self.offset <= file_offset < self.offset + self.size

    def virtual_address(self, file_offset: int) -> int:
        return file_offset - self.offset + self.vaddr


class SegmentSource:
    """Where the program headers of a module come from. The tests inject their own."""

    def segments_for(self, module: Module) -> list[Segment]:
        raise NotImplementedError

    def notes(self) -> list[str]:
        return []


class ElfSegmentSource(SegmentSource):
    """The PT_LOAD segments of a module's file, read with the standard library.

    Only ELF64 little endian is parsed, which is what this project builds. Any other input (ELF32, a
    file that is not an ELF, one that is missing) yields no segments and a note; the translation then
    falls back to the file offset itself, which is the old behaviour and is right for the linkers whose
    p_vaddr equals p_offset.
    """

    def __init__(self):
        self._cache: dict[str, list[Segment]] = {}
        self._notes: list[str] = []

    def notes(self) -> list[str]:
        return list(self._notes)

    def segments_for(self, module: Module) -> list[Segment]:
        if module.path not in self._cache:
            self._cache[module.path] = self._read(module)
        return self._cache[module.path]

    def _read(self, module: Module) -> list[Segment]:
        path = Path(module.path)
        try:
            data = path.read_bytes() if path.is_file() else b""
        except OSError:
            data = b""
        if not data:
            return []
        if len(data) < 64 or data[:4] != ELF_MAGIC:
            self._note_once(f"{module.path}: not an ELF file: symbols are looked up by file offset")
            return []
        if data[4] != 2 or data[5] != 1:  # EI_CLASS, EI_DATA: only 64-bit little endian
            self._note_once(f"{module.path}: not ELF64 little endian: symbols are looked up by file offset")
            return []
        e_phoff, = struct.unpack_from("<Q", data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
        segments: list[Segment] = []
        for index in range(e_phnum):
            base = e_phoff + index * e_phentsize
            if base + 56 > len(data):
                break
            p_type, = struct.unpack_from("<I", data, base)
            p_offset, p_vaddr, _p_paddr, p_filesz, _p_memsz, _p_align = struct.unpack_from("<QQQQQQ", data, base + 8)
            if p_type != PT_LOAD or p_filesz == 0:
                continue
            segments.append(Segment(p_offset, p_vaddr, p_filesz))
        if not segments:
            self._note_once(f"{module.path}: no PT_LOAD segment with content")
        return segments

    def _note_once(self, note: str) -> None:
        if note not in self._notes:
            self._notes.append(note)


class DictSegmentSource(SegmentSource):
    """Segments given directly (the tests): {module path or basename: [(p_offset, p_vaddr, p_filesz)]}."""

    def __init__(self, tables: dict[str, list[tuple[int, int, int]]]):
        self._tables = {key: [Segment(*entry) for entry in entries] for key, entries in tables.items()}

    def segments_for(self, module: Module) -> list[Segment]:
        return self._tables.get(module.path, self._tables.get(module.basename, []))


def virtual_address_of(module: Module, address: int, segments: list[Segment]) -> int:
    """The virtual address of a sampled address: its file offset through the segment that holds it."""
    file_offset = module.file_address(address)
    for segment in segments:
        if segment.contains(file_offset):
            return segment.virtual_address(file_offset)
    return file_offset  # no segment knows it: the file offset is all there is


def stale_module_notes(profile: Profile) -> list[str]:
    """One note per module whose file is newer than the profile: the symbols may not match it."""
    notes: list[str] = []
    try:
        profile_time = profile.path.stat().st_mtime
    except OSError:
        return notes
    for module in profile.modules:
        try:
            if Path(module.path).stat().st_mtime > profile_time:
                notes.append(f"symbols may not match: {module.path} is newer than the profile")
        except OSError:
            continue
    return notes


def find_symbol(symbols: list[Symbol], address: int, addresses: list[int] | None = None) -> Symbol | None:
    """The text symbol that covers `address` (a VIRTUAL address, as nm prints them), or None.

    The last symbol at or before the address decides: a data symbol there means the address is not
    code; a text symbol with a size covers only up to its end, so an address in a gap between two
    sized symbols is in no symbol; a text symbol without a size (nm prints none for some of them)
    covers everything up to the next one.
    """
    if not symbols:
        return None
    if addresses is None:
        addresses = [symbol.address for symbol in symbols]
    index = bisect.bisect_right(addresses, address) - 1
    if index < 0:
        return None
    symbol = symbols[index]
    if not symbol.is_text:
        return None
    if symbol.size and address >= symbol.address + symbol.size:
        return None
    return symbol


# =============================================================================================
# The summary
# =============================================================================================


def bucket_of(name: str, module: Module | None) -> str:
    """The key of the first row of BUCKET_RULES that matches."""
    for key, _, test in BUCKET_RULES:
        if test(name, module):
            return key
    return "unknown"  # unreachable: the last rule always matches


BUCKET_LABELS = {key: label for key, label, _ in BUCKET_RULES}
BUCKET_ORDER = [key for key, _, _ in BUCKET_RULES]


@dataclass
class SymbolCount:
    name: str  # "" for a sample in no symbol
    module: Module | None
    samples: int
    bucket: str


@dataclass
class GameFunction:
    guest_address: int
    samples: int
    symbol: str
    name: str  # from the functions CSV when one was given, else the symbol name


@dataclass
class Summary:
    profile: Profile
    buckets: dict[str, int]
    symbols: list[SymbolCount]
    game: list[GameFunction]
    in_module: int
    with_symbol: int
    notes: list[str] = field(default_factory=list)
    # T-765: for each of the 10 most sampled addresses, its callers: (address name, samples, [(caller name, samples)]).
    caller_rows: list[tuple[str, int, list[tuple[str, int]]]] = field(default_factory=list)

    @property
    def samples(self) -> int:
        return self.profile.samples


def resolve_symbol(module: Module, address: int, source: SymbolSource, segments: SegmentSource,
                   tables: dict[str, tuple[list[Symbol], list[int]]],
                   segment_cache: dict[str, list[Segment]]) -> tuple[Symbol | None, bool]:
    """Finds the symbol of a non-Windows `address` in `module`; the bool tells whether its file offset
    lies in a known segment (False: the lookup used the raw file offset, see `analyze`).

    `tables` and `segment_cache` are per-analysis caches keyed by module path, filled on first use.
    """
    if module.path not in tables:
        symbols = source.symbols_for(module)
        tables[module.path] = (symbols, [entry.address for entry in symbols])
    if module.path not in segment_cache:
        segment_cache[module.path] = segments.segments_for(module)
    module_segments = segment_cache[module.path]
    virtual_address = virtual_address_of(module, address, module_segments)
    inside_segment = any(segment.contains(module.file_address(address)) for segment in module_segments)
    symbols, addresses = tables[module.path]
    return find_symbol(symbols, virtual_address, addresses), inside_segment


def analyze(profile: Profile, source: SymbolSource, functions: dict[int, str] | None = None,
            segments: SegmentSource | None = None) -> Summary:
    """Resolves every sampled address and counts the samples per subsystem, symbol and game function.

    `segments` translates a sampled address's file offset into the virtual address that the symbol
    table uses (see the Segment comment above); without it the file offset is used directly.
    """
    buckets = {key: 0 for key in BUCKET_ORDER}
    symbol_totals: dict[tuple[str, str], SymbolCount] = {}
    game_totals: dict[int, GameFunction] = {}
    tables: dict[str, tuple[list[Symbol], list[int]]] = {}
    segment_cache: dict[str, list[Segment]] = {}
    in_module = 0
    with_symbol = 0
    no_segment = 0
    functions = functions or {}
    segments = segments or DictSegmentSource({})

    for address, count in profile.counts:
        module = profile.module_for(address)
        symbol = None
        if module is not None and module.is_windows:
            # A Windows profile (T-759): module lines are whole images with offset 0, so the offset is
            # the RVA. The DLL has no symbols and the files are rarely on this machine: samples are
            # grouped by module + offset (WINDOWS_GROUP bytes) and the name says which.
            in_module += count
            with_symbol += count
            name = f"{module.basename}+0x{(module.file_address(address) // WINDOWS_GROUP) * WINDOWS_GROUP:x}"
            key = bucket_of(name, module)
            buckets[key] += count
            entry = symbol_totals.get((module.path, name))
            if entry is None:
                entry = SymbolCount(name=name, module=module, samples=0, bucket=key)
                symbol_totals[(module.path, name)] = entry
            entry.samples += count
            continue
        if module is not None:
            in_module += count
            symbol, inside_segment = resolve_symbol(module, address, source, segments, tables, segment_cache)
            if not inside_segment:
                no_segment += count
        name = symbol.name if symbol is not None else ""
        if symbol is not None:
            with_symbol += count
        key = bucket_of(name, module)
        buckets[key] += count

        entry = symbol_totals.get((module.path if module else "", name))
        if entry is None:
            entry = SymbolCount(name=name, module=module, samples=0, bucket=key)
            symbol_totals[(module.path if module else "", name)] = entry
        entry.samples += count

        if key == "recomp":
            match = GAME_ADDRESS_RE.search(name)
            guest = int(match.group(1), 16) if match else 0
            game = game_totals.get(guest)
            if game is None:
                game = GameFunction(guest_address=guest, samples=0, symbol=name, name=functions.get(guest, name))
                game_totals[guest] = game
            game.samples += count

    caller_rows = []
    if profile.callers:
        def describe(address: int) -> str:
            module = profile.module_for(address)
            if module is None:
                return f"0x{address:x} (no module)"
            if module.is_windows:
                return f"{module.basename}+0x{module.file_address(address):x}"
            symbol_here, _ = resolve_symbol(module, address, source, segments, tables, segment_cache)
            return f"{symbol_here.name if symbol_here else '(no symbol)'} [{module.basename}]"

        top_pcs = sorted(profile.counts, key=lambda entry: (-entry[1], entry[0]))[:10]
        for pc, pc_samples in top_pcs:
            rows = {}
            for (row_pc, caller), count in profile.callers.items():
                if row_pc == pc:
                    rows[describe(caller)] = rows.get(describe(caller), 0) + count
            ordered = sorted(rows.items(), key=lambda item: (-item[1], item[0]))
            caller_rows.append((describe(pc), pc_samples, ordered))

    symbols = sorted(symbol_totals.values(), key=lambda entry: (-entry.samples, entry.name))
    game = sorted(game_totals.values(), key=lambda entry: (-entry.samples, entry.guest_address))
    notes = list(source.notes()) + list(segments.notes()) + stale_module_notes(profile)
    if no_segment:
        notes.append(f"{no_segment} sample(s) have a file offset outside every known segment of their "
                     f"module: their symbol is looked up at the raw file offset")
    return Summary(profile=profile, buckets=buckets, symbols=symbols, game=game, in_module=in_module,
                   with_symbol=with_symbol, notes=notes, caller_rows=caller_rows)


def load_functions(path: Path) -> dict[int, str]:
    """config/functions.ghidra.csv (Name,Start,End,Size) as {guest start address: name}."""
    functions: dict[int, str] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                start = int(row["Start"], 0)
            except (KeyError, TypeError, ValueError):
                continue
            functions[start] = row.get("Name") or f"0x{start:08x}"
    return functions


def format_summary(summary: Summary, top: int) -> str:
    """The whole report, as plain text."""
    profile = summary.profile
    total = summary.samples
    share = (lambda value: (100.0 * value / total) if total else 0.0)
    expected = profile.rate_hz * profile.cpu_us / 1e6

    lines: list[str] = []
    lines.append(f"profile {profile.path}")
    lines.append(f"thread {profile.thread}  rate {profile.rate_hz} Hz  wall {profile.duration_us / 1e6:.3f} s  "
                 f"thread cpu {profile.cpu_us / 1e6:.3f} s")
    lines.append(f"samples {profile.samples_taken} stored, {profile.samples_lost} lost; "
                 f"{len(profile.counts)} distinct addresses; {expected:.0f} expected from the thread's "
                 f"processor time")
    lines.append(f"resolved: in a module {summary.in_module} ({share(summary.in_module):.1f}%), "
                 f"with a symbol {summary.with_symbol} ({share(summary.with_symbol):.1f}%); "
                 f"outside every module {summary.samples - summary.in_module} "
                 f"({share(summary.samples - summary.in_module):.1f}%)")
    lines.append("")

    lines.append(f"{'subsystem':<40}{'samples':>10}{'share':>9}")
    for key in BUCKET_ORDER:
        lines.append(f"{BUCKET_LABELS[key]:<40}{summary.buckets[key]:>10}{share(summary.buckets[key]):>8.1f}%")
    lines.append(f"{'total':<40}{total:>10}{share(total):>8.1f}%")
    lines.append("")

    lines.append(f"top {top} symbols (by samples; share and cumulative share)")
    lines.append(f"{'samples':>10}{'share':>8}{'cum':>8}  symbol")
    cumulative = 0
    for entry in summary.symbols[:top]:
        cumulative += entry.samples
        name = entry.name or "(no symbol)"
        if entry.module is not None:
            name += f"  [{entry.module.basename}]"
        lines.append(f"{entry.samples:>10}{share(entry.samples):>7.1f}%{share(cumulative):>7.1f}%  {name}")
    if not summary.symbols:
        lines.append("(none)")
    lines.append("")

    lines.append(f"top {top} game functions (recompiled game code, by samples)")
    lines.append(f"{'samples':>10}{'share':>8}{'cum':>8}  guest       name")
    cumulative = 0
    for entry in summary.game[:top]:
        cumulative += entry.samples
        lines.append(f"{entry.samples:>10}{share(entry.samples):>7.1f}%{share(cumulative):>7.1f}%  "
                     f"0x{entry.guest_address:08x}  {entry.name}")
    if not summary.game:
        lines.append("(no samples in recompiled game code)")

    if summary.caller_rows:
        lines.append("")
        lines.append("callers of the top 10 addresses (return address at the top of the stack when sampled)")
        for pc_name, pc_samples, callers in summary.caller_rows:
            known = sum(count for _, count in callers)
            lines.append(f"{pc_samples:>10}  {pc_name}  ({known} of {pc_samples} with a caller)")
            for caller_name, count in callers[:5]:
                lines.append(f"{count:>14}  <- {caller_name}")

    if summary.notes:
        lines.append("")
        lines.append("notes")
        for note in summary.notes:
            lines.append(f"  {note}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Summarise a profile written by the port (T-203).")
    parser.add_argument("profile", type=Path, help="the file written with DON_PROFILE=<file>")
    parser.add_argument("--top", type=int, default=30, help="how many symbols and game functions to list (default 30)")
    parser.add_argument("--csv", type=Path, default=None,
                        help="config/functions.ghidra.csv, to name the game functions (optional)")
    parser.add_argument("--nm", default=None, help="the nm program to use (default $DON_NM, else nm)")
    arguments = parser.parse_args(argv)

    if not arguments.profile.is_file():
        print(f"profile_summary: no such file: {arguments.profile}", file=sys.stderr)
        return 2
    try:
        profile = read_profile(arguments.profile)
    except ValueError as error:
        print(f"profile_summary: {error}", file=sys.stderr)
        return 2

    functions: dict[int, str] = {}
    if arguments.csv is not None:
        if not arguments.csv.is_file():
            print(f"profile_summary: no such CSV: {arguments.csv}", file=sys.stderr)
            return 2
        functions = load_functions(arguments.csv)

    summary = analyze(profile, NmSymbolSource(arguments.nm), functions, ElfSegmentSource())
    print(format_summary(summary, max(1, arguments.top)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
